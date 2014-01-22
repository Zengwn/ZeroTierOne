/*
 * ZeroTier One - Global Peer to Peer Ethernet
 * Copyright (C) 2012-2013  ZeroTier Networks LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * ZeroTier may be used and distributed under the terms of the GPLv3, which
 * are available at: http://www.gnu.org/licenses/gpl-3.0.html
 *
 * If you would like to embed ZeroTier into a commercial application or
 * redistribute it in a modified binary form, please contact ZeroTier Networks
 * LLC. Start here: http://www.zerotier.com/
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include <string>
#include <stdexcept>

#include "node/Constants.hpp"

#ifdef __WINDOWS__
#include <WinSock2.h>
#include <Windows.h>
#include <tchar.h>
#include <wchar.h>
#include <lmcons.h>
#else
#include <unistd.h>
#include <pwd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#endif

#include "node/Constants.hpp"
#include "node/Defaults.hpp"
#include "node/Utils.hpp"
#include "node/Node.hpp"
#include "node/Condition.hpp"
#include "node/C25519.hpp"
#include "node/Identity.hpp"

using namespace ZeroTier;

static Node *node = (Node *)0;

static void printHelp(const char *cn,FILE *out)
{
	fprintf(out,"ZeroTier One version %d.%d.%d"ZT_EOL_S"(c)2012-2013 ZeroTier Networks LLC"ZT_EOL_S,Node::versionMajor(),Node::versionMinor(),Node::versionRevision());
	fprintf(out,"Licensed under the GNU General Public License v3"ZT_EOL_S""ZT_EOL_S);
#ifdef ZT_AUTO_UPDATE
	fprintf(out,"Auto-update enabled build, will update from URL:"ZT_EOL_S);
	fprintf(out,"  %s"ZT_EOL_S,ZT_DEFAULTS.updateLatestNfoURL.c_str());
	fprintf(out,"Update authentication signing authorities: "ZT_EOL_S);
	int no = 0;
	for(std::map< Address,Identity >::const_iterator sa(ZT_DEFAULTS.updateAuthorities.begin());sa!=ZT_DEFAULTS.updateAuthorities.end();++sa) {
		if (no == 0)
			fprintf(out,"  %s",sa->first.toString().c_str());
		else fprintf(out,", %s",sa->first.toString().c_str());
		if (++no == 6) {
			fprintf(out,ZT_EOL_S);
			no = 0;
		}
	}
	fprintf(out,ZT_EOL_S""ZT_EOL_S);
#else
	fprintf(out,"Auto-updates not enabled on this build. You must update manually."ZT_EOL_S""ZT_EOL_S);
#endif
	fprintf(out,"Usage: %s [-switches] [home directory]"ZT_EOL_S""ZT_EOL_S,cn);
	fprintf(out,"Available switches:"ZT_EOL_S);
	fprintf(out,"  -h                - Display this help"ZT_EOL_S);
	fprintf(out,"  -v                - Show version"ZT_EOL_S);
	fprintf(out,"  -p<port>          - Bind to this port for network I/O"ZT_EOL_S);
	fprintf(out,"  -c<port>          - Bind to this port for local control packets"ZT_EOL_S);
	fprintf(out,"  -q                - Send a query to a running service (zerotier-cli)"ZT_EOL_S);
	fprintf(out,"  -i                - Run idtool command (zerotier-idtool)"ZT_EOL_S);
}

namespace ZeroTierCLI { // ---------------------------------------------------

static void printHelp(FILE *out,const char *exename)
{
	fprintf(out,"Usage: %s [-switches] <command>"ZT_EOL_S,exename);
	fprintf(out,ZT_EOL_S);
	fprintf(out,"Available switches:"ZT_EOL_S);
	fprintf(out,"  -c<port>         - Communicate with daemon over this local port"ZT_EOL_S);
	fprintf(out,"  -t<token>        - Specify token on command line"ZT_EOL_S);
	fprintf(out,"  -T<file>         - Read token from file"ZT_EOL_S);
	fprintf(out,ZT_EOL_S);
	fprintf(out,"Use the 'help' command to get help from ZeroTier One itself."ZT_EOL_S);
}

static volatile unsigned int numResults = 0;
static Condition doneCondition;

static void resultHandler(void *arg,unsigned long id,const char *line)
{
	++numResults;
	if (strlen(line))
		fprintf(stdout,"%s"ZT_EOL_S,line);
	else doneCondition.signal();
}

// Runs instead of rest of main() if process is called zerotier-cli or if
// -q is specified as an option.
#ifdef __WINDOWS__
static int main(int argc,_TCHAR* argv[])
#else
static int main(int argc,char **argv)
#endif
{
	if (argc <= 1) {
		printHelp(stdout,argv[0]);
		return -1;
	}

	std::string authToken;
	std::string command;
	bool pastSwitches = false;
	unsigned int controlPort = 0;
	for(int i=1;i<argc;++i) {
		if ((argv[i][0] == '-')&&(!pastSwitches)) {
			if (strlen(argv[i]) <= 1) {
				printHelp(stdout,argv[0]);
				return -1;
			}
			switch(argv[i][1]) {
				case 'q': // does nothing, for invocation without binary path name aliasing
					if (argv[i][2]) {
						printHelp(argv[0],stderr);
						return 0;
					}
					break;
				case 'c':
					controlPort = Utils::strToUInt(argv[i] + 2);
					break;
				case 't':
					authToken.assign(argv[i] + 2);
					break;
				case 'T':
					if (!Utils::readFile(argv[i] + 2,authToken)) {
						fprintf(stdout,"FATAL ERROR: unable to read token from '%s'"ZT_EOL_S,argv[i] + 2);
						return -2;
					}
					break;
				case 'h':
					printHelp(stdout,argv[0]);
					return 0;
				default:
					return -1;
			}
		} else {
			pastSwitches = true;
			if (command.length())
				command.push_back(' ');
			command.append(argv[i]);
		}
	}

	if (!command.length()) {
		printHelp(stdout,argv[0]);
		return -1;
	}

	if (!authToken.length()) {
		if (!Utils::readFile(Node::LocalClient::authTokenDefaultUserPath().c_str(),authToken)) {
			if (!Utils::readFile(Node::LocalClient::authTokenDefaultSystemPath().c_str(),authToken)) {
				fprintf(stdout,"FATAL ERROR: no token specified on command line and could not read '%s' or '%s'"ZT_EOL_S,Node::LocalClient::authTokenDefaultSystemPath().c_str(),Node::LocalClient::authTokenDefaultUserPath().c_str());
				return -2;
			}
		}
	}
	if (!authToken.length()) {
		fprintf(stdout,"FATAL ERROR: could not find auth token"ZT_EOL_S);
		return -2;
	}

	Node::LocalClient client(authToken.c_str(),controlPort,&resultHandler,(void *)0);
	client.send(command.c_str());

	doneCondition.wait(1000);

	if (!numResults) {
		fprintf(stdout,"ERROR: no results received. Is ZeroTier One running?"ZT_EOL_S);
		return -1;
	}

	return 0;
}

} // namespace ZeroTierCLI ---------------------------------------------------

namespace ZeroTierIdTool { // ------------------------------------------------

static void printHelp(FILE *out,const char *pn)
{
	fprintf(out,"Usage: %s <command> [<args>]"ZT_EOL_S""ZT_EOL_S"Commands:"ZT_EOL_S,pn);
	fprintf(out,"  generate [<identity.secret>] [<identity.public>]"ZT_EOL_S);
	fprintf(out,"  validate <identity.secret/public>"ZT_EOL_S);
	fprintf(out,"  getpublic <identity.secret>"ZT_EOL_S);
	fprintf(out,"  sign <identity.secret> <file>"ZT_EOL_S);
	fprintf(out,"  verify <identity.secret/public> <file> <signature>"ZT_EOL_S);
}

static Identity getIdFromArg(char *arg)
{
	Identity id;
	if ((strlen(arg) > 32)&&(arg[10] == ':')) { // identity is a literal on the command line
		if (id.fromString(arg))
			return id;
	} else { // identity is to be read from a file
		std::string idser;
		if (Utils::readFile(arg,idser)) {
			if (id.fromString(idser))
				return id;
		}
	}
	return Identity();
}

// Runs instead of rest of main() if process is called zerotier-idtool or if
// -i is specified as an option.
#ifdef __WINDOWS__
static int main(int argc,_TCHAR* argv[])
#else
static int main(int argc,char **argv)
#endif
{
	if (argc < 2) {
		printHelp(stderr,argv[0]);
		return -1;
	}

	if (!strcmp(argv[1],"generate")) {
		Identity id;
		id.generate();
		std::string idser = id.toString(true);
		if (argc >= 3) {
			if (!Utils::writeFile(argv[2],idser)) {
				fprintf(stderr,"Error writing to %s"ZT_EOL_S,argv[2]);
				return -1;
			} else printf("%s written"ZT_EOL_S,argv[2]);
			if (argc >= 4) {
				idser = id.toString(false);
				if (!Utils::writeFile(argv[3],idser)) {
					fprintf(stderr,"Error writing to %s"ZT_EOL_S,argv[3]);
					return -1;
				} else printf("%s written"ZT_EOL_S,argv[3]);
			}
		} else printf("%s",idser.c_str());
	} else if (!strcmp(argv[1],"validate")) {
		if (argc < 3) {
			printHelp(stderr,argv[0]);
			return -1;
		}

		Identity id = getIdFromArg(argv[2]);
		if (!id) {
			fprintf(stderr,"Identity argument invalid or file unreadable: %s"ZT_EOL_S,argv[2]);
			return -1;
		}

		if (!id.locallyValidate()) {
			fprintf(stderr,"%s FAILED validation."ZT_EOL_S,argv[2]);
			return -1;
		} else printf("%s is a valid identity"ZT_EOL_S,argv[2]);
	} else if (!strcmp(argv[1],"getpublic")) {
		if (argc < 3) {
			printHelp(stderr,argv[0]);
			return -1;
		}

		Identity id = getIdFromArg(argv[2]);
		if (!id) {
			fprintf(stderr,"Identity argument invalid or file unreadable: %s"ZT_EOL_S,argv[2]);
			return -1;
		}

		printf("%s",id.toString(false).c_str());
	} else if (!strcmp(argv[1],"sign")) {
		if (argc < 4) {
			printHelp(stderr,argv[0]);
			return -1;
		}

		Identity id = getIdFromArg(argv[2]);
		if (!id) {
			fprintf(stderr,"Identity argument invalid or file unreadable: %s"ZT_EOL_S,argv[2]);
			return -1;
		}

		if (!id.hasPrivate()) {
			fprintf(stderr,"%s does not contain a private key (must use private to sign)"ZT_EOL_S,argv[2]);
			return -1;
		}

		std::string inf;
		if (!Utils::readFile(argv[3],inf)) {
			fprintf(stderr,"%s is not readable"ZT_EOL_S,argv[3]);
			return -1;
		}
		C25519::Signature signature = id.sign(inf.data(),(unsigned int)inf.length());
		printf("%s",Utils::hex(signature.data,(unsigned int)signature.size()).c_str());
	} else if (!strcmp(argv[1],"verify")) {
		if (argc < 4) {
			printHelp(stderr,argv[0]);
			return -1;
		}

		Identity id = getIdFromArg(argv[2]);
		if (!id) {
			fprintf(stderr,"Identity argument invalid or file unreadable: %s"ZT_EOL_S,argv[2]);
			return -1;
		}

		std::string inf;
		if (!Utils::readFile(argv[3],inf)) {
			fprintf(stderr,"%s is not readable"ZT_EOL_S,argv[3]);
			return -1;
		}

		std::string signature(Utils::unhex(argv[4]));
		if ((signature.length() > ZT_ADDRESS_LENGTH)&&(id.verify(inf.data(),(unsigned int)inf.length(),signature.data(),(unsigned int)signature.length()))) {
			printf("%s signature valid"ZT_EOL_S,argv[3]);
		} else {
			fprintf(stderr,"%s signature check FAILED"ZT_EOL_S,argv[3]);
			return -1;
		}
	} else {
		printHelp(stderr,argv[0]);
		return -1;
	}

	return 0;
}


} // namespace ZeroTierIdTool ------------------------------------------------

#ifdef __UNIX_LIKE__
static void sighandlerQuit(int sig)
{
	Node *n = node;
	if (n)
		n->terminate(Node::NODE_NORMAL_TERMINATION,"terminated by signal");
	else exit(0);
}
#endif

#ifdef __WINDOWS__
static BOOL WINAPI _handlerRoutine(DWORD dwCtrlType)
{
	switch(dwCtrlType) {
		case CTRL_C_EVENT:
		case CTRL_BREAK_EVENT:
		case CTRL_CLOSE_EVENT:
		case CTRL_SHUTDOWN_EVENT:
			Node *n = node;
			if (n)
				n->terminate(Node::NODE_NORMAL_TERMINATION,"terminated by signal");
			return TRUE;
	}
	return FALSE;
}

static BOOL IsCurrentUserLocalAdministrator(void)
{
   BOOL   fReturn         = FALSE;
   DWORD  dwStatus;
   DWORD  dwAccessMask;
   DWORD  dwAccessDesired;
   DWORD  dwACLSize;
   DWORD  dwStructureSize = sizeof(PRIVILEGE_SET);
   PACL   pACL            = NULL;
   PSID   psidAdmin       = NULL;

   HANDLE hToken              = NULL;
   HANDLE hImpersonationToken = NULL;

   PRIVILEGE_SET   ps;
   GENERIC_MAPPING GenericMapping;

   PSECURITY_DESCRIPTOR     psdAdmin           = NULL;
   SID_IDENTIFIER_AUTHORITY SystemSidAuthority = SECURITY_NT_AUTHORITY;


   /*
      Determine if the current thread is running as a user that is a member 

of
      the local admins group.  To do this, create a security descriptor 

that
      has a DACL which has an ACE that allows only local aministrators 

access.
      Then, call AccessCheck with the current thread's token and the 

security
      descriptor.  It will say whether the user could access an object if 

it
      had that security descriptor.  Note: you do not need to actually 

create
      the object.  Just checking access against the security descriptor 

alone
      will be sufficient.
   */
   const DWORD ACCESS_READ  = 1;
   const DWORD ACCESS_WRITE = 2;


   __try
   {

      /*
         AccessCheck() requires an impersonation token.  We first get a 

primary
         token and then create a duplicate impersonation token.  The
         impersonation token is not actually assigned to the thread, but is
         used in the call to AccessCheck.  Thus, this function itself never
         impersonates, but does use the identity of the thread.  If the 

thread
         was impersonating already, this function uses that impersonation 

context.
      */
      if (!OpenThreadToken(GetCurrentThread(), TOKEN_DUPLICATE|TOKEN_QUERY, 

TRUE, &hToken))
      {
         if (GetLastError() != ERROR_NO_TOKEN)
            __leave;

         if (!OpenProcessToken(GetCurrentProcess(), 

TOKEN_DUPLICATE|TOKEN_QUERY, &hToken))
            __leave;
      }

      if (!DuplicateToken (hToken, SecurityImpersonation, 

&hImpersonationToken))
          __leave;


      /*
        Create the binary representation of the well-known SID that
        represents the local administrators group.  Then create the 

security
        descriptor and DACL with an ACE that allows only local admins 

access.
        After that, perform the access check.  This will determine whether
        the current user is a local admin.
      */
      if (!AllocateAndInitializeSid(&SystemSidAuthority, 2,
                                    SECURITY_BUILTIN_DOMAIN_RID,
                                    DOMAIN_ALIAS_RID_ADMINS,
                                    0, 0, 0, 0, 0, 0, &psidAdmin))
         __leave;

      psdAdmin = LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
      if (psdAdmin == NULL)
         __leave;

      if (!InitializeSecurityDescriptor(psdAdmin, 

SECURITY_DESCRIPTOR_REVISION))
         __leave;

      // Compute size needed for the ACL.
      dwACLSize = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) +
                  GetLengthSid(psidAdmin) - sizeof(DWORD);

      pACL = (PACL)LocalAlloc(LPTR, dwACLSize);
      if (pACL == NULL)
         __leave;

      if (!InitializeAcl(pACL, dwACLSize, ACL_REVISION2))
         __leave;

      dwAccessMask= ACCESS_READ | ACCESS_WRITE;

      if (!AddAccessAllowedAce(pACL, ACL_REVISION2, dwAccessMask, 

psidAdmin))
         __leave;

      if (!SetSecurityDescriptorDacl(psdAdmin, TRUE, pACL, FALSE))
         __leave;

      /*
         AccessCheck validates a security descriptor somewhat; set the 

group
         and owner so that enough of the security descriptor is filled out 

to
         make AccessCheck happy.
      */
      SetSecurityDescriptorGroup(psdAdmin, psidAdmin, FALSE);
      SetSecurityDescriptorOwner(psdAdmin, psidAdmin, FALSE);

      if (!IsValidSecurityDescriptor(psdAdmin))
         __leave;

      dwAccessDesired = ACCESS_READ;

      /*
         Initialize GenericMapping structure even though you
         do not use generic rights.
      */
      GenericMapping.GenericRead    = ACCESS_READ;
      GenericMapping.GenericWrite   = ACCESS_WRITE;
      GenericMapping.GenericExecute = 0;
      GenericMapping.GenericAll     = ACCESS_READ | ACCESS_WRITE;

      if (!AccessCheck(psdAdmin, hImpersonationToken, dwAccessDesired,
                       &GenericMapping, &ps, &dwStructureSize, &dwStatus,
                       &fReturn))
      {
         fReturn = FALSE;
         __leave;
      }
   }
   __finally
   {
      // Clean up.
      if (pACL) LocalFree(pACL);
      if (psdAdmin) LocalFree(psdAdmin);
      if (psidAdmin) FreeSid(psidAdmin);
      if (hImpersonationToken) CloseHandle (hImpersonationToken);
      if (hToken) CloseHandle (hToken);
   }

   return fReturn;
}
#endif // __WINDOWS__

#ifdef __WINDOWS__
int _tmain(int argc, _TCHAR* argv[])
#else
int main(int argc,char **argv)
#endif
{
#ifdef __UNIX_LIKE__
	signal(SIGHUP,SIG_IGN);
	signal(SIGPIPE,SIG_IGN);
	signal(SIGUSR1,SIG_IGN);
	signal(SIGUSR2,SIG_IGN);
	signal(SIGALRM,SIG_IGN);
	signal(SIGINT,&sighandlerQuit);
	signal(SIGTERM,&sighandlerQuit);
	signal(SIGQUIT,&sighandlerQuit);
#endif

#ifdef __WINDOWS__
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2,2),&wsaData);
	SetConsoleCtrlHandler(&_handlerRoutine,TRUE);
#endif

	if ((strstr(argv[0],"zerotier-cli"))||(strstr(argv[0],"ZEROTIER-CLI")))
		return ZeroTierCLI::main(argc,argv);
	if ((strstr(argv[0],"zerotier-idtool"))||(strstr(argv[0],"ZEROTIER-IDTOOL")))
		return ZeroTierIdTool::main(argc,argv);

	const char *homeDir = (const char *)0;
	unsigned int port = 0;
	unsigned int controlPort = 0;
	for(int i=1;i<argc;++i) {
		if (argv[i][0] == '-') {
			switch(argv[i][1]) {
				case 'p':
					port = Utils::strToUInt(argv[i] + 2);
					if (port > 65535) {
						printHelp(argv[0],stderr);
						return 1;
					}
					break;
				case 'v':
					printf("%s"ZT_EOL_S,Node::versionString());
					return 0;
				case 'c':
					controlPort = Utils::strToUInt(argv[i] + 2);
					if (controlPort > 65535) {
						printHelp(argv[0],stderr);
						return 1;
					}
					break;
				case 'q':
					if (argv[i][2]) {
						printHelp(argv[0],stderr);
						return 0;
					} else return ZeroTierCLI::main(argc,argv);
				case 'i':
					if (argv[i][2]) {
						printHelp(argv[0],stderr);
						return 0;
					} else return ZeroTierIdTool::main(argc,argv);
				case 'h':
				case '?':
				default:
					printHelp(argv[0],stderr);
					return 0;
			}
		} else {
			if (homeDir) {
				printHelp(argv[0],stderr);
				return 0;
			}
			homeDir = argv[i];
			break;
		}
	}

	if ((!homeDir)||(strlen(homeDir) == 0))
		homeDir = ZT_DEFAULTS.defaultHomePath.c_str();

#ifdef __UNIX_LIKE__
	if (getuid()) {
		fprintf(stderr,"%s: must be run as root (uid==0)\n",argv[0]);
		return 1;
	}
	mkdir(homeDir,0755); // will fail if it already exists
	{
		char pidpath[4096];
		Utils::snprintf(pidpath,sizeof(pidpath),"%s/zerotier-one.pid",homeDir);
		FILE *pf = fopen(pidpath,"w");
		if (pf) {
			fprintf(pf,"%ld",(long)getpid());
			fclose(pf);
		}
	}
#else
#ifdef __WINDOWS__
	if (IsCurrentUserLocalAdministrator() != TRUE) {
		fprintf(stderr,"%s: must be run as a local administrator."ZT_EOL_S,argv[0]);
		return 1;
	}
#endif
#endif

	int exitCode = 0;

	try {
		node = new Node(homeDir,port,controlPort);
		switch(node->run()) {
			case Node::NODE_RESTART_FOR_UPGRADE: {
				const char *upgPath = node->reasonForTermination();
#ifdef __UNIX_LIKE__
				// On Unix-type OSes we exec() right into the upgrade. This in turn will
				// end with us being re-launched either via the upgrade itself or something
				// like OSX's launchd.
				if (upgPath) {
					Utils::rm((std::string(homeDir)+"/zerotier-one.pid").c_str());
					::execl(upgPath,upgPath,(char *)0);
				}
				exitCode = 2;
				fprintf(stderr,"%s: abnormal termination: unable to execute update at %s\n",argv[0],(upgPath) ? upgPath : "(unknown path)");
#else // not __UNIX_LIKE
#ifdef __WINDOWS__
				// On Windows the service checks updates.d and invokes updates if they are
				// found there. This only happens after exit code 4. The Windows service
				// will listen to stdout as well to catch the filename.
				if (upgPath) {
					printf("[[[ UPDATE AVAILABLE: \"%s\" ]]]\r\n",upgPath);
					exitCode = 4;
				} else {
					exitCode = 2;
				}
#endif // __WINDOWS__
#endif // not __UNIX_LIKE__
			}	break;
			case Node::NODE_UNRECOVERABLE_ERROR: {
				exitCode = 3;
				const char *termReason = node->reasonForTermination();
				fprintf(stderr,"%s: abnormal termination: %s\n",argv[0],(termReason) ? termReason : "(unknown reason)");
			}	break;
			default:
				break;
		}
		delete node;
		node = (Node *)0;
	} catch ( ... ) {}

#ifdef __UNIX_LIKE__
	Utils::rm((std::string(homeDir)+"/zerotier-one.pid").c_str());
#endif

	return exitCode;
}
