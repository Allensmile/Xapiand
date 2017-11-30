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
#include <sys/resource.h>            // for rlimit
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

#include "cmdoutput.h"               // for CmdOutput
#include "endpoint.h"                // for Endpoint, Endpoint::cwd
#include "ev/ev++.h"                 // for ::DEVPOLL, ::EPOLL, ::KQUEUE
#include "exception.h"               // for Exit
#include "ignore_unused.h"           // for ignore_unused
#include "io_utils.h"                // for close, open, write
#include "log.h"                     // for Logging, L_INFO, L_CRIT, L_NOTICE
#include "manager.h"                 // for opts_t, XapiandManager, XapiandM...
#include "schema.h"                  // for default_spc
#include "utils.h"                   // for format_string, center_string
#include "worker.h"                  // for Worker
#include "xxh64.hpp"                 // for xxh64

#include <sys/sysctl.h>              // for sysctl, sysctlnametomib...


#define FDS_RESERVED     50          // Is there a better approach?
#define FDS_PER_CLIENT    2          // KQUEUE + IPv4
#define FDS_PER_DATABASE  7          // Writable~=7, Readable~=5


template<typename T, std::size_t N>
ssize_t write(int fildes, T (&buf)[N]) {
	return write(fildes, buf, N - 1);
}

ssize_t write(int fildes, const std::string& buf) {
	if ((isatty(fildes) || Logging::colors) && !Logging::no_colors) {
		return write(fildes, buf.data(), buf.size());
	} else {
		auto buf2 = Logging::decolorize(buf);
		return write(fildes, buf2.data(), buf2.size());
	}
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
	write(STDERR_FILENO, msg);

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


void parseOptions(int argc, char** argv, opts_t &opts) {
	const unsigned int nthreads = std::thread::hardware_concurrency() * SERVERS_MULTIPLIER;

	using namespace TCLAP;

	try {
		CmdLine cmd("", ' ', Package::STRING);

		// ZshCompletionOutput zshoutput;
		// cmd.setOutput(&zshoutput);

		CmdOutput output;
		cmd.setOutput(&output);

		MultiSwitchArg verbose("v", "verbose", "Increase verbosity.", cmd);
		ValueArg<unsigned int> verbosity("", "verbosity", "Set verbosity.", false, 0, "verbosity", cmd);

#ifdef XAPIAN_HAS_GLASS_BACKEND
		SwitchArg chert("", "chert", "Prefer Chert databases.", cmd, false);
#endif

		std::vector<std::string> uuid_repr_allowed({
#ifdef UUID_USE_BASE16
			"base16",
#endif
#ifdef UUID_USE_BASE58
			"base58",
#endif
#ifdef UUID_USE_BASE62
			"base62",
#endif
#ifdef UUID_USE_GUID
			"guid",
#endif
#ifdef UUID_USE_URN
			"urn",
#endif
			"simple",
		});
		ValuesConstraint<std::string> uuid_repr_constraint(uuid_repr_allowed);
		ValueArg<std::string> uuid_repr("", "uuid", "UUID normalizer.", false, "auto", &uuid_repr_constraint, cmd);
		SwitchArg uuid_compact("", "uuid-compact", "Generate compact UUIDs.", cmd, false);

		ValueArg<unsigned int> raft_port("", "raft-port", "Raft UDP port number to listen on.", false, XAPIAND_RAFT_SERVERPORT, "port", cmd);
		ValueArg<std::string> raft_group("", "raft-group", "Raft UDP group name.", false, XAPIAND_RAFT_GROUP, "group", cmd);

		ValueArg<unsigned int> discovery_port("", "discovery-port", "Discovery UDP port number to listen on.", false, XAPIAND_DISCOVERY_SERVERPORT, "port", cmd);
		ValueArg<std::string> discovery_group("", "discovery-group", "Discovery UDP group name.", false, XAPIAND_DISCOVERY_GROUP, "group", cmd);
		ValueArg<std::string> cluster_name("", "cluster", "Cluster name to join.", false, XAPIAND_CLUSTER_NAME, "cluster", cmd);
		ValueArg<std::string> node_name("", "name", "Node name.", false, "", "node", cmd);

		ValueArg<size_t> num_replicators("", "replicators", "Number of replicators.", false, NUM_REPLICATORS, "replicators", cmd);

		ValueArg<size_t> num_committers("", "committers", "Number of threads handling the commits.", false, NUM_COMMITTERS, "committers", cmd);
		ValueArg<size_t> max_databases("", "max-databases", "Max number of open databases.", false, MAX_DATABASES, "databases", cmd);
		ValueArg<size_t> dbpool_size("", "dbpool-size", "Maximum number of databases in database pool.", false, DBPOOL_SIZE, "size", cmd);

		ValueArg<size_t> num_fsynchers("", "fsynchers", "Number of threads handling the fsyncs.", false, NUM_FSYNCHERS, "fsynchers", cmd);
		ValueArg<size_t> max_files("", "max-files", "Max number of files to open.", false, 0, "files", cmd);

		ValueArg<size_t> max_clients("", "max-clients", "Max number of open client connections.", false, MAX_CLIENTS, "clients", cmd);
		ValueArg<size_t> num_servers("", "workers", "Number of worker servers.", false, nthreads, "threads", cmd);

		auto use_allowed = ev_supported();
		ValuesConstraint<std::string> use_constraint(use_allowed);
		ValueArg<std::string> use("", "use", "Connection processing backend.", false, "auto", &use_constraint, cmd);

		ValueArg<unsigned int> binary_port("", "xapian-port", "Xapian binary protocol TCP port number to listen on.", false, XAPIAND_BINARY_SERVERPORT, "port", cmd);
		ValueArg<unsigned int> http_port("", "port", "TCP HTTP port number to listen on for REST API.", false, XAPIAND_HTTP_SERVERPORT, "port", cmd);

		ValueArg<std::string> gid("", "gid", "Group ID.", false, "", "gid", cmd);
		ValueArg<std::string> uid("", "uid", "User ID.", false, "", "uid", cmd);

		ValueArg<std::string> pidfile("P", "pidfile", "Save PID in <file>.", false, "", "file", cmd);
		ValueArg<std::string> logfile("L", "logfile", "Save logs in <file>.", false, "", "file", cmd);

		SwitchArg no_colors("", "no-colors", "Disables colors on the console.", cmd, false);
		SwitchArg colors("", "colors", "Enables colors on the console.", cmd, false);

		SwitchArg detach("d", "detach", "detach process. (run in background)", cmd);
#ifdef XAPIAND_CLUSTERING
		SwitchArg solo("", "solo", "Run solo indexer. (no replication or discovery)", cmd, false);
#endif
		SwitchArg optimal_arg("", "optimal", "Minimal optimal indexing configuration.", cmd, false);
		SwitchArg strict_arg("", "strict", "Force the user to define the type for each field.", cmd, false);
		ValueArg<std::string> database("D", "database", "Path to the root of the node.", false, ".", "path", cmd);

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

#ifdef XAPIAND_CLUSTERING
		opts.solo = solo.getValue();
#else
		opts.solo = true;
#endif
		opts.strict = strict_arg.getValue();
		opts.optimal = optimal_arg.getValue();

		opts.colors = colors.getValue();
		opts.no_colors = no_colors.getValue();

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
		opts.max_databases = max_databases.getValue();
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
		opts.uuid_compact = uuid_compact.getValue();
		auto uuid_repr_str = uuid_repr.getValue();
		opts.uuid_repr = UUIDRepr::simple;
#ifdef UUID_USE_BASE16
		if (uuid_repr_str == "base16") {
			opts.uuid_repr = UUIDRepr::base16;
		}
#endif
#ifdef UUID_USE_BASE58
		// Properties of encoded UUIDs in Base58 (with dubaluchk)

		// Compacted UUIDs (v1):
		//   * Will start with 'du' until 2032 (14 characters).
		//   * Will start with 'de' until 2081 (14 characters).
		//   * Will start with 'dl' until 2094 (14 characters).
		//   * Will start with 'z' from 2094 and for a long time (15 characters).
		//   * From 2017 to 2040, prefixes will be:
		//       duj, duc, duf, dug,
		//       duh, duk, dum, dui, duu, dul, dun, dup, dur, dus,
		//       dut, duv, dex, deq, deB, dey, deD, dez, deF, deG
		//   * Easter egg base58 UUIDs created on Jul 13, 2020 will start with 'dubalu'
		//     from 17:15:29 to 17:17:16 UTC  (0x01eac5565c065680 - 0x01eac5569bcd3e00)

		// Expanded UUIDs (v1):
		//   * Will start with 'DU' for a very very long time (22 characters).
		//   * From 2017 to 2040, prefixes will be:
		//       DUqv, DUBy, DUBG, DUBM,
		//       DUBE, DUBa, DUBY, DUBW, DUBe, DUBX, DUJd, DUJQ, DUJw, DUJ7,
		//       DUJ4, DUJL, DUJf, DUJm, DUJn, DUJt, DUyB, DUyF, DUyA, DUyO
		//   * Easter egg base58 UUIDs created on Jul 13, 2020 will start with 'DUBALU'
		//     from 4:19:59 to 4:46:07 UTC  (0x01eac4ea05fbc980 - 0x01eac4edac959980)

		if (uuid_repr_str == "base58") {
			opts.uuid_repr = UUIDRepr::base58;
		}
#endif
#ifdef UUID_USE_BASE62
		if (uuid_repr_str == "base62") {
			opts.uuid_repr = UUIDRepr::base62;
		}
#endif
#ifdef UUID_USE_GUID
		if (uuid_repr_str == "guid") {
			opts.uuid_repr = UUIDRepr::guid;
		}
#endif
#ifdef UUID_USE_URN
		if (uuid_repr_str == "urn") {
			opts.uuid_repr = UUIDRepr::urn;
		}
#endif
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

	// Try getting the currently available number of files (-10%):
	ssize_t available_files = get_max_files_per_proc() - get_open_files();
	ssize_t aprox_available_files = (available_files * 8) / 10;


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
		if (max_files > aprox_available_files) {
			L_WARNING(nullptr, "The requested open files limit of %zd %s the system-wide currently available number of files: %zd", max_files, max_files > available_files ? "exceeds" : "almost exceeds", available_files);
		}
	} else {
		max_files = recommended_files;
		if (max_files > aprox_available_files) {
			L_WARNING(nullptr, "The minimum recommended open files limit of %zd %s the system-wide currently available number of files: %zd", max_files, max_files > available_files ? "exceeds" : "almost exceeds", available_files);
		}
	}


	// Try getting current limit of files:
	ssize_t limit_cur_files;
	struct rlimit limit;
	if (getrlimit(RLIMIT_NOFILE, &limit) == -1) {
		limit_cur_files = available_files;
		if (!limit_cur_files || limit_cur_files > 4000) {
			limit_cur_files = 4000;
		}
		L_WARNING(nullptr, "Unable to obtain the current NOFILE limit (%s), assuming %zd", strerror(errno), limit_cur_files);
	} else {
		limit_cur_files = limit.rlim_cur;
	}


	// Set the the max number of files:
	// Increase if the current limit is not enough for our needs or
	// decrease if the user requests it.
	if (opts.max_files || limit_cur_files < max_files) {
		bool increasing = limit_cur_files < max_files;

		const ssize_t step = 16;
		int setrlimit_error = 0;

		// Try to set the file limit to match 'max_files' or at least to the higher value supported less than max_files.
		ssize_t new_max_files = max_files;
		while (new_max_files != limit_cur_files) {
			limit.rlim_cur = static_cast<rlim_t>(new_max_files);
			limit.rlim_max = static_cast<rlim_t>(new_max_files);
			if (setrlimit(RLIMIT_NOFILE, &limit) != -1) {
				if (increasing) {
					L_INFO(nullptr, "Increased maximum number of open files to %zd (it was originally set to %zd)", new_max_files, (size_t)limit_cur_files);
				} else {
					L_INFO(nullptr, "Decresed maximum number of open files to %zd (it was originally set to %zd)", new_max_files, (size_t)limit_cur_files);
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
		max_files = new_max_files;
	} else {
		max_files = limit_cur_files;
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
		L_CRIT(nullptr, "Your open files limit of %zd is not enough for the server to start. Please increase your system-wide open files limit to at least %zd",
			max_files,
			minimum_files);
		L_WARNING(nullptr, "If you need to increase your system-wide open files limit use 'ulimit -n'");
		throw Exit(EX_OSFILE);
	} else if (max_files < recommended_files) {
		L_WARNING(nullptr, "Your current max_files of %zd is not enough. Please increase your system-wide open files limit to at least %zd",
			max_files,
			recommended_files);
		L_WARNING(nullptr, "If you need to increase your system-wide open files limit use 'ulimit -n'");
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
	ignore_unused(solo);
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

	std::vector<std::string> values({
			format_string("Xapian v%d.%d.%d", Xapian::major_version(), Xapian::minor_version(), Xapian::revision()),
#if defined(XAPIAND_V8)
			format_string("V8 v%u.%u", V8_MAJOR_VERSION, V8_MINOR_VERSION),
#endif
#if defined(XAPIAND_CHAISCRIPT)
			format_string("ChaiScript v%d.%d", chaiscript::Build_Info::version_major(), chaiscript::Build_Info::version_minor()),
#endif
	});

	L_INFO(nullptr,
		"\n\n" +
		rgb(255, 255, 255) + "     __  __           _                 _\n" +
		rgb(224, 224, 224) + "     \\ \\/ /__ _ _ __ (_) __ _ _ __   __| |\n" +
		rgb(192, 192, 192) + "      \\  // _` | '_ \\| |/ _` | '_ \\ / _` |\n" +
		rgb(160, 160, 160) + "      /  \\ (_| | |_) | | (_| | | | | (_| |\n" +
		rgb(128, 128, 128) + "     /_/\\_\\__,_| .__/|_|\\__,_|_| |_|\\__,_|\n" +
		rgb(96, 96, 96)    + "     " + LIGHT_GREEN + center_string(Package::HASH, 11) + rgb(96, 96, 96) +" |_|" + LIGHT_GREEN + center_string(Package::FULLVERSION, 25) + "\n" + GREEN +
		center_string("[" + Package::BUGREPORT + "]", 46) + "\n" +
		center_string("Using " + join_string(values, ", ", " and "), 46) + "\n\n");
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
	Logging::colors = opts.colors;
	Logging::no_colors = opts.no_colors;

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
			std::to_string(opts.max_databases) + ((opts.max_databases == 1) ? " database" : " databases"),
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
