/*************************************************************************\
* Copyright (c) 2002 The University of Chicago, as Operator of Argonne
* National Laboratory.
* Copyright (c) 2002 Berliner Speicherring-Gesellschaft fuer Synchrotron-
* Strahlung mbH (BESSY).
* Copyright (c) 2002 The Regents of the University of California, as
* Operator of Los Alamos National Laboratory.
* This file is distributed subject to a Software License Agreement found
* in the file LICENSE that is included with this distribution. 
\*************************************************************************/
// Author: Jim Kowalkowski
// Date: 2/96

#define DEBUG_ENV 0
#define DEBUG_OPENFILES 0

// Use this to truncate the core file if GATEWAY_CORE_SIZE is
// specified in the environment.  (Truncating makes it unusable so
// consider truncation to 0 if anything.)
#define TRUNC_CORE_FILE 1

#if DEBUG_OPENFILES
#include <limits.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#ifdef USE_SYSLOG
#include <syslog.h>
#endif
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef WIN32
# include <direct.h>
# include <process.h>
#else
# include <sys/wait.h>
# include <unistd.h>
# include <sys/resource.h>
#endif

#include <envDefs.h>
#include "epicsVersion.h"
#include "gateResources.h"

// Function Prototypes
static int startEverything(char *prefix);
void gatewayServer(char *prefix);
void print_instructions(void);
int manage_gateway(void);
static int setEnv(const char *var, const char *val, char **envString);
static int setEnv(const char *var, const int ival, char **envString);

// Global variables
#ifndef WIN32
static pid_t gate_pid;
#endif
static int death_flag=0;

// still need to add client and server IP addr info using 
// the CA environment variables.

// still need to add ability to load and run user servers dynamically

#if 0
void* operator new(size_t x)
{
	void* y = (void*)malloc(x);
	fprintf(stderr,"in op new for %d %8.8x\n",(int)x,y);
	return y;
}
void operator delete(void* x)
{
	fprintf(stderr,"in op del for %8.8x\n",x);
	free((char*)x);
}
#endif

// The parameters passed in from the user are:
//	-debug ? = set debug level, ? is the integer level to set
//	-pvlist file_name = process variable list file
//	-log file_name = log file name
//	-access file_name = access security file
//	-command file_name = USR1 command list file
//	-home directory = the program's home directory
//	-connect_timeout number = clear PV connect requests every number seconds
//	-inactive_timeout number = Hold inactive PV connections for number seconds
//	-dead_timeout number = Hold PV connections with no user for number seconds
//	-cip ip_addr_list = CA client library IP address list (exclusive)
//	-sip ip_addr = IP address where CAS listens for requests
//	-signore ip_addr_list = IP address CAS ignores
//	-cport port_number = CA client library port
//	-sport port_number = CAS port number
//	-ro = read only server, no puts allowed
//	-? = display usage
//
//	GATEWAY_HOME = environement variable pointing to the home of the gateway
//
// Defaults:
//	Home directory = .
//	Access security file = gateway.access
//	process variable list file = gateway.pvlist
//	USR1 command list file = gateway.command
//	log file = gateway.log
//	debug level = 0 (none)
//  connect_timeout = 1 second
//  inactive_timeout = 60*60*2 seconds (2 hours)
//  dead_timeout = 60*2 seconds (2 minutes)
//	cport/sport = CA default port number
//	cip = nothing, the normal interface
//	sip = nothing, the normal interface
//
// Precedence:
//	(1) Command line parameter 
//	(2) environment variables
//	(3) defaults

#define PARM_DEBUG             0
#define PARM_PVLIST            1
#define PARM_LOG               2
#define PARM_ACCESS            3
#define PARM_HOME              4
#define PARM_COMMAND           5
#define PARM_CONNECT           6
#define PARM_INACTIVE          7
#define PARM_DEAD              8
#define PARM_USAGE             9
#define PARM_SERVER_IP        10
#define PARM_CLIENT_IP        11
#define PARM_SERVER_PORT      12
#define PARM_CLIENT_PORT      13
#define PARM_HELP             14
#define PARM_SERVER           15
#define PARM_RO               16
#define PARM_UID              17
#define PARM_PREFIX           18
#define PARM_GID              19
#define PARM_RECONNECT        20
#define PARM_DISCONNECT       21
#define PARM_MASK             22
#define PARM_SERVER_IGNORE_IP 23

#define HOME_DIR_SIZE    300
#define GATE_LOG         "gateway.log"
#define GATE_COMMAND     "gateway.command"

static char *gate_ca_auto_list=NULL;
static char *server_ip_addr=NULL;
static char *server_ignore_ip_addr=NULL;
static char *client_ip_addr=NULL;
static int server_port=0;
static int client_port=0;
static int make_server=0;
static char *home_directory;
static const char *log_file=NULL;
#ifndef WIN32
static pid_t parent_pid;
#endif

struct parm_stuff
{
	const char* parm;
	int len;
	int id;
	const char* desc;
};
typedef struct parm_stuff PARM_STUFF;

// Second parameter is length of first not including null
static PARM_STUFF ptable[] = {
    { "-debug",               6, PARM_DEBUG,       "value" },
    { "-log",                 4, PARM_LOG,         "file_name" },
    { "-pvlist",              7, PARM_PVLIST,      "file_name" },
    { "-access",              7, PARM_ACCESS,      "file_name" },
    { "-command",             8, PARM_COMMAND,     "file_name" },
    { "-home",                5, PARM_HOME,        "directory" },
    { "-sip",                 4, PARM_SERVER_IP,   "IP_address" },
    { "-cip",                 4, PARM_CLIENT_IP,   "IP_address_list" },
    { "-signore",             8, PARM_SERVER_IGNORE_IP, "IP_address_list" },
    { "-sport",               6, PARM_SERVER_PORT, "CA_server_port" },
    { "-cport",               6, PARM_CLIENT_PORT, "CA_client_port" },
    { "-connect_timeout",    16, PARM_CONNECT,     "seconds" },
    { "-inactive_timeout",   17, PARM_INACTIVE,    "seconds" },
    { "-dead_timeout",       13, PARM_DEAD,        "seconds" },
    { "-disconnect_timeout", 19, PARM_DISCONNECT,  "seconds" },
    { "-reconnect_inhibit",  18, PARM_RECONNECT,   "seconds" },
    { "-server",              9, PARM_SERVER,      "(start as server)" },
    { "-uid",                 4, PARM_UID,         "user_id_number" },
    { "-gid",                 4, PARM_GID,         "group_id_number" },
    { "-ro",                  3, PARM_RO,          NULL },
    { "-prefix",              7, PARM_PREFIX,      "statistics_prefix" },
    { "-mask",                5, PARM_MASK,        "event_mask" },
    { "-help",                5, PARM_HELP,        NULL },
    { NULL,                  -1, -1,               NULL }
};

extern "C" {

typedef void (*SIG_FUNC)(int);

static SIG_FUNC save_hup = NULL;
static SIG_FUNC save_int = NULL;
static SIG_FUNC save_term = NULL;
static SIG_FUNC save_bus = NULL;
static SIG_FUNC save_ill = NULL;
static SIG_FUNC save_segv = NULL;
static SIG_FUNC save_chld = NULL;

static void sig_end(int sig)
{
	fflush(stdout);
	fflush(stderr);
	
	switch(sig)
	{
#ifndef WIN32
	case SIGHUP:
#ifdef USE_SYSLOG
		syslog(LOG_NOTICE|LOG_DAEMON,"PV Gateway Ending (SIGHUP)");
#endif
		fprintf(stderr,"PV Gateway Ending (SIGHUP)\n");
		if(save_hup) save_hup(sig);
		break;
#endif //#ifndef WIN32
	case SIGTERM:
#ifdef USE_SYSLOG
		syslog(LOG_NOTICE|LOG_DAEMON,"PV Gateway Ending (SIGTERM)");
#endif
		fprintf(stderr,"PV Gateway Ending (SIGTERM)\n");
		if(save_term) save_term(sig);
		break;
	case SIGINT:
#ifdef USE_SYSLOG
		syslog(LOG_NOTICE|LOG_DAEMON,"PV Gateway Ending (SIGINT)");
#endif
		fprintf(stderr,"PV Gateway Ending (SIGINT)\n");
		if(save_int) save_int(sig);
		break;
	case SIGILL:
#ifdef USE_SYSLOG
		syslog(LOG_NOTICE|LOG_DAEMON,"PV Gateway Aborting (SIGILL)");
#endif
		fprintf(stderr,"PV Gateway Aborting (SIGILL)\n");
		if(save_ill) save_ill(sig);
		abort();
#ifndef WIN32
	case SIGBUS:
#ifdef USE_SYSLOG
		syslog(LOG_NOTICE|LOG_DAEMON,"PV Gateway Aborting (SIGBUS)");
#endif
		fprintf(stderr,"PV Gateway Aborting (SIGBUS)\n");
		if(save_bus) save_bus(sig);
		abort();
#endif //#ifndef WIN32
	case SIGSEGV:
#ifdef USE_SYSLOG
		syslog(LOG_NOTICE|LOG_DAEMON,"PV Gateway Aborting (SIGSEGV)");
#endif
		fprintf(stderr,"PV Gateway Aborting (SIGSEGV)\n");
		if(save_segv) save_segv(sig);
		abort();
	default:
#ifdef USE_SYSLOG
		syslog(LOG_NOTICE|LOG_DAEMON,"PV Gateway Exiting (Unknown Signal)");
#endif
		fprintf(stderr,"PV Gateway Exiting (Unknown Signal)\n");
		break;
	}
	
	exit(0);
}

#ifndef WIN32
static void sig_chld(int /*sig*/)
{
#ifdef SOLARIS
	while(waitpid(-1,NULL,WNOHANG)>0);
#else
	while(wait3(NULL,WNOHANG,NULL)>0);
#endif
	signal(SIGCHLD,sig_chld);
}
#endif //#ifndef WIN32

#ifndef WIN32
static void sig_stop(int /*sig*/)
{
	if(gate_pid)
	  kill(gate_pid,SIGTERM);
	
	death_flag=1;
}
#endif

} // End extern "C"

static int startEverything(char *prefix)
{
	char *gate_cas_port=NULL;
	char *gate_cas_addr=NULL;
	char *gate_cas_ignore_addr=NULL;
	char *gate_ca_list=NULL;
	char *gate_ca_port=NULL;
	int sid;
#ifndef WIN32
	FILE* fd;
	struct rlimit lim;
#endif

	if(client_ip_addr) {
		int status=setEnv("EPICS_CA_ADDR_LIST",client_ip_addr,
		  &gate_ca_list);
		// In addition, make EPICS_CA_AUTO_LIST=NO to avoid sending
		// search requests to ourself.  Note that if
		// EPICS_CA_ADDR_LIST is specified instead of -cip, then
		// EPICS_CA_AUTO_ADDR_LIST=NO must be set also as this branch
		// will not be taken.
		status=setEnv("EPICS_CA_AUTO_ADDR_LIST","NO",&gate_ca_auto_list);
		gateDebug1(15,"gateway setting <%s>\n",gate_ca_auto_list);
		gateDebug1(15,"gateway setting <%s>\n",gate_ca_list);
	}

	if(server_ip_addr) {
		setEnv("EPICS_CAS_INTF_ADDR_LIST",server_ip_addr,
		  &gate_cas_addr);
		gateDebug1(15,"gateway setting <%s>\n",gate_cas_addr);
	}

	if(server_ignore_ip_addr) {
		setEnv("EPICS_CAS_IGNORE_ADDR_LIST",server_ignore_ip_addr,
		  &gate_cas_ignore_addr);
		gateDebug1(15,"gateway setting <%s>\n",gate_cas_ignore_addr);
	}

	if(client_port) {
		setEnv("EPICS_CA_SERVER_PORT",client_port,
		  &gate_ca_port);
		gateDebug1(15,"gateway setting <%s>\n",gate_ca_port);
	}

	if(server_port) {
		setEnv("EPICS_CAS_SERVER_PORT",server_port,
		  &gate_cas_port);
		gateDebug1(15,"gateway setting <%s>\n",gate_cas_port);
	}

	sid=getpid();
	
#ifndef WIN32
	// Make script file ("gateway.killer" by default)
	errno=0;
	if((fd=fopen(GATE_SCRIPT_FILE,"w"))==(FILE*)NULL) {
		fprintf(stderr,"Opening script file failed: %s\n",
		  GATE_SCRIPT_FILE);
		fflush(stderr);
		perror("Reason");
		fflush(stderr);
		fd=stderr;
	}
	fprintf(fd,"\n");
	fprintf(fd,"# options:\n");
	fprintf(fd,"# home=<%s>\n",home_directory);
	fprintf(fd,"# log file=<%s>\n",log_file);
	fprintf(fd,"# access file=<%s>\n",global_resources->accessFile());
	fprintf(fd,"# pvlist file=<%s>\n",global_resources->listFile());
	fprintf(fd,"# command file=<%s>\n",global_resources->commandFile());
	fprintf(fd,"# debug level=%d\n",global_resources->debugLevel());
	fprintf(fd,"# dead timeout=%ld\n",global_resources->deadTimeout());
	fprintf(fd,"# connect timeout=%ld\n",global_resources->connectTimeout());
	fprintf(fd,"# disconnect timeout=%ld\n",global_resources->disconnectTimeout());
	fprintf(fd,"# reconnect inhibit time=%ld\n",global_resources->reconnectInhibit());
	fprintf(fd,"# inactive timeout=%ld\n",global_resources->inactiveTimeout());
	fprintf(fd,"# event mask=%s\n",global_resources->eventMaskString());
	fprintf(fd,"# user id=%ld\n",(long)getuid());
	fprintf(fd,"# group id=%ld\n",(long)getgid());
	fprintf(fd,"# \n");
	fprintf(fd,"# use the following to execute commands in command file:\n");
	fprintf(fd,"#    kill -USR1 %d\n",sid);
	fprintf(fd,"# use the following to get a PV summary report in the log:\n");
	fprintf(fd,"#    kill -USR2 %d\n",sid);

	fprintf(fd,"# \n");

	if(global_resources->isReadOnly()) {
		fprintf(fd,"# Gateway running in read-only mode.\n");
	}

	if(client_ip_addr) {
		fprintf(fd,"# %s\n",gate_ca_list);
		fprintf(fd,"# %s\n",gate_ca_auto_list);
	}

	// Print command-line arguments to script file
	if(server_ip_addr) fprintf(fd,"# %s\n",gate_cas_addr);
	if(server_ignore_ip_addr) fprintf(fd,"# %s\n",gate_cas_ignore_addr);
	if(client_port) fprintf(fd,"# %s\n",gate_ca_port);
	if(server_port) fprintf(fd,"# %s\n",gate_cas_port);

	fprintf(fd,"\n kill %ld # to kill everything\n\n",parent_pid);
	fprintf(fd,"\n # kill %u # to kill off this gateway\n\n",sid);
	fflush(fd);
	
	if(fd!=stderr) fclose(fd);
	chmod(GATE_SCRIPT_FILE,00755);
#endif  //#ifndef WIN32
	
#ifndef WIN32
	// Make script file ("gateway.restart" by default)
	errno=0;
	if((fd=fopen(GATE_RESTART_FILE,"w"))==(FILE*)NULL) {
		fprintf(stderr,"Opening restart file failed: %s\n",
		  GATE_RESTART_FILE);
		fflush(stderr);
		perror("Reason");
		fflush(stderr);
		fd=stderr;
	}
	
	fprintf(fd,"\n kill %d # to kill off this gateway\n\n",sid);
	fflush(fd);
	
	if(fd!=stderr) fclose(fd);
	chmod(GATE_RESTART_FILE,00755);
#endif  //#ifndef WIN32
	
#ifndef WIN32
	// Set process limits
	if(getrlimit(RLIMIT_NOFILE,&lim)<0) {
		fprintf(stderr,"Cannot retrieve the process FD limits\n");
	} else	{
#if DEBUG_OPENFILES
		printf("RLIMIT_NOFILE (before): rlim_cur=%d rlim_rlim_max=%d "
		  "OPEN_MAX=%d SC_OPEN_MAX=%d FOPEN_MAX=%d\n",
		  lim.rlim_cur,lim.rlim_max,
		  OPEN_MAX,_SC_OPEN_MAX,FOPEN_MAX);
		printf("  sysconf: _SC_OPEN_MAX %d _SC_STREAM_MAX %d\n",
		  sysconf(_SC_OPEN_MAX), sysconf(_SC_STREAM_MAX));
#endif
		if(lim.rlim_cur<lim.rlim_max) {
			lim.rlim_cur=lim.rlim_max;
			if(setrlimit(RLIMIT_NOFILE,&lim)<0)
			  fprintf(stderr,"Failed to set FD limit %d\n",
				(int)lim.rlim_cur);
		}
#if DEBUG_OPENFILES
		if(getrlimit(RLIMIT_NOFILE,&lim)<0) {
			printf("RLIMIT_NOFILE (after): Failed\n");
		} else {
			printf("RLIMIT_NOFILE (after): rlim_cur=%d rlim_rlim_max=%d "
			  "OPEN_MAX=%d SC_OPEN_MAX=%d FOPEN_MAX=%d\n",
			  lim.rlim_cur,lim.rlim_max,
			  OPEN_MAX,_SC_OPEN_MAX,FOPEN_MAX);
			printf("  sysconf: _SC_OPEN_MAX %d _SC_STREAM_MAX %d\n",
			  sysconf(_SC_OPEN_MAX), sysconf(_SC_STREAM_MAX));
		}
#endif
	}
	
	if(getrlimit(RLIMIT_CORE,&lim)<0) {
		fprintf(stderr,"Cannot retrieve the process FD limits\n");
	} else {
#if TRUNC_CORE_FILE
		// KE: Used to truncate it to 20000000 if GATEWAY_CORE_SIZE
		// was not specified.  Truncating the core file makes it
		// unusable.  Now only does it if GATEWAY_CORE_SIZE is
		// specified.
		long core_len=0;
		char *core_size=getenv("GATEWAY_CORE_SIZE");
		if(core_size && sscanf(core_size,"%ld",&core_len) == 1) {
			lim.rlim_cur=core_len;
			if(setrlimit(RLIMIT_CORE,&lim) < 0) {
				fprintf(stderr,"Failed to set core limit to %d\n",
				  (int)lim.rlim_cur);
			}
		}
#endif
	}
#endif  //#ifndef WIN32
	
#ifndef WIN32
	save_hup=signal(SIGHUP,sig_end);
	save_bus=signal(SIGBUS,sig_end);
#endif
	save_term=signal(SIGTERM,sig_end);
	save_int=signal(SIGINT,sig_end);
	save_ill=signal(SIGILL,sig_end);
	save_segv=signal(SIGSEGV,sig_end);

#ifdef USE_SYSLOG
	syslog(LOG_NOTICE|LOG_DAEMON,"PV Gateway Starting");
#endif

	char timeStampStr[16];
	long now;
	struct tm *tblock;
	
	time(&now);
	tblock=localtime(&now);
	strftime(timeStampStr,20,"%b %d %H:%M:%S",tblock);
	printf("%s %s [%s %s]\n",
	  timeStampStr,GATEWAY_VERSION_STRING,__DATE__,__TIME__);	  
	printf("%s PID=%d\n",EPICS_VERSION_STRING,sid);

#if DEBUG_ENV
	system("printenv | grep EPICS");
	fflush(stdout); fflush(stderr);
#endif

	gatewayServer(prefix);

	return 0;
}

int main(int argc, char** argv)
{
#ifndef WIN32
	uid_t uid;
	gid_t gid;
#endif
	int i,j,k;
	int not_done=1;
	int no_error=1;
	int level=0;
	int read_only=0;
	unsigned long mask=0;
	int connect_tout=-1;
	int inactive_tout=-1;
	int dead_tout=-1;
	int disconnect_tout=-1;
	int reconnect_tinhib=-1;
	char* home_dir=NULL;
	char* pvlist_file=NULL;
	char* access_file=NULL;
	char* command_file=NULL;
	char* stat_prefix=NULL;
	time_t t;
#ifndef WIN32
	char cur_time[300];
	struct stat sbuf;
#endif

	home_dir=getenv("GATEWAY_HOME");
	home_directory=new char[HOME_DIR_SIZE];

	// Parse command line
	for(i=1;i<argc && no_error;i++)
	{
		for(j=0;not_done && no_error && ptable[j].parm;j++)
		{
			if(strncmp(ptable[j].parm,argv[i],ptable[j].len)==0)
			{
				switch(ptable[j].id)
				{
				case PARM_DEBUG:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							if(sscanf(argv[i],"%d",&level)<1)
								no_error=0;
							else
								not_done=0;
						}
					}
					break;
				case PARM_MASK:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							for (k=0; argv[i][k]; k++) {
								switch (argv[i][k]) {
								case 'a' :
								case 'A' :
									mask |= DBE_ALARM;
									break;
								case 'v' :
								case 'V' :
									mask |= DBE_VALUE;
									break;
								case 'l' :
								case 'L' :
									mask |= DBE_LOG;
									break;
								default :
									break;
								}
							}
							not_done=0;
						}
					}
					break;
				case PARM_HELP:
					print_instructions();
					return 0;
				case PARM_SERVER:
					make_server=1;
					not_done=0;
					break;
				case PARM_RO:
					read_only=1;
					not_done=0;
					break;
#ifndef WIN32
				case PARM_UID:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							sscanf(argv[i],"%d",(int *)&uid);
							setuid(uid);
							not_done=0;
						}
					}
					break;
#endif
#ifndef WIN32
				case PARM_GID:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							sscanf(argv[i],"%d",(int *)&gid);
							setgid(gid);
							not_done=0;
						}
					}
					break;
#endif
				case PARM_PVLIST:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							pvlist_file=argv[i];
							not_done=0;
						}
					}
					break;
				case PARM_LOG:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							log_file=argv[i];
							not_done=0;
						}
					}
					break;
				case PARM_COMMAND:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							command_file=argv[i];
							not_done=0;
						}
					}
					break;
				case PARM_ACCESS:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							access_file=argv[i];
							not_done=0;
						}
					}
					break;
				case PARM_HOME:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							home_dir=argv[i];
							not_done=0;
						}
					}
					break;
				case PARM_SERVER_IP:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							server_ip_addr=argv[i];
							not_done=0;
						}
					}
					break;
				case PARM_SERVER_IGNORE_IP:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							server_ignore_ip_addr=argv[i];
							not_done=0;
						}
					}
					break;
				case PARM_CLIENT_IP:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							client_ip_addr=argv[i];
							not_done=0;
						}
					}
					break;
				case PARM_CLIENT_PORT:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							if(sscanf(argv[i],"%d",&client_port)<1)
								no_error=0;
							else
								not_done=0;
						}
					}
					break;
				case PARM_SERVER_PORT:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							if(sscanf(argv[i],"%d",&server_port)<1)
								no_error=0;
							else
								not_done=0;
						}
					}
					break;
				case PARM_DEAD:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							if(sscanf(argv[i],"%d",&dead_tout)<1)
								no_error=0;
							else
								not_done=0;
						}
					}
					break;
				case PARM_INACTIVE:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							if(sscanf(argv[i],"%d",&inactive_tout)<1)
								no_error=0;
							else
								not_done=0;
						}
					}
					break;
				case PARM_CONNECT:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							if(sscanf(argv[i],"%d",&connect_tout)<1)
								no_error=0;
							else
								not_done=0;
						}
					}
					break;
				case PARM_DISCONNECT:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							if(sscanf(argv[i],"%d",&disconnect_tout)<1)
								no_error=0;
							else
								not_done=0;
						}
					}
					break;
				case PARM_RECONNECT:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							if(sscanf(argv[i],"%d",&reconnect_tinhib)<1)
								no_error=0;
							else
								not_done=0;
						}
					}
					break;
				case PARM_PREFIX:
					if(++i>=argc) no_error=0;
					else
					{
						if(argv[i][0]=='-') no_error=0;
						else
						{
							stat_prefix=argv[i];
							not_done=0;
						}
					}
					break;
				default:
					no_error=0;
					break;
				}
			}
		}
		not_done=1;
		if(ptable[j].parm==NULL) no_error=0;
	}

	// ----------------------------------
	// Go to gateway's home directory now
	if(home_dir)
	{
		if(chdir(home_dir)<0)
		{
			perror("Change to home directory failed");
			fprintf(stderr,"-->Bad home <%s>\n",home_dir); fflush(stderr);
			return -1;
		}
	}
	getcwd(home_directory,HOME_DIR_SIZE);

#ifndef WIN32
	if(make_server)
	{
		// start watcher process
		if(manage_gateway()) return 0;
	}
	else
	{
		parent_pid=getpid();
	}
#endif

	// ****************************************
	// gets here if this is interactive gateway
	// ****************************************

	// ----------------------------------------
	// change stderr and stdout to the log file

	if(log_file || make_server)
	{
		if(log_file==NULL) log_file=GATE_LOG;
		time(&t);

#ifndef WIN32
		// Save log file if it exists
		if(stat(log_file,&sbuf)==0)
		{
			if(sbuf.st_size>0)
			{
				sprintf(cur_time,"%s.%lu",log_file,(unsigned long)t);
				if(link(log_file,cur_time)<0)
				{
					fprintf(stderr,"Failure to move old log to new name %s",
						cur_time);
				}
				else
					unlink(log_file);
			}
		}
#endif

		// Redirect stdout and stderr
		// Open it and close it to empty it (Necessary on WIN32,
		// apparently not necessary on Solaris)
		FILE *fp=fopen(log_file,"w");
		if(fp == NULL) {
			fprintf(stderr,"Cannot open %s\n",log_file);
			fflush(stderr);
		} else {
			fclose(fp);
		}
		// KE: This was formerly "w" instead of "a" and stderr was
		//  overwriting the top of the log file
		if((freopen(log_file,"a",stderr))==NULL ) {
			fprintf(stderr,"Redirect of stderr to file %s failed\n",log_file);
			fflush(stderr);
		}
		if((freopen(log_file,"a",stdout))==NULL ) {
			fprintf(stderr,"Redirect of stdout to file %s failed\n",log_file);
			fflush(stderr);
		}
	}
	else
		log_file="<terminal>";

	// ----------------------------------------
	// set up gateway resources

	global_resources = new gateResources;
	gateResources* gr = global_resources;

	if(no_error==0)
	{
		int ii;
		fprintf(stderr,"usage: %s followed by the these options:\n",argv[0]);
		for(ii=0;ptable[ii].parm;ii++)
		{
			if(ptable[ii].desc)
				fprintf(stderr,"\t[%s %s ]\n",ptable[ii].parm,ptable[ii].desc);
			else
				fprintf(stderr,"\t[%s]\n",ptable[ii].parm);
		}
		fprintf(stderr,"\nDefaults are:\n");
		fprintf(stderr,"\tdebug=%d\n",gr->debugLevel());
		fprintf(stderr,"\thome=%s\n",home_directory);
		fprintf(stderr,"\tlog=%s\n",log_file);
		fprintf(stderr,"\taccess=%s\n",gr->accessFile());
		fprintf(stderr,"\tpvlist=%s\n",gr->listFile());
		fprintf(stderr,"\tcommand=%s\n",gr->commandFile());
		fprintf(stderr,"\tdead=%ld\n",gr->deadTimeout());
		fprintf(stderr,"\tconnect=%ld\n",gr->connectTimeout());
		fprintf(stderr,"\tdisconnect=%ld\n",gr->disconnectTimeout());
		fprintf(stderr,"\treconnect=%ld\n",gr->reconnectInhibit());
		fprintf(stderr,"\tinactive=%ld\n",gr->inactiveTimeout());
		fprintf(stderr,"\tmask=%s\n",gr->eventMaskString());
#ifndef WIN32
		fprintf(stderr,"\tuser id=%ld\n",(long)getuid());
		fprintf(stderr,"\tgroup id=%ld\n",(long)getgid());
#endif
		if(gr->isReadOnly())
			fprintf(stderr," read only mode\n");
		return -1;
	}

	// order is somewhat important
	if(level)				gr->setDebugLevel(level);
	if(read_only)			gr->setReadOnly();
	if(mask)				gr->setEventMask(mask);
	if(connect_tout>=0)		gr->setConnectTimeout(connect_tout);
	if(inactive_tout>=0)	gr->setInactiveTimeout(inactive_tout);
	if(dead_tout>=0)		gr->setDeadTimeout(dead_tout);
	if(disconnect_tout>=0)	gr->setDisconnectTimeout(disconnect_tout);
	if(reconnect_tinhib>=0)	gr->setReconnectInhibit(reconnect_tinhib);
	if(access_file)			gr->setAccessFile(access_file);
	if(pvlist_file)			gr->setListFile(pvlist_file);
	if(command_file)		gr->setCommandFile(command_file);

	gr->setUpAccessSecurity();

	if(gr->debugLevel()>10)
	{
		fprintf(stderr,"\noption dump:\n");
		fprintf(stderr," home = <%s>\n",home_directory);
		fprintf(stderr," log file = <%s>\n",log_file);
		fprintf(stderr," access file = <%s>\n",gr->accessFile());
		fprintf(stderr," list file = <%s>\n",gr->listFile());
		fprintf(stderr," command file = <%s>\n",gr->commandFile());
		fprintf(stderr," debug level = %d\n",gr->debugLevel());
		fprintf(stderr," connect timeout = %ld\n",gr->connectTimeout());
		fprintf(stderr," disconnect timeout = %ld\n",gr->disconnectTimeout());
		fprintf(stderr," reconnect inhibit time = %ld\n",gr->reconnectInhibit());
		fprintf(stderr," inactive timeout = %ld\n",gr->inactiveTimeout());
		fprintf(stderr," dead timeout = %ld\n",gr->deadTimeout());
		fprintf(stderr," event mask = %s\n",gr->eventMaskString());
#ifndef WIN32
		fprintf(stderr," user id= %ld\n",(long)getuid());
		fprintf(stderr," group id= %ld\n",(long)getgid());
#endif
		if(gr->isReadOnly())
			fprintf(stderr," read only mode\n");
		fflush(stderr);
	}

	startEverything(stat_prefix);
	delete global_resources;
	return 0;
}

#define pr fprintf

void print_instructions(void)
{
	pr(stderr,"-debug value: Enter value between 0-100.  50 gives lots of\n");
	pr(stderr," info, 1 gives small amount.\n\n");
	
	pr(stderr,"-pvlist file_name: Name of file with all the allowed PVs in it\n");
	pr(stderr," See the sample file gateway.pvlist in the source distribution\n");
	pr(stderr," for a description of how to create this file.\n");
	
	pr(stderr,"-access file_name: Name of file with all the EPICS access\n");
	pr(stderr," security rules in it.  PVs in the pvlist file use groups\n");
	pr(stderr," and rules defined in this file.\n");
	
	pr(stderr,"-log file_name: Name of file where all messages from the\n");
	pr(stderr," gateway go, including stderr and stdout.\n\n");
	
	pr(stderr,"-command file_name: Name of file where gateway command(s) go\n");
	pr(stderr," Commands are executed when a USR1 signal is sent to gateway.\n\n");
	
	pr(stderr,"-home directory: Home directory where all your gateway\n");
	pr(stderr," configuration files are kept where log and command files go.\n\n");
	
	pr(stderr,"-sip IP_address: IP address that gateway's CA server listens\n");
	pr(stderr," for PV requests.  Sets env variable EPICS_CAS_INTF_ADDR.\n\n");
	
	pr(stderr,"-signore IP_address_list: IP address that gateway's CA server\n");
	pr(stderr," ignores.  Sets env variable EPICS_CAS_IGNORE_ADDR_LIST.\n\n");
	
	pr(stderr,"-cip IP_address_list: IP address list that the gateway's CA\n");
	pr(stderr," client uses to find the real PVs.  See CA reference manual.\n");
	pr(stderr," This sets environment variables EPICS_CA_AUTO_LIST=NO and\n");
	pr(stderr," EPICS_CA_ADDR_LIST.\n\n");
	
	pr(stderr,"-sport CA_server_port: The port which the gateway's CA server\n");
	pr(stderr," uses to listen for PV requests.  Sets environment variable\n");
	pr(stderr," EPICS_CAS_SERVER_PORT.\n\n");
	
	pr(stderr,"-cport CA_client_port:  The port thich the gateway's CA client\n");
	pr(stderr," uses to find the real PVs.  Sets environment variable\n");
	pr(stderr," EPICS_CA_SERVER_PORT.\n\n");
	
	pr(stderr,"-connect_timeout seconds: The amount of time that the\n");
	pr(stderr," gateway will allow a PV search to continue before marking the\n");
	pr(stderr," PV as being not found.\n\n");
	
	pr(stderr,"-inactive_timeout seconds: The amount of time that the gateway\n");
	pr(stderr," will hold the real connection to an unused PV.  If no gateway\n");
	pr(stderr," clients are using the PV, the real connection will still be\n");
	pr(stderr," held for this long.\n\n");
	
	pr(stderr,"-dead_timeout seconds:  The amount of time that the gateway\n");
	pr(stderr," will hold requests for PVs that are not found on the real\n");
	pr(stderr," network that the gateway is using.  Even if a client's\n");
	pr(stderr," requested PV is not found on the real network, the gateway\n");
	pr(stderr," marks the PV dead, holds the request and continues trying\n");
	pr(stderr," to connect for this long.\n\n");
	
	pr(stderr,"-disconnect_timeout seconds:  The amount of time that the gateway\n");
	pr(stderr," will hold requests for PVs that were connected but have been\n");
	pr(stderr," disconnected. When a disconnected PV reconnects, the gateway will\n");
	pr(stderr," broadcast a beacon signal to inform the clients that they may\n");
	pr(stderr," reconnect to the gateway.\n\n");

	pr(stderr,"-reconnect_inhibit seconds:  The minimum amount of time between\n");
	pr(stderr," additional beacons that the gateway will send to its clients\n");
	pr(stderr," when channels from the real network reconnect.\n\n");

	pr(stderr,"-server: Start as server. Detach from controlling terminal\n");
	pr(stderr," and start a daemon that watches the gateway and automatically\n");
	pr(stderr," restarted it if it dies.\n");
	
	pr(stderr,"-mask event_mask: Event mask that is used for connections on the\n");
	pr(stderr," real network: use any combination of v (value), a (alarm), l (log).\n");
	pr(stderr," Default is va (forward value and alarm change events).\n");
	
	pr(stderr,"-prefix string: Set the prefix for the gateway statistics PVs.\n");
	pr(stderr," Defaults to the hostname the gateway is running on.\n");
	
	pr(stderr,"-uid number: Run the server with this id, server does a\n");
	pr(stderr," setuid(2) to this user id number.\n\n");
	pr(stderr,"-gid number: Run the server with this id, server does a\n");
	pr(stderr," setgid(2) to this group id number.\n\n");
}

#ifndef WIN32
// -------------------------------------------------------------------
//  part that watches the gateway process and ensures that it stays up

int manage_gateway(void)
{
	time_t t,pt=0;
	int rc;
	
	save_chld=signal(SIGCHLD,sig_chld);
	save_hup=signal(SIGHUP,sig_stop);
	save_term=signal(SIGTERM,sig_stop);
	save_int=signal(SIGINT,sig_stop);
	
	// disassociate from parent
	switch(fork())
	{
	case -1: // error
		perror("Cannot create gateway processes");
		return -1;
	case 0: // child
#if defined UNIX
		setpgrp();
#else
		setpgrp(0,0);
#endif
		setsid();
		break;
	default: // parent
		return 1;
	}
	
	parent_pid=getpid();
	
	do
	{
		time(&t);
		if((t-pt)<5) sleep(6); // don't respawn faster than every 6 seconds
		pt=t;
		
		switch(gate_pid=fork())
		{
		case -1: // error
			perror("Cannot create gateway processes");
			gate_pid=0;
			break;
		case 0: // child
			break;
		default: // parent
			pause();
			break;
		}
	}
	while(gate_pid && death_flag==0);
	
	if(death_flag || gate_pid==-1)
		rc=1;
	else
		rc=0;

	return rc;

}
#endif //#ifdef WIN32

static int setEnv(const char *var, const char *val, char **envString)
{
	int len=strlen(var)+strlen(val)+2;

	*envString=(char *)malloc(len);
	if(!*envString) {
		fprintf(stderr,"Memory allocation error for %s",var);
		return 1;
	}
	sprintf(*envString,"%s=%s",var,val);
#if 0
	// There is no putenv on Linux
	int status=putenv(*envString);
	if(status) {
		fprintf(stderr,"putenv failed for:\n  %s\n",*envString);
	}
#else
	epicsEnvSet(var,val);
#endif
	return 0;
}

static int setEnv(const char *var, int ival, char **envString)
{
	// Allow 40 for size of ival
	int len=strlen(var)+40+2;

	*envString=(char *)malloc(len);
	if(!*envString) {
		fprintf(stderr,"Memory allocation error for %s",var);
		return 1;
	}
	sprintf(*envString,"%s=%d",var,ival);
#if 0
	// There is no putenv on Linux
	int status=putenv(*envString);
	if(status) {
		fprintf(stderr,"putenv failed for:\n  %s\n",*envString);
	}
#else
	char *pVal=strchr(*envString,'=');
	if(!pVal || !(pVal+1)) {
		epicsEnvSet(var,"");
	} else {
		epicsEnvSet(var,pVal+1);
	}
#endif
	return 0;
}


/* **************************** Emacs Editing Sequences ***************** */
/* Local Variables: */
/* tab-width: 4 */
/* c-basic-offset: 4 */
/* c-comment-only-line-offset: 0 */
/* c-file-offsets: ((substatement-open . 0) (label . 0)) */
/* End: */
