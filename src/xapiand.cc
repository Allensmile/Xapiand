/*
 * Copyright (C) 2015,2016,2017 deipi.com LLC and contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "xapiand.h"

#include <algorithm>                 // for min
#include <chrono>                    // for system_clock, time_point
#include <clocale>                   // for setlocale, LC_CTYPE
#include <grp.h>                     // for getgrgid, group, getgrnam, gid_t
#include <iostream>                  // for operator<<, basic_ostream, ostream
#include <list>                      // for __list_iterator, list, operator!=
#include <memory>                    // for unique_ptr, allocator, make_unique
#include <pwd.h>                     // for passwd, getpwnam, getpwuid
#include <signal.h>                  // for sigaction, sigemptyset
#include <sstream>                   // for basic_stringbuf<>::int_type, bas...
#include <stdio.h>                   // for snprintf
#include <stdlib.h>                  // for size_t, atoi, setenv, exit, getenv
#include <string.h>                  // for strcat, strchr, strlen, strrchr
#include <strings.h>                 // for strcasecmp
#include <sys/fcntl.h>               // for O_RDWR, O_CREAT
#include <sys/signal.h>              // for sigaction, signal, SIG_IGN, SIGHUP
#include <sysexits.h>                // for EX_NOUSER, EX_OK, EX_USAGE, EX_O...
#include <thread>                    // for thread
#include <time.h>                    // for tm, localtime, mktime, time_t
#include <unistd.h>                  // for dup2, unlink, STDERR_FILENO, chdir
#include <vector>                    // for vector
#include <xapian.h>                  // for XAPIAN_HAS_GLASS_BACKEND, XAPIAN...

#if defined(XAPIAND_V8)
#include <v8-version.h>                       // for V8_MAJOR_VERSION, V8_MINOR_VERSION
#endif
#if defined(XAPIAND_CHAISCRIPT)
#include <chaiscript/chaiscript_defines.hpp>  // for chaiscript::Build_Info
#endif

#include "endpoint.h"                // for Endpoint, Endpoint::cwd
#include "ev/ev++.h"                 // for ::DEVPOLL, ::EPOLL, ::KQUEUE
#include "exception.h"               // for Exit
#include "io_utils.h"                // for close, open, write
#include "log.h"                     // for Logging, L_INFO, L_CRIT, L_NOTICE
#include "manager.h"                 // for opts_t, XapiandManager, XapiandM...
#include "schema.h"                  // for default_spc
#include "tclap/CmdLine.h"           // for CmdLine, ArgException, Arg, CmdL...
#include "utils.h"                   // for format_string, center_string
#include "package.h"                 // for Package
#include "worker.h"                  // for Worker
#include "xxh64.hpp"                 // for xxh64

#include <sys/sysctl.h>              // for sysctl, sysctlnametomib...


#define LINE_LENGTH 78

#define FDS_RESERVED     50          // Is there a better approach?
#define FDS_PER_CLIENT    2          // KQUEUE + IPv4
#define FDS_PER_DATABASE  7          // Writable~=7, Readable~=5

#define CONFIG_DEFAULT_MAX_CLIENTS 1000
#define CONFIG_DEFAULT_MAX_FILES   (FDS_RESERVED + ((DBPOOL_SIZE + 2) * FDS_PER_DATABASE) + ((CONFIG_DEFAULT_MAX_CLIENTS + 2) * FDS_PER_CLIENT))


using namespace TCLAP;


template<typename T, std::size_t N>
ssize_t write(int fildes, T (&buf)[N]) {
	return write(fildes, buf, N - 1);
}

ssize_t write(int fildes, const std::string& buf) {
	return write(fildes, buf.data(), buf.size());
}


static const std::vector<std::string> vec_signame = []() {
	std::vector<std::string> res;
	auto len = arraySize(sys_siglist); /* same size of sys_signame but sys_signame is not portable */
	res.reserve(len);
	for (size_t sig = 0; sig < len; ++sig) {
		auto col = &NO_COL;
		switch (sig) {
			case SIGQUIT:
			case SIGILL:
			case SIGTRAP:
			case SIGABRT:
#if defined(__APPLE__) || defined(__FreeBSD__)
			case SIGEMT:
#endif
			case SIGFPE:
			case SIGBUS:
			case SIGSEGV:
			case SIGSYS:
				// create core image
				col = &LIGHT_RED;
				break;
			case SIGHUP:
			case SIGINT:
			case SIGKILL:
			case SIGPIPE:
			case SIGALRM:
			case SIGTERM:
			case SIGXCPU:
			case SIGXFSZ:
			case SIGVTALRM:
			case SIGPROF:
			case SIGUSR1:
			case SIGUSR2:
#if defined(__linux__)
			case SIGSTKFLT:
#endif
				// terminate process
				col = &RED;
				break;
			case SIGSTOP:
			case SIGTSTP:
			case SIGTTIN:
			case SIGTTOU:
				// stop process
				col = &YELLOW;
				break;
			case SIGURG:
			case SIGCONT:
			case SIGCHLD:
			case SIGIO:
			case SIGWINCH:
#if defined(__APPLE__) || defined(__FreeBSD__)
			case SIGINFO:
#endif
				// discard signal
				col = &BLUE;
				break;
			default:
				col = &BLUE;
				break;
		}
#if defined(__linux__)
		res.push_back(format_string(*col + "Signal received: %s" + NO_COL + "\n", strsignal(sig)));
#elif defined(__APPLE__) || defined(__FreeBSD__)
		res.push_back(format_string(*col + "Signal received: %s" + NO_COL + "\n", sys_signame[sig]));
#endif
	}
	res.push_back(BLUE + "Signal received: unknown" + NO_COL + "\n");
	return res;
}();


#ifndef NDEBUG
void sig_info(int) {
	if (logger_info_hook) {
		logger_info_hook = 0;
		write(STDERR_FILENO, BLUE + "Info hooks disabled!" + NO_COL + "\n");
	} else {
		logger_info_hook = -1ULL;
		write(STDERR_FILENO, BLUE + "Info hooks enabled!" + NO_COL + "\n");
	}
}
#endif


void sig_handler(int sig) {
	const auto& msg = (sig >= 0 && sig < static_cast<int>(vec_signame.size() - 1)) ? vec_signame[sig] : vec_signame.back();
	write(STDERR_FILENO, msg.data(), msg.size());

#if !defined(NDEBUG) && (defined(__APPLE__) || defined(__FreeBSD__))
	if (sig == SIGINFO) {
		sig_info(sig);
	}
#endif

	if (XapiandManager::manager) {
		XapiandManager::manager->signal_sig(sig);
	}
}


void setup_signal_handlers(void) {
	signal(SIGHUP, SIG_IGN);   // Ignore terminal line hangup
	signal(SIGPIPE, SIG_IGN);  // Ignore write on a pipe with no reader

	struct sigaction act;

	/* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction is used.
	 * Otherwise, sa_handler is used. */
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = sig_handler;
	sigaction(SIGTERM, &act, nullptr);  // On software termination signal
	sigaction(SIGINT, &act, nullptr);   // On interrupt program (Ctrl-C)
#if defined(__APPLE__) || defined(__FreeBSD__)
	sigaction(SIGINFO, &act, nullptr);  // On status request from keyboard (Ctrl-T)
#endif
	sigaction(SIGUSR1, &act, nullptr);
	sigaction(SIGUSR1, &act, nullptr);
}


#define EV_SELECT_NAME  "select"
#define EV_POLL_NAME    "poll"
#define EV_EPOLL_NAME   "epoll"
#define EV_KQUEUE_NAME  "kqueue"
#define EV_DEVPOLL_NAME "devpoll"
#define EV_PORT_NAME    "port"


unsigned int ev_backend(const std::string& name) {
	auto ev_use = lower_string(name);
	if (ev_use.empty() || ev_use.compare("auto") == 0) {
		return ev::AUTO;
	}
	if (ev_use.compare(EV_SELECT_NAME) == 0) {
		return ev::SELECT;
	}
	if (ev_use.compare(EV_POLL_NAME) == 0) {
		return ev::POLL;
	}
	if (ev_use.compare(EV_EPOLL_NAME) == 0) {
		return ev::EPOLL;
	}
	if (ev_use.compare(EV_KQUEUE_NAME) == 0) {
		return ev::KQUEUE;
	}
	if (ev_use.compare(EV_DEVPOLL_NAME) == 0) {
		return ev::DEVPOLL;
	}
	if (ev_use.compare(EV_PORT_NAME) == 0) {
		return ev::PORT;
	}
	return -1;
}


const char* ev_backend(unsigned int backend) {
	switch(backend) {
		case ev::SELECT:
			return EV_SELECT_NAME;
		case ev::POLL:
			return EV_POLL_NAME;
		case ev::EPOLL:
			return EV_EPOLL_NAME;
		case ev::KQUEUE:
			return EV_KQUEUE_NAME;
		case ev::DEVPOLL:
			return EV_DEVPOLL_NAME;
		case ev::PORT:
			return EV_PORT_NAME;
	}
	return "unknown";
}


std::vector<std::string> ev_supported() {
	std::vector<std::string> backends;
	unsigned int supported = ev::supported_backends();
	if (supported & ev::SELECT) backends.push_back(EV_SELECT_NAME);
	if (supported & ev::POLL) backends.push_back(EV_POLL_NAME);
	if (supported & ev::EPOLL) backends.push_back(EV_EPOLL_NAME);
	if (supported & ev::KQUEUE) backends.push_back(EV_KQUEUE_NAME);
	if (supported & ev::DEVPOLL) backends.push_back(EV_DEVPOLL_NAME);
	if (supported & ev::PORT) backends.push_back(EV_PORT_NAME);
	if (backends.empty()) {
		backends.push_back("auto");
	}
	return backends;
}


/*
 * This exemplifies how the output class can be overridden to provide
 * user defined output.
 */
class CmdOutput : public StdOutput {
	inline void spacePrint(std::ostream& os, const std::string& s, int maxWidth,
				int indentSpaces, int secondLineOffset, bool endl=true) const {
		int len = static_cast<int>(s.length());

		if ((len + indentSpaces > maxWidth) && maxWidth > 0) {
			int allowedLen = maxWidth - indentSpaces;
			int start = 0;
			while (start < len) {
				// find the substring length
				// int stringLen = std::min<int>( len - start, allowedLen );
				// doing it this way to support a VisualC++ 2005 bug
				using namespace std;
				int stringLen = min<int>(len - start, allowedLen);

				// trim the length so it doesn't end in middle of a word
				if (stringLen == allowedLen) {
					while (stringLen >= 0 &&
						s[stringLen+start] != ' ' &&
						s[stringLen+start] != ',' &&
						s[stringLen+start] != '|') {
						--stringLen;
					}
				}

				// ok, the word is longer than the line, so just split
				// wherever the line ends
				if (stringLen <= 0) {
					stringLen = allowedLen;
				}

				// check for newlines
				for (int i = 0; i < stringLen; ++i) {
					if (s[start+i] == '\n') {
						stringLen = i + 1;
					}
				}

				if (start != 0) {
					os << std::endl;
				}

				// print the indent
				for (int i = 0; i < indentSpaces; ++i) {
					os << " ";
				}

				if (start == 0) {
					// handle second line offsets
					indentSpaces += secondLineOffset;

					// adjust allowed len
					allowedLen -= secondLineOffset;
				}

				os << s.substr(start,stringLen);

				// so we don't start a line with a space
				while (s[stringLen+start] == ' ' && start < len) {
					++start;
				}

				start += stringLen;
			}
		} else {
			for (int i = 0; i < indentSpaces; ++i) {
				os << " ";
			}
			os << s;
			if (endl) {
				os << std::endl;
			}
		}
	}

	inline void _shortUsage(CmdLineInterface& _cmd, std::ostream& os) const {
		std::list<Arg*> argList = _cmd.getArgList();
		std::string progName = _cmd.getProgramName();
		XorHandler xorHandler = _cmd.getXorHandler();
		std::vector<std::vector<Arg*>> xorList = xorHandler.getXorList();

		std::string s = progName + " ";

		// first the xor
		for (size_t i = 0; i < xorList.size(); ++i) {
			s += " {";
			for (auto it = xorList[i].begin(); it != xorList[i].end(); ++it) {
				s += (*it)->shortID() + "|";
			}

			s[s.length() - 1] = '}';
		}

		// then the rest
		for (auto it = argList.begin(); it != argList.end(); ++it) {
			if (!xorHandler.contains((*it))) {
				s += " " + (*it)->shortID();
			}
		}

		// if the program name is too long, then adjust the second line offset
		int secondLineOffset = static_cast<int>(progName.length()) + 2;
		if (secondLineOffset > LINE_LENGTH / 2) {
			secondLineOffset = static_cast<int>(LINE_LENGTH / 2);
		}

		spacePrint(os, s, LINE_LENGTH, 3, secondLineOffset);
	}

	inline void _longUsage(CmdLineInterface& _cmd, std::ostream& os) const {
		std::list<Arg*> argList = _cmd.getArgList();
		std::string message = _cmd.getMessage();
		XorHandler xorHandler = _cmd.getXorHandler();
		std::vector< std::vector<Arg*> > xorList = xorHandler.getXorList();

		size_t max = 0;

		for (size_t i = 0; i < xorList.size(); ++i) {
			for (auto it = xorList[i].begin(); it != xorList[i].end(); ++it) {
				const std::string& id = (*it)->longID();
				if (id.size() > max) {
					max = id.size();
				}
			}
		}

		// first the xor
		for (size_t i = 0; i < xorList.size(); ++i) {
			for (auto it = xorList[i].begin(); it != xorList[i].end(); ++it) {
				const std::string& id = (*it)->longID();
				spacePrint(os, id, (int)max + 3, 3, 3, false);
				spacePrint(os, (*it)->getDescription(), LINE_LENGTH - ((int)max - 10), static_cast<int>((2 + max) - id.size()), static_cast<int>(3 + id.size()), false);

				if (it + 1 != xorList[i].end()) {
					spacePrint(os, "-- OR --", LINE_LENGTH, 9, 0);
				}
			}
			os << std::endl << std::endl;
		}

		// then the rest
		for (auto it = argList.begin(); it != argList.end(); ++it) {
			if (!xorHandler.contains((*it))) {
				const std::string& id = (*it)->longID();
				if (id.size() > max) {
					max = id.size();
				}
			}
		}

		// then the rest
		for (auto it = argList.begin(); it != argList.end(); ++it) {
			if (!xorHandler.contains((*it))) {
				const std::string& id = (*it)->longID();
				spacePrint(os, id, (int)max + 3, 3, 3, false);
				spacePrint(os, (*it)->getDescription(), LINE_LENGTH - ((int)max - 10), static_cast<int>((2 + max) - id.size()), static_cast<int>(3 + id.size()), false);
				os << std::endl;
			}
		}

		os << std::endl;

		if (!message.empty()) {
			spacePrint( os, message, LINE_LENGTH, 3, 0 );
		}
	}

public:
	virtual void failure(CmdLineInterface& _cmd, ArgException& exc) {
		std::string progName = _cmd.getProgramName();

		std::cerr << "Error: " << exc.argId() << std::endl;

		spacePrint(std::cerr, exc.error(), LINE_LENGTH, 3, 0);

		std::cerr << std::endl;

		if (_cmd.hasHelpAndVersion()) {
			std::cerr << "Usage: " << std::endl;

			_shortUsage( _cmd, std::cerr );

			std::cerr << std::endl << "For complete usage and help type: "
					  << std::endl << "   " << progName << " "
					  << Arg::nameStartString() << "help"
					  << std::endl << std::endl;
		} else {
			usage(_cmd);
		}

		throw ExitException(EX_USAGE);
	}

	virtual void usage(CmdLineInterface& _cmd) {
		spacePrint(std::cout, Package::STRING, LINE_LENGTH, 0, 0);
		spacePrint(std::cout, "[" + Package::BUGREPORT + "]", LINE_LENGTH, 0, 0);

		std::cout << std::endl;

		std::cout << "Usage: " << std::endl;

		_shortUsage(_cmd, std::cout);

		std::cout << std::endl << "Where: " << std::endl;

		_longUsage(_cmd, std::cout);
	}

	virtual void version(CmdLineInterface& _cmd) {
		std::string xversion = _cmd.getVersion();
		std::cout << xversion << std::endl;
	}
};


void parseOptions(int argc, char** argv, opts_t &opts) {
	const unsigned int nthreads = std::thread::hardware_concurrency() * SERVERS_MULTIPLIER;

	try {
		CmdLine cmd("", ' ', Package::STRING);

		// ZshCompletionOutput zshoutput;
		// cmd.setOutput(&zshoutput);

		CmdOutput output;
		cmd.setOutput(&output);

		SwitchArg detach("d", "detach", "detach process. (run in background)", cmd);
		MultiSwitchArg verbose("v", "verbose", "Increase verbosity.", cmd);
		ValueArg<unsigned int> verbosity("", "verbosity", "Set verbosity.", false, 0, "verbosity", cmd);

#ifdef XAPIAN_HAS_GLASS_BACKEND
		SwitchArg chert("", "chert", "Prefer Chert databases.", cmd, false);
#endif

		SwitchArg solo("", "solo", "Run solo indexer. (no replication or discovery)", cmd, false);
		SwitchArg strict_arg("", "strict", "Force the user to define the type for each field", cmd, false);
		SwitchArg optimal_arg("", "optimal", "Force the configuration for indexing documents to optimal", cmd, false);

		ValueArg<std::string> database("D", "database", "Path to the root of the node.", false, ".", "path", cmd);
		ValueArg<std::string> cluster_name("", "cluster", "Cluster name to join.", false, XAPIAND_CLUSTER_NAME, "cluster", cmd);
		ValueArg<std::string> node_name("", "name", "Node name.", false, "", "node", cmd);

		ValueArg<unsigned int> http_port("", "port", "TCP HTTP port number to listen on for REST API.", false, XAPIAND_HTTP_SERVERPORT, "port", cmd);
		ValueArg<unsigned int> binary_port("", "xapian", "Xapian binary protocol TCP port number to listen on.", false, XAPIAND_BINARY_SERVERPORT, "port", cmd);

		ValueArg<unsigned int> discovery_port("", "discovery_port", "Discovery UDP port number to listen on.", false, XAPIAND_DISCOVERY_SERVERPORT, "port", cmd);
		ValueArg<std::string> discovery_group("", "dgroup", "Discovery UDP group name.", false, XAPIAND_DISCOVERY_GROUP, "group", cmd);

		ValueArg<unsigned int> raft_port("", "raft_port", "Raft UDP port number to listen on.", false, XAPIAND_RAFT_SERVERPORT, "port", cmd);
		ValueArg<std::string> raft_group("", "rgroup", "Raft UDP group name.", false, XAPIAND_RAFT_GROUP, "group", cmd);

		ValueArg<std::string> pidfile("P", "pidfile", "Save PID in <file>.", false, "", "file", cmd);
		ValueArg<std::string> logfile("L", "logfile", "Save logs in <file>.", false, "", "file", cmd);
		ValueArg<std::string> uid("", "uid", "User ID.", false, "", "uid", cmd);
		ValueArg<std::string> gid("", "gid", "Group ID.", false, "", "gid", cmd);

		auto allowed = ev_supported();
		ValuesConstraint<std::string> constraint(allowed);
		ValueArg<std::string> use("", "use", "Connection processing backend.", false, "auto", &constraint, cmd);

		ValueArg<size_t> num_servers("", "workers", "Number of worker servers.", false, nthreads, "threads", cmd);
		ValueArg<size_t> dbpool_size("", "dbpool_size", "Maximum number of databases in database pool.", false, DBPOOL_SIZE, "size", cmd);
		ValueArg<size_t> num_replicators("", "replicators", "Number of replicators.", false, NUM_REPLICATORS, "replicators", cmd);
		ValueArg<size_t> num_committers("", "committers", "Number of threads handling the commits.", false, NUM_COMMITTERS, "committers", cmd);
		ValueArg<size_t> num_fsynchers("", "fsynchers", "Number of threads handling the fsyncs.", false, NUM_FSYNCHERS, "fsynchers", cmd);
		ValueArg<size_t> max_clients("", "max_clients", "Max number of clients.", false, CONFIG_DEFAULT_MAX_CLIENTS, "clients", cmd);
		ValueArg<size_t> max_files("", "max_files", "Max number of files to open.", false, 0, "files", cmd);

		std::vector<std::string> args;
		for (int i = 0; i < argc; ++i) {
			if (i == 0) {
				const char* a = strrchr(argv[i], '/');
				if (a) {
					++a;
				} else {
					a = argv[i];
				}
				args.push_back(a);
			} else {
				// Split arguments when possible (e.g. -Dnode, --verbosity=3)
				const char* arg = argv[i];
				if (arg[0] == '-') {
					if (arg[1] != '-' && arg[1] != 'v') {  // skip long arguments (e.g. --verbosity) or multiswitch (e.g. -vvv)
						std::string tmp(arg, 2);
						args.push_back(tmp);
						arg += 2;
					}
				}
				const char* a = strchr(arg, '=');
				if (a) {
					if (a - arg) {
						std::string tmp(arg, a - arg);
						args.push_back(tmp);
					}
					arg = a + 1;
				}
				if (*arg) {
					args.push_back(arg);
				}
			}
		}
		cmd.parse(args);

		opts.verbosity = verbosity.getValue() + verbose.getValue();
		opts.detach = detach.getValue();
#ifdef XAPIAN_HAS_GLASS_BACKEND
		opts.chert = chert.getValue();
#else
		opts.chert = true;
#endif

		opts.solo = solo.getValue();
		opts.strict = strict_arg.getValue();
		opts.optimal = optimal_arg.getValue();

		opts.database = database.getValue();
		opts.cluster_name = cluster_name.getValue();
		opts.node_name = node_name.getValue();
		opts.http_port = http_port.getValue();
		opts.binary_port = binary_port.getValue();
		opts.discovery_port = discovery_port.getValue();
		opts.discovery_group = discovery_group.getValue();
		opts.raft_port = raft_port.getValue();
		opts.raft_group = raft_group.getValue();
		opts.pidfile = pidfile.getValue();
		opts.logfile = logfile.getValue();
		opts.uid = uid.getValue();
		opts.gid = gid.getValue();
		opts.num_servers = num_servers.getValue();
		opts.dbpool_size = dbpool_size.getValue();
		opts.num_replicators = opts.solo ? 0 : num_replicators.getValue();
		opts.num_committers = num_committers.getValue();
		opts.num_fsynchers = num_fsynchers.getValue();
		opts.max_clients = max_clients.getValue();
		opts.max_files = max_files.getValue();
		opts.threadpool_size = THEADPOOL_SIZE;
		opts.endpoints_list_size = ENDPOINT_LIST_SIZE;
		if (opts.detach) {
			if (opts.logfile.empty()) {
				opts.logfile = XAPIAND_LOG_FILE;
			}
			if (opts.pidfile.empty()) {
				opts.pidfile = XAPIAND_PID_FILE;
			}
		}
		opts.ev_flags = ev_backend(use.getValue());
	} catch (const ArgException& exc) { // catch any exceptions
		std::cerr << "error: " << exc.error() << " for arg " << exc.argId() << std::endl;
	}
}


/*
 * From https://github.com/antirez/redis/blob/b46239e58b00774d121de89e0e033b2ed3181eb0/src/server.c#L1496
 *
 * This function will try to raise the max number of open files accordingly to
 * the configured max number of clients. It also reserves a number of file
 * descriptors for extra operations of persistence, listening sockets, log files and so forth.
 *
 * If it will not be possible to set the limit accordingly to the configured
 * max number of clients, the function will do the reverse setting
 * to the value that we can actually handle.
 */
ssize_t get_max_files_per_proc()
{
	int32_t max_files_per_proc = 0;

#if defined(KERN_MAXFILESPERPROC)
#define _SYSCTL_NAME "kern.maxfilesperproc"  // FreeBSD, Apple
	int mib[] = {CTL_KERN, KERN_MAXFILESPERPROC};
	size_t mib_len = sizeof(mib) / sizeof(int);
#endif
#ifdef _SYSCTL_NAME
	auto max_files_per_proc_len = sizeof(max_files_per_proc);
	if (sysctl(mib, mib_len, &max_files_per_proc, &max_files_per_proc_len, nullptr, 0) < 0) {
		L_ERR(nullptr, "ERROR: Unable to get max files per process: sysctl(" _SYSCTL_NAME "): [%d] %s", errno, strerror(errno));
	}
#undef _SYSCTL_NAME
#else
	L_WARNING(nullptr, "WARNING: No way of getting max files per process.");
#endif

	return max_files_per_proc;
}


ssize_t get_open_files()
{
	int32_t max_files_per_proc = 0;

#if defined(KERN_OPENFILES)
#define _SYSCTL_NAME "kern.openfiles"  // FreeBSD
	int mib[] = {CTL_KERN, KERN_OPENFILES};
	size_t mib_len = sizeof(mib) / sizeof(int);
#elif defined(__APPLE__)
#define _SYSCTL_NAME "kern.num_files"  // Apple
	int mib[CTL_MAXNAME + 2];
	size_t mib_len = sizeof(mib) / sizeof(int);
	if (sysctlnametomib(_SYSCTL_NAME, mib, &mib_len) < 0) {
		L_ERR(nullptr, "ERROR: sysctl(" _SYSCTL_NAME "): [%d] %s", errno, strerror(errno));
		return 0;
	}
#endif
#ifdef _SYSCTL_NAME
	auto max_files_per_proc_len = sizeof(max_files_per_proc);
	if (sysctl(mib, mib_len, &max_files_per_proc, &max_files_per_proc_len, nullptr, 0) < 0) {
		L_ERR(nullptr, "ERROR: Unable to get number of open files: sysctl(" _SYSCTL_NAME "): [%d] %s", errno, strerror(errno));
	}
#undef _SYSCTL_NAME
#else
	L_WARNING(nullptr, "WARNING: No way of getting number of open files.");
#endif

	return max_files_per_proc;
}


void adjustOpenFilesLimit(opts_t &opts) {

	// Try getting the currently available number of files:
	ssize_t available_files = get_max_files_per_proc() - get_open_files();


	// Try calculating minimum and recommended number of files:
	ssize_t new_dbpool_size;
	ssize_t new_max_clients;
	ssize_t files = 1;
	ssize_t minimum_files = 1;
	ssize_t recommended_files = 1;

	while (true) {
		ssize_t used_files = FDS_RESERVED;

		used_files += FDS_PER_DATABASE;
		new_dbpool_size = (files - used_files) / FDS_PER_DATABASE;
		if (new_dbpool_size > opts.dbpool_size) {
			new_dbpool_size = opts.dbpool_size;
		}
		used_files += (new_dbpool_size + 1) * FDS_PER_DATABASE;

		used_files += FDS_PER_CLIENT;
		new_max_clients = (files - used_files) / FDS_PER_CLIENT;
		if (new_max_clients > opts.max_clients) {
			new_max_clients = opts.max_clients;
		}
		used_files += (new_max_clients + 1) * FDS_PER_CLIENT;

		if (new_dbpool_size < 1 || new_max_clients < 1) {
			files = minimum_files = used_files;
		} else if (new_dbpool_size < opts.dbpool_size || new_max_clients < opts.max_clients) {
			files = recommended_files = used_files;
		} else {
			break;
		}
	}


	// Calculate max_files (from configuration, recommended and available numbers):
	ssize_t max_files = opts.max_files;
	if (max_files) {
		// Decrese whatever the user requested to the amount of available files:
		if (max_files > available_files) {
			L_WARNING(nullptr, "Requested maximum number of files (%zd) capped to the maximum number of currently available files (%zd)", max_files, available_files);
			max_files = available_files;
		}
	} else {
		if (max_files < recommended_files) {
			max_files = recommended_files;
		}
		if (max_files > available_files) {
			max_files = available_files;
		}
	}


	// Try getting current limit of files:
	rlim_t limit_cur_files;
	struct rlimit limit;
	if (getrlimit(RLIMIT_NOFILE, &limit) == -1) {
		limit_cur_files = available_files;
		if (!limit_cur_files) {
			limit_cur_files = 4000;
		}
		L_WARNING(nullptr, "Unable to obtain the current NOFILE limit (%s), assuming %d", strerror(errno), limit_cur_files);
	} else {
		limit_cur_files = limit.rlim_cur;
	}


	// Set the the max number of files:
	// Increase if the current limit is not enough for our needs or
	// decrease if the user requests it.
	if (opts.max_files || limit_cur_files < max_files) {
		bool increasing = limit_cur_files < max_files;

		const rlim_t step = 16;
		int setrlimit_error = 0;

		// Try to set the file limit to match 'max_files' or at least to the higher value supported less than max_files.
		rlim_t new_max_files = max_files;
		while (new_max_files != limit_cur_files) {
			limit.rlim_cur = new_max_files;
			limit.rlim_max = new_max_files;
			if (setrlimit(RLIMIT_NOFILE, &limit) != -1) {
				max_files = new_max_files;
				if (increasing) {
					L_INFO(nullptr, "Increased maximum number of open files to %zd (it was originally set to %zd)", max_files, (size_t)limit_cur_files);
				} else {
					L_INFO(nullptr, "Decresed maximum number of open files to %zd (it was originally set to %zd)", max_files, (size_t)limit_cur_files);
				}
				break;
			}

			// We failed to set file limit to 'new_max_files'. Try with a smaller limit decrementing by a few FDs per iteration.
			setrlimit_error = errno;
			if (!increasing || new_max_files < step) {
				// Assume that the limit we get initially is still valid if our last try was even lower.
				new_max_files = limit_cur_files;
				break;
			}
			new_max_files -= step;
		}

		if (setrlimit_error) {
			L_ERR(nullptr, "Server can't set maximum open files to %zd because of OS error: %s", max_files, strerror(setrlimit_error));
		}
	}


	// Calculate dbpool_size and max_clients from current max_files:
	files = max_files;
	ssize_t used_files = FDS_RESERVED;
	used_files += FDS_PER_DATABASE;
	new_dbpool_size = (files - used_files) / FDS_PER_DATABASE;
	if (new_dbpool_size > opts.dbpool_size) {
		new_dbpool_size = opts.dbpool_size;
	}
	used_files += (new_dbpool_size + 1) * FDS_PER_DATABASE;
	used_files += FDS_PER_CLIENT;
	new_max_clients = (files - used_files) / FDS_PER_CLIENT;
	if (new_max_clients > opts.max_clients) {
		new_max_clients = opts.max_clients;
	}


	// Warn about changes to the configured dbpool_size or max_clients:
	if (new_dbpool_size > 0 && new_dbpool_size < opts.dbpool_size) {
		L_WARNING(nullptr, "You requested a dbpool_size of %zd requiring at least %zd max file descriptors", opts.dbpool_size, (opts.dbpool_size + 1) * FDS_PER_DATABASE  + FDS_RESERVED);
		L_WARNING(nullptr, "Current maximum open files is %zd so dbpool_size has been reduced to %zd to compensate for low limit.", max_files, new_dbpool_size);
	}
	if (new_max_clients > 0 && new_max_clients < opts.max_clients) {
		L_WARNING(nullptr, "You requested max_clients of %zd requiring at least %zd max file descriptors", opts.max_clients, (opts.max_clients + 1) * FDS_PER_CLIENT + FDS_RESERVED);
		L_WARNING(nullptr, "Current maximum open files is %zd so max_clients has been reduced to %zd to compensate for low limit.", max_files, new_max_clients);
	}

	// Warn about minimum/recommended sizes:
	if (max_files < minimum_files) {
		L_CRIT(nullptr, "Your current 'ulimit -n' of %zd is not enough for the server to start. Please increase your open file limit to at least %zd",
			max_files,
			minimum_files);
		throw Exit(EX_OSFILE);
	} else if (max_files < recommended_files) {
		L_WARNING(nullptr, "Your current max_files of %zd is not enough. Please increase your open file limit to at least %zd",
			max_files,
			recommended_files);
	}

	if ((new_dbpool_size < opts.dbpool_size || new_max_clients < opts.max_clients) && (!opts.max_files || max_files < opts.max_files)) {
		L_WARNING(nullptr, "If you need higher dbpool_size or max_clients increase 'ulimit -n'");
	}


	// Set new values:
	opts.max_files = max_files;
	opts.dbpool_size = new_dbpool_size;
	opts.max_clients = new_max_clients;
}


void demote(const char* username, const char* group) {
	/* lose root privileges if we have them */
	uid_t uid = getuid();
	if (uid == 0 || geteuid() == 0) {
		if (username == nullptr || *username == '\0') {
			L_CRIT(nullptr, "Can't run as root without the --uid switch");
			throw Exit(EX_USAGE);

		}

		struct passwd *pw;
		struct group *gr;

		if ((pw = getpwnam(username)) == nullptr) {
			uid = atoi(username);
			if (!uid || (pw = getpwuid(uid)) == nullptr) {
				L_CRIT(nullptr, "Can't find the user %s to switch to", username);
				throw Exit(EX_NOUSER);
			}
		}
		uid = pw->pw_uid;
		gid_t gid = pw->pw_gid;
		username = pw->pw_name;

		if (group && *group) {
			if ((gr = getgrnam(group)) == nullptr) {
				gid = atoi(group);
				if (!gid || (gr = getgrgid(gid)) == nullptr) {
					L_CRIT(nullptr, "Can't find the group %s to switch to", group);
					throw Exit(EX_NOUSER);
				}
			}
			gid = gr->gr_gid;
			group = gr->gr_name;
		} else {
			if ((gr = getgrgid(gid)) == nullptr) {
				L_CRIT(nullptr, "Can't find the group id %d", gid);
				throw Exit(EX_NOUSER);
			}
			group = gr->gr_name;
		}
		if (setgid(gid) < 0 || setuid(uid) < 0) {
			L_CRIT(nullptr, "Failed to assume identity of %s:%s", username, group);
			throw Exit(EX_OSERR);
		}
		L_NOTICE(nullptr, "Running as %s:%s", username, group);
	}
}


void detach() {
	pid_t pid = fork();
	if (pid != 0) {
		exit(EX_OK); /* parent exits */
	}
	setsid(); /* create a new session */

	/* Every output goes to /dev/null */
	int fd;
	if ((fd = io::open("/dev/null", O_RDWR, 0)) != -1) {
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		if (fd > STDERR_FILENO) io::close(fd);
	}
}


void writepid(const char* pidfile) {
	if (pidfile != nullptr && *pidfile != '\0') {
		/* Try to write the pid file in a best-effort way. */
		int fd = io::open(pidfile, O_RDWR | O_CREAT, 0644);
		if (fd > 0) {
			char buffer[100];
			snprintf(buffer, sizeof(buffer), "%lu\n", (unsigned long)getpid());
			io::write(fd, buffer, strlen(buffer));
			io::close(fd);
		}
	}
}


void usedir(const char* path, bool solo) {

#ifdef XAPIAND_CLUSTERING
	if (!solo) {
		DIR *dirp;
		dirp = opendir(path, true);
		if (!dirp) {
			L_CRIT(nullptr, "Cannot open working directory: %s", path);
			throw Exit(EX_OSFILE);
		}

		bool empty = true;
		struct dirent *ent;
		while ((ent = readdir(dirp)) != nullptr) {
			const char *s = ent->d_name;
			if (ent->d_type == DT_DIR) {
				continue;
			}
			if (ent->d_type == DT_REG) {
#if defined(__APPLE__) && defined(__MACH__)
				if (ent->d_namlen == 9 && strcmp(s, "flintlock") == 0)
#else
					if (strcmp(s, "flintlock") == 0)
#endif
					{
						empty = true;
						break;
					}
			}
			empty = false;
		}
		closedir(dirp);

		if (!empty) {
			L_CRIT(nullptr, "Working directory must be empty or a valid xapian database: %s", path);
			throw Exit(EX_DATAERR);
		}
	}
#else
	(void)solo;  // silence -Wunused-parameter
#endif

	if (chdir(path) == -1) {
		L_CRIT(nullptr, "Cannot change current working directory to %s", path);
		throw Exit(EX_OSFILE);
	}

	char buffer[PATH_MAX];
	getcwd(buffer, sizeof(buffer));
	Endpoint::cwd = normalize_path(buffer, buffer, true);  // Endpoint::cwd must always end with slash
	L_NOTICE(nullptr, "Changed current working directory to %s", Endpoint::cwd.c_str());
}


void banner() {
	set_thread_name("-=-");

	L_INFO(nullptr,
		"\n\n" +
		rgb(255, 255, 255) + "     __  __           _                 _\n" +
		rgb(224, 224, 224) + "     \\ \\/ /__ _ _ __ (_) __ _ _ __   __| |\n" +
		rgb(192, 192, 192) + "      \\  // _` | '_ \\| |/ _` | '_ \\ / _` |\n" +
		rgb(160, 160, 160) + "      /  \\ (_| | |_) | | (_| | | | | (_| |\n" +
		rgb(128, 128, 128) + "     /_/\\_\\__,_| .__/|_|\\__,_|_| |_|\\__,_|\n" +
		rgb(96, 96, 96)    + "               |_|" + LIGHT_GREEN + center_string(Package::STRING, 25) + "\n" + GREEN +
		center_string("[" + Package::BUGREPORT + "]", 46) + "\n" +
		center_string("Using " + join_string(std::vector<std::string>{
			format_string("Xapian v%d.%d.%d", Xapian::major_version(), Xapian::minor_version(), Xapian::revision()),
#if defined(XAPIAND_V8)
			format_string("V8 v%u.%u", V8_MAJOR_VERSION, V8_MINOR_VERSION),
#endif
#if defined(XAPIAND_CHAISCRIPT)
			format_string("ChaiScript v%d.%d", chaiscript::Build_Info::version_major(), chaiscript::Build_Info::version_minor()),
#endif
		}, ", ", " and "), 46) + "\n\n");
}


void run(const opts_t &opts) {
	usedir(opts.database.c_str(), opts.solo);

	demote(opts.uid.c_str(), opts.gid.c_str());

	setup_signal_handlers();
	ev::default_loop default_loop(opts.ev_flags);

	L_INFO(nullptr, "Connection processing backend: %s", ev_backend(default_loop.backend()));

	XapiandManager::manager = Worker::make_shared<XapiandManager>(&default_loop, opts.ev_flags, opts);
	try {
		XapiandManager::manager->run(opts);
	} catch (...) {
		XapiandManager::manager.reset();
		throw;
	}

	long managers = XapiandManager::manager.use_count() - 1;
	if (managers == 0) {
		L_NOTICE(nullptr, "Xapiand is cleanly done with all work!");
	} else {
		L_WARNING(nullptr, "Xapiand is uncleanly done with all work (%ld)!\n%s", managers, XapiandManager::manager->dump_tree().c_str());
	}
	XapiandManager::manager.reset();
}


int main(int argc, char **argv) {
	int exit_code = EX_OK;

	opts_t opts;

	parseOptions(argc, argv, opts);

	std::setlocale(LC_CTYPE, "");

	if (opts.detach) {
		detach();
		writepid(opts.pidfile.c_str());
	}

	// Logging thread must be created after fork the parent process
	auto& handlers = Logging::handlers;
	if (opts.logfile.compare("syslog") == 0) {
		handlers.push_back(std::make_unique<SysLog>());
	} else if (!opts.logfile.empty()) {
		handlers.push_back(std::make_unique<StreamLogger>(opts.logfile.c_str()));
	}
	if (!opts.detach || handlers.empty()) {
		handlers.push_back(std::make_unique<StderrLogger>());
	}

	Logging::log_level += opts.verbosity;

	try {

		banner();

		if (opts.detach) {
			L_NOTICE(nullptr, "Xapiand is done with all work here. Daemon on process ID [%d] taking over!", getpid());
		}

		usleep(100000ULL);

		L_NOTICE(nullptr, Package::STRING + " started.");

		// Flush threshold increased
		int flush_threshold = 10000;  // Default is 10000 (if no set)
		const char *p = getenv("XAPIAN_FLUSH_THRESHOLD");
		if (p) flush_threshold = atoi(p);
		if (flush_threshold < 100000 && setenv("XAPIAN_FLUSH_THRESHOLD", "100000", false) == 0) {
			L_INFO(nullptr, "Increased database flush threshold to 100000 (it was originally set to %d).", flush_threshold);
		}

#ifdef XAPIAN_HAS_GLASS_BACKEND
		if (!opts.chert) {
			// Prefer glass database
			if (setenv("XAPIAN_PREFER_GLASS", "1", false) != 0) {
				opts.chert = true;
			}
		}
#endif
		if (opts.chert) {
			L_INFO(nullptr, "Using Chert databases by default.");
		} else {
			L_INFO(nullptr, "Using Glass databases by default.");
		}

		std::vector<std::string> modes;
		if (opts.strict) {
			modes.push_back("strict");
			default_spc.flags.strict = true;
		}

		if (opts.optimal) {
			modes.push_back("optimal");
			default_spc.flags.optimal = true;
			default_spc.index = TypeIndex::FIELD_ALL;
			default_spc.flags.text_detection = false;
			default_spc.index_uuid_field = UUIDFieldIndex::UUID;
		}
		if (!modes.empty()) {
			L_INFO(nullptr, "Activated " + join_string(modes, ", ", " and ") + ((modes.size() == 1) ? " mode by default." : " modes by default."));
		}

		adjustOpenFilesLimit(opts);

		L_INFO(nullptr, "With a maximum of " + join_string(std::vector<std::string>{
			std::to_string(opts.max_files) + ((opts.max_files == 1) ? " file" : " files"),
			std::to_string(opts.max_clients) + ((opts.max_clients == 1) ? " client" : " clients"),
			std::to_string(opts.dbpool_size) + ((opts.dbpool_size == 1) ? " database" : " databases"),
		}, ", ", " and ", [](const auto& s) { return s.empty(); }));

		run(opts);

	} catch (const Exit& exc) {
		exit_code = exc.code;
	}

	if (opts.detach && !opts.pidfile.empty()) {
		L_INFO(nullptr, "Removing the pid file.");
		unlink(opts.pidfile.c_str());
	}

	Logging::finish();
	return exit_code;
}
