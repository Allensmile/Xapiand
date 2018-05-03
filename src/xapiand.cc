/*
 * Copyright (C) 2015-2018 Dubalu LLC. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "xapiand.h"

#include <algorithm>                 // for min
#include <chrono>                    // for system_clock, time_point
#include <clocale>                   // for std::setlocale, LC_CTYPE
#include <csignal>                   // for sigaction, sigemptyset
#include <cstdio>                    // for std::fprintf, std::snprintf
#include <cstdlib>                   // for std::size_t, std::atoi, std::getenv, std::exit, setenv
#include <cstring>                   // for std::strchr, std::strlen, std::strrchr, std::strcmp, std::strerror
#include <grp.h>                     // for getgrgid, group, getgrnam, gid_t
#include <iostream>                  // for operator<<, basic_ostream, ostream
#include <list>                      // for __list_iterator, list, operator!=
#include <memory>                    // for unique_ptr, allocator, make_unique
#include <pwd.h>                     // for passwd, getpwnam, getpwuid
#include <sstream>                   // for basic_stringbuf<>::int_type, bas...
#include <strings.h>                 // for strcasecmp
#include <sys/errno.h>               // for errno
#include <sys/fcntl.h>               // for O_RDWR, O_CREAT
#include <sys/resource.h>            // for rlimit
#include <sys/signal.h>              // for sigaction, signal, SIG_IGN, SIGHUP
#include <sysexits.h>                // for EX_NOUSER, EX_OK, EX_USAGE, EX_O...
#include <thread>                    // for thread
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
#include "database_handler.h"        // for DatabaseHandler
#include "endpoint.h"                // for Endpoint, Endpoint::cwd
#include "ev/ev++.h"                 // for ::DEVPOLL, ::EPOLL, ::KQUEUE
#include "exception.h"               // for Exit
#include "ignore_unused.h"           // for ignore_unused
#include "io_utils.h"                // for io::close, io::open, io::write, io::strerrno
#include "log.h"                     // for Logging, L_INFO, L_CRIT, L_NOTICE
#include "manager.h"                 // for XapiandManager, XapiandM...
#include "opts.h"                    // for opts_t
#include "schema.h"                  // for default_spc
#include "string.hh"                 // for string::format, string::center
#include "worker.h"                  // for Worker
#include "hashes.hh"                 // for fnv1ah32

#include <sys/sysctl.h>              // for sysctl, sysctlnametomib...


#define FDS_RESERVED     50          // Is there a better approach?
#define FDS_PER_CLIENT    2          // KQUEUE + IPv4
#define FDS_PER_DATABASE  7          // Writable~=7, Readable~=5

opts_t opts;


static const bool is_tty = isatty(STDERR_FILENO) != 0;


template<typename T, std::size_t N>
static ssize_t write(int fildes, T (&buf)[N]) {
	return write(fildes, buf, N - 1);
}

static ssize_t write(int fildes, std::string_view str) {
	return write(fildes, str.data(), str.size());
}


template <std::size_t N>
class signals_t {
	static constexpr int signals = N;

	std::array<std::string, signals> tty_messages;
	std::array<std::string, signals> messages;

public:
	signals_t() {
		for (std::size_t sig = 0; sig < signals; ++sig) {
#if defined(__linux__)
				const char* sig_str = strsignal(sig);
#elif defined(__APPLE__) || defined(__FreeBSD__)
				const char* sig_str = sys_signame[sig];
#endif
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
					tty_messages[sig] = string::format(LIGHT_RED + "Signal received: %s" + CLEAR_COLOR + "\n", sig_str);
					messages[sig] = string::format("Signal received: %s\n", sig_str);
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
					tty_messages[sig] = string::format(BROWN + "Signal received: %s" + CLEAR_COLOR + "\n", sig_str);
					messages[sig] = string::format("Signal received: %s\n", sig_str);
					break;
				case SIGSTOP:
				case SIGTSTP:
				case SIGTTIN:
				case SIGTTOU:
					// stop process
					tty_messages[sig] = string::format(SADDLE_BROWN + "Signal received: %s" + CLEAR_COLOR + "\n", sig_str);
					messages[sig] = string::format("Signal received: %s\n", sig_str);
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

					tty_messages[sig] = string::format(STEEL_BLUE + "Signal received: %s" + CLEAR_COLOR + "\n", sig_str);
					messages[sig] = string::format("Signal received: %s\n", sig_str);
					break;
				default:
					tty_messages[sig] = string::format(STEEL_BLUE + "Signal received: %s" + CLEAR_COLOR + "\n", sig_str);
					messages[sig] = string::format("Signal received: %s\n", sig_str);
					break;
			}
		}
	}

	void write(int fildes, int sig) {
		if (is_tty) {
			::write(fildes, tty_messages[(sig >= 0 && sig < signals) ? sig : signals - 1]);
		} else {
			::write(fildes, messages[(sig >= 0 && sig < signals) ? sig : signals - 1]);
		}
	}
};

static signals_t<arraySize(sys_siglist)> signals;


void toggle_hooks(int /*unused*/) {
	if (logger_info_hook != 0u) {
		logger_info_hook = 0;
		if (is_tty) {
			::write(STDERR_FILENO, STEEL_BLUE + "Info hooks disabled!" + CLEAR_COLOR + "\n");
		} else {
			::write(STDERR_FILENO, "Info hooks disabled!\n");
		}
	} else {
		logger_info_hook = fnv1ah32::hash("");
		if (is_tty) {
			::write(STDERR_FILENO, STEEL_BLUE + "Info hooks enabled!" + CLEAR_COLOR + "\n");
		} else {
			::write(STDERR_FILENO, "Info hooks enabled!\n");
		}
	}
}


void sig_handler(int sig) {
	int old_errno = errno; // save errno because write will clobber it

	signals.write(STDERR_FILENO, sig);

	if (sig == SIGTERM || sig == SIGINT) {
		close(STDIN_FILENO);
	}

// #if defined(__APPLE__) || defined(__FreeBSD__)
// 	if (sig == SIGINFO) {
// 		toggle_hooks(sig);
// 	}
// #endif

	if (XapiandManager::manager) {
		XapiandManager::manager->signal_sig(sig);
	}

	errno = old_errno;
}


void setup_signal_handlers() {
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
	auto ev_use = string::lower(name);
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
	if ((supported & ev::SELECT) != 0u) { backends.emplace_back(EV_SELECT_NAME); }
	if ((supported & ev::POLL) != 0u) { backends.emplace_back(EV_POLL_NAME); }
	if ((supported & ev::EPOLL) != 0u) { backends.emplace_back(EV_EPOLL_NAME); }
	if ((supported & ev::KQUEUE) != 0u) { backends.emplace_back(EV_KQUEUE_NAME); }
	if ((supported & ev::DEVPOLL) != 0u) { backends.emplace_back(EV_DEVPOLL_NAME); }
	if ((supported & ev::PORT) != 0u) { backends.emplace_back(EV_PORT_NAME); }
	if (backends.empty()) {
		backends.emplace_back("auto");
	}
	return backends;
}


void parseOptions(int argc, char** argv) {
	const unsigned int nthreads = std::thread::hardware_concurrency() * CONCURRENCY_MULTIPLIER;

	using namespace TCLAP;

	try {
		CmdLine cmd("", ' ', Package::STRING);

		CmdOutput output;
		ZshCompletionOutput zshoutput;

		if (std::getenv("ZSH_COMPLETE") != nullptr) {
			cmd.setOutput(&zshoutput);
		} else {
			cmd.setOutput(&output);
		}

		ValueArg<std::string> out("o", "out", "Output filename for dump.", false, "", "file", cmd);
		ValueArg<std::string> dump_metadata("", "dump-metadata", "Dump endpoint metadata to stdout.", false, "", "endpoint", cmd);
		ValueArg<std::string> dump_schema("", "dump-schema", "Dump endpoint schema to stdout.", false, "", "endpoint", cmd);
		ValueArg<std::string> dump_documents("", "dump", "Dump endpoint to stdout.", false, "", "endpoint", cmd);
		ValueArg<std::string> in("i", "in", "Input filename for restore.", false, "", "file", cmd);
		ValueArg<std::string> restore("", "restore", "Restore endpoint from stdin.", false, "", "endpoint", cmd);

		MultiSwitchArg verbose("v", "verbose", "Increase verbosity.", cmd);
		ValueArg<unsigned int> verbosity("", "verbosity", "Set verbosity.", false, 0, "verbosity", cmd);

#ifdef XAPIAN_HAS_GLASS_BACKEND
		SwitchArg chert("", "chert", "Prefer Chert databases.", cmd, false);
#endif

		std::vector<std::string> uuid_allowed({
			"simple",
			"compact",
			"partition",
#ifdef XAPIAND_UUID_GUID
			"guid",
#endif
#ifdef XAPIAND_UUID_URN
			"urn",
#endif
#ifdef XAPIAND_UUID_ENCODED
			"encoded",
#endif
		});
		ValuesConstraint<std::string> uuid_constraint(uuid_allowed);
		MultiArg<std::string> uuid("", "uuid", "Toggle modes for compact and/or encoded UUIDs and UUID index path partitioning.", false, &uuid_constraint, cmd);

		ValueArg<unsigned int> raft_port("", "raft-port", "Raft UDP port number to listen on.", false, XAPIAND_RAFT_SERVERPORT, "port", cmd);
		ValueArg<std::string> raft_group("", "raft-group", "Raft UDP group name.", false, XAPIAND_RAFT_GROUP, "group", cmd);

		ValueArg<unsigned int> discovery_port("", "discovery-port", "Discovery UDP port number to listen on.", false, XAPIAND_DISCOVERY_SERVERPORT, "port", cmd);
		ValueArg<std::string> discovery_group("", "discovery-group", "Discovery UDP group name.", false, XAPIAND_DISCOVERY_GROUP, "group", cmd);
		ValueArg<std::string> cluster_name("", "cluster", "Cluster name to join.", false, XAPIAND_CLUSTER_NAME, "cluster", cmd);
		ValueArg<std::string> node_name("", "name", "Node name.", false, "", "node", cmd);

		ValueArg<std::size_t> num_replicators("", "replicators", "Number of replicators.", false, NUM_REPLICATORS, "replicators", cmd);

		ValueArg<std::size_t> num_committers("", "committers", "Number of threads handling the commits.", false, NUM_COMMITTERS, "committers", cmd);
		ValueArg<std::size_t> max_databases("", "max-databases", "Max number of open databases.", false, MAX_DATABASES, "databases", cmd);
		ValueArg<std::size_t> dbpool_size("", "dbpool-size", "Maximum number of databases in database pool.", false, DBPOOL_SIZE, "size", cmd);

		ValueArg<std::size_t> num_fsynchers("", "fsynchers", "Number of threads handling the fsyncs.", false, NUM_FSYNCHERS, "fsynchers", cmd);
		ValueArg<std::size_t> max_files("", "max-files", "Max number of files to open.", false, 0, "files", cmd);
		ValueArg<std::size_t> flush_threshold("", "flush-threshold", "Xapian flush threshold.", false, FLUSH_THRESHOLD, "threshold", cmd);

		ValueArg<std::size_t> threadpool_size("", "threads", "Worker threads.", false, nthreads, "threads", cmd);
		ValueArg<std::size_t> tasks_size("", "tasks", "Number of async tasks.", false, TASKS_SIZE, "tasks", cmd);
		ValueArg<std::size_t> max_clients("", "max-clients", "Max number of open client connections.", false, MAX_CLIENTS, "clients", cmd);
		ValueArg<std::size_t> num_servers("", "servers", "Number of servers.", false, NUM_SERVERS, "servers", cmd);

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
		SwitchArg foreign("", "foreign", "Force foreign (shared) schemas for all indexes.", cmd, false);
		SwitchArg strict("", "strict", "Force the user to define the type for each field.", cmd, false);
		SwitchArg optimal("", "optimal", "Minimal optimal indexing configuration.", cmd, false);
		SwitchArg forceup("", "force-up", "Force to up xapiand on any directory.", cmd, false);
		ValueArg<std::string> database("D", "database", "Path to the root of the node.", false, ".", "path", cmd);

		std::vector<std::string> args;
		for (int i = 0; i < argc; ++i) {
			if (i == 0) {
				const char* a = std::strrchr(argv[i], '/');
				if (a != nullptr) {
					++a;
				} else {
					a = argv[i];
				}
				args.emplace_back(a);
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
				const char* a = std::strchr(arg, '=');
				if (a != nullptr) {
					if ((a - arg) != 0) {
						std::string tmp(arg, a - arg);
						args.push_back(tmp);
					}
					arg = a + 1;
				}
				if (*arg != 0) {
					args.emplace_back(arg);
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
		opts.strict = strict.getValue();
		opts.optimal = optimal.getValue();
		opts.foreign = foreign.getValue();
		opts.forceup = forceup.getValue();

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
		opts.flush_threshold = flush_threshold.getValue();
		opts.threadpool_size = threadpool_size.getValue();
		opts.tasks_size = tasks_size.getValue();
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
		if (opts.optimal) {
			opts.uuid_compact = true;
			opts.uuid_partition = true;
#if defined XAPIAND_UUID_ENCODED
			opts.uuid_repr = fnv1ah32::hash("encoded");
#else
			opts.uuid_repr = fnv1ah32::hash("simple");
#endif
		} else {
			opts.uuid_compact = false;
			opts.uuid_partition = false;
			opts.uuid_repr = fnv1ah32::hash("simple");
		}

		for (const auto& u : uuid.getValue()) {
			switch (fnv1ah32::hash(u)) {
				case fnv1ah32::hash("simple"):
					opts.uuid_repr = fnv1ah32::hash("simple");
					break;
				case fnv1ah32::hash("compact"):
					opts.uuid_compact = true;
					break;
				case fnv1ah32::hash("partition"):
					opts.uuid_partition = true;
					break;
#ifdef XAPIAND_UUID_GUID
				case fnv1ah32::hash("guid"):
					opts.uuid_repr = fnv1ah32::hash("guid");
					break;
#endif
#ifdef XAPIAND_UUID_URN
				case fnv1ah32::hash("urn"):
					opts.uuid_repr = fnv1ah32::hash("urn");
					break;
#endif
#ifdef XAPIAND_UUID_ENCODED
				case fnv1ah32::hash("encoded"):
					opts.uuid_repr = fnv1ah32::hash("encoded");
					break;
#endif
			}
		}

		opts.dump_metadata = dump_metadata.getValue();
		opts.dump_schema = dump_schema.getValue();
		opts.dump_documents = dump_documents.getValue();
		auto out_filename = out.getValue();
		opts.restore = restore.getValue();
		auto in_filename = in.getValue();
		if ((!opts.dump_metadata.empty() || !opts.dump_schema.empty() || !opts.dump_documents.empty()) && !opts.restore.empty()) {
			throw CmdLineParseException("Cannot dump and restore at the same time");
		} else if (!opts.dump_metadata.empty() || !opts.dump_schema.empty() || !opts.dump_documents.empty() || !opts.restore.empty()) {
			if (!opts.restore.empty()) {
				if (!out_filename.empty()) {
					throw CmdLineParseException("Option invalid: --out <file> can be used only with --dump");
				}
				opts.filename = in_filename;
			} else {
				if (!in_filename.empty()) {
					throw CmdLineParseException("Option invalid: --in <file> can be used only with --restore");
				}
				opts.filename = out_filename;
			}
			opts.detach = false;
		} else {
			if (!in_filename.empty()) {
				throw CmdLineParseException("Option invalid: --in <file> can be used only with --restore");
			}
			if (!out_filename.empty()) {
				throw CmdLineParseException("Option invalid: --out <file> can be used only with --dump");
			}
		}

	} catch (const ArgException& exc) { // catch any exceptions
		std::fprintf(stderr, "Error: %s for arg %s\n", exc.error().c_str(), exc.argId().c_str());
		std::exit(EX_USAGE);
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
	std::size_t mib_len = sizeof(mib) / sizeof(int);
#endif
#ifdef _SYSCTL_NAME
	auto max_files_per_proc_len = sizeof(max_files_per_proc);
	if (sysctl(mib, mib_len, &max_files_per_proc, &max_files_per_proc_len, nullptr, 0) < 0) {
		L_ERR("ERROR: Unable to get max files per process: sysctl(" _SYSCTL_NAME "): [%d] %s", errno, std::strerror(errno));
	}
#undef _SYSCTL_NAME
#else
	L_WARNING("WARNING: No way of getting max files per process.");
#endif

	return max_files_per_proc;
}


ssize_t get_open_files()
{
	int32_t max_files_per_proc = 0;

#if defined(KERN_OPENFILES)
#define _SYSCTL_NAME "kern.openfiles"  // FreeBSD
	int mib[] = {CTL_KERN, KERN_OPENFILES};
	std::size_t mib_len = sizeof(mib) / sizeof(int);
#elif defined(__APPLE__)
#define _SYSCTL_NAME "kern.num_files"  // Apple
	int mib[CTL_MAXNAME + 2];
	std::size_t mib_len = sizeof(mib) / sizeof(int);
	if (sysctlnametomib(_SYSCTL_NAME, mib, &mib_len) < 0) {
		L_ERR("ERROR: sysctl(" _SYSCTL_NAME "): [%d] %s", errno, std::strerror(errno));
		return 0;
	}
#endif
#ifdef _SYSCTL_NAME
	auto max_files_per_proc_len = sizeof(max_files_per_proc);
	if (sysctl(mib, mib_len, &max_files_per_proc, &max_files_per_proc_len, nullptr, 0) < 0) {
		L_ERR("ERROR: Unable to get number of open files: sysctl(" _SYSCTL_NAME "): [%d] %s", errno, std::strerror(errno));
	}
#undef _SYSCTL_NAME
#else
	L_WARNING("WARNING: No way of getting number of open files.");
#endif

	return max_files_per_proc;
}


void adjustOpenFilesLimit() {

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
	if (max_files != 0) {
		if (max_files > aprox_available_files) {
			L_WARNING("The requested open files limit of %zd %s the system-wide currently available number of files: %zd", max_files, max_files > available_files ? "exceeds" : "almost exceeds", available_files);
		}
	} else {
		max_files = recommended_files;
		if (max_files > aprox_available_files) {
			L_WARNING("The minimum recommended open files limit of %zd %s the system-wide currently available number of files: %zd", max_files, max_files > available_files ? "exceeds" : "almost exceeds", available_files);
		}
	}


	// Try getting current limit of files:
	ssize_t limit_cur_files;
	struct rlimit limit;
	if (getrlimit(RLIMIT_NOFILE, &limit) == -1) {
		limit_cur_files = available_files;
		if ((limit_cur_files == 0) || limit_cur_files > 4000) {
			limit_cur_files = 4000;
		}
		L_WARNING("Unable to obtain the current NOFILE limit (%s), assuming %zd", std::strerror(errno), limit_cur_files);
	} else {
		limit_cur_files = limit.rlim_cur;
	}


	// Set the the max number of files:
	// Increase if the current limit is not enough for our needs or
	// decrease if the user requests it.
	if ((opts.max_files != 0) || limit_cur_files < max_files) {
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
					L_INFO("Increased maximum number of open files to %zd (it was originally set to %zd)", new_max_files, (std::size_t)limit_cur_files);
				} else {
					L_INFO("Decresed maximum number of open files to %zd (it was originally set to %zd)", new_max_files, (std::size_t)limit_cur_files);
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

		if (setrlimit_error != 0) {
			L_ERR("Server can't set maximum open files to %zd because of OS error: %s", max_files, std::strerror(setrlimit_error));
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
		L_WARNING("You requested a dbpool_size of %zd requiring at least %zd max file descriptors", opts.dbpool_size, (opts.dbpool_size + 1) * FDS_PER_DATABASE  + FDS_RESERVED);
		L_WARNING("Current maximum open files is %zd so dbpool_size has been reduced to %zd to compensate for low limit.", max_files, new_dbpool_size);
	}
	if (new_max_clients > 0 && new_max_clients < opts.max_clients) {
		L_WARNING("You requested max_clients of %zd requiring at least %zd max file descriptors", opts.max_clients, (opts.max_clients + 1) * FDS_PER_CLIENT + FDS_RESERVED);
		L_WARNING("Current maximum open files is %zd so max_clients has been reduced to %zd to compensate for low limit.", max_files, new_max_clients);
	}


	// Warn about minimum/recommended sizes:
	if (max_files < minimum_files) {
		L_CRIT("Your open files limit of %zd is not enough for the server to start. Please increase your system-wide open files limit to at least %zd",
			max_files,
			minimum_files);
		L_WARNING("If you need to increase your system-wide open files limit use 'ulimit -n'");
		throw Exit(EX_OSFILE);
	} else if (max_files < recommended_files) {
		L_WARNING("Your current max_files of %zd is not enough. Please increase your system-wide open files limit to at least %zd",
			max_files,
			recommended_files);
		L_WARNING("If you need to increase your system-wide open files limit use 'ulimit -n'");
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
			L_CRIT("Can't run as root without the --uid switch");
			throw Exit(EX_USAGE);

		}

		struct passwd *pw;
		struct group *gr;

		if ((pw = getpwnam(username)) == nullptr) {
			uid = std::atoi(username);
			if ((uid == 0u) || (pw = getpwuid(uid)) == nullptr) {
				L_CRIT("Can't find the user %s to switch to", username);
				throw Exit(EX_NOUSER);
			}
		}
		uid = pw->pw_uid;
		gid_t gid = pw->pw_gid;
		username = pw->pw_name;

		if ((group != nullptr) && (*group != 0)) {
			if ((gr = getgrnam(group)) == nullptr) {
				gid = std::atoi(group);
				if ((gid == 0u) || (gr = getgrgid(gid)) == nullptr) {
					L_CRIT("Can't find the group %s to switch to", group);
					throw Exit(EX_NOUSER);
				}
			}
			gid = gr->gr_gid;
			group = gr->gr_name;
		} else {
			if ((gr = getgrgid(gid)) == nullptr) {
				L_CRIT("Can't find the group id %d", gid);
				throw Exit(EX_NOUSER);
			}
			group = gr->gr_name;
		}
		if (setgid(gid) < 0 || setuid(uid) < 0) {
			L_CRIT("Failed to assume identity of %s:%s", username, group);
			throw Exit(EX_OSERR);
		}
		L_NOTICE("Running as %s:%s", username, group);
	}
}


void detach() {
	pid_t pid = fork();
	if (pid != 0) {
		std::exit(EX_OK); /* parent exits */
	}
	setsid(); /* create a new session */

	/* Every output goes to /dev/null */
	int fd;
	if ((fd = io::open("/dev/null", O_RDWR, 0)) != -1) {
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		if (fd > STDERR_FILENO) { io::close(fd); }
	}
}


void writepid(const char* pidfile) {
	if (pidfile != nullptr && *pidfile != '\0') {
		/* Try to write the pid file in a best-effort way. */
		int fd = io::open(pidfile, O_RDWR | O_CREAT, 0644);
		if (fd > 0) {
			char buffer[100];
			std::snprintf(buffer, sizeof(buffer), "%lu\n", (unsigned long)getpid());
			io::write(fd, buffer, std::strlen(buffer));
			io::close(fd);
		}
	}
}


void usedir(const char* path, bool forceup) {

	if (!forceup) {
		DIR *dirp;
		dirp = opendir(path, true);
		if (!dirp) {
			L_CRIT("Cannot open working directory: %s", path);
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
				if (ent->d_namlen == 9 && std::strcmp(s, "flintlock") == 0)
#else
					if (std::strcmp(s, "flintlock") == 0)
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
			L_CRIT("Working directory must be empty or a valid xapian database: %s", path);
			throw Exit(EX_DATAERR);
		}
	}

	if (chdir(path) == -1) {
		if (build_path(std::string(path))) {
			if (chdir(path) == -1) {
				L_CRIT("Cannot change current working directory to %s", path);
				throw Exit(EX_OSFILE);
			}
		} else {
			L_ERR("Cannot create working directory: %s (%s)", path, std::strerror(errno));
		}
	}

	char buffer[PATH_MAX];
	getcwd(buffer, sizeof(buffer));
	Endpoint::cwd = normalize_path(buffer, buffer, true);  // Endpoint::cwd must always end with slash
	L_NOTICE("Changed current working directory to %s", Endpoint::cwd);
}


void banner() {
	set_thread_name("-=-");

	std::vector<std::string> values({
			string::format("Xapian v%d.%d.%d", Xapian::major_version(), Xapian::minor_version(), Xapian::revision()),
#if defined(XAPIAND_V8)
			string::format("V8 v%u.%u", V8_MAJOR_VERSION, V8_MINOR_VERSION),
#endif
#if defined(XAPIAND_CHAISCRIPT)
			string::format("ChaiScript v%d.%d", chaiscript::Build_Info::version_major(), chaiscript::Build_Info::version_minor()),
#endif
	});

	if (Logging::log_level >= LOG_NOTICE) {
		constexpr auto outer = rgb(0, 128, 0);
		constexpr auto inner = rgb(144, 238, 144);
		constexpr auto top = rgb(255, 255, 255);
		L(-LOG_INFO, NO_COLOR,
			"\n\n" +
			outer + "      _       "                                                       + rgb(255, 255, 255) + "      __\n" +
			outer + "  _-´´" +      top + "_" + outer  + "``-_   "                         + rgb(255, 255, 255) + " __  / /          _                 _\n" +
			outer + ".´ " +       top + "_-´ `-_" + outer + " `. "                         + rgb(224, 224, 224) + " \\ \\/ /__ _ _ __ (_) __ _ _ __   __| |\n" +
			outer + "| " +       top + "`-_   _-´" + outer + " | "                         + rgb(192, 192, 192) + "  \\  // _` | '_ \\| |/ _` | '_ \\ / _` |\n" +
			outer + "| " +     inner + "`-_" + top + "`-´" + inner + "_-´" + outer + " | " + rgb(160, 160, 160) + "  /  \\ (_| | |_) | | (_| | | | | (_| |\n" +
			outer + "| " +     inner + "`-_`-´_-´" + outer + " | "                         + rgb(128, 128, 128) + " / /\\_\\__,_| .__/|_|\\__,_|_| |_|\\__,_|\n" +
			outer + " `-_ " +     inner + "`-´" + outer + " _-´  "                         + rgb(96, 96, 96)    + "/_/" + rgb(144, 238, 144) + "%s" + rgb(96, 96, 96) + "|_|" + rgb(144, 238, 144) + "%s" + "\n" +
			outer + "    ``-´´   " + rgb(0, 128, 0) + "%s" + "\n" +
					"            " + rgb(0, 96, 0)  + "%s" + "\n\n",
			string::center(Package::HASH, 8, true),
			string::center(Package::FULLVERSION, 25, true),
			string::center("Using " + string::join(values, ", ", " and "), 42),
			string::center("[" + Package::BUGREPORT + "]", 42));
	} else {
		L(-LOG_INFO, NO_COLOR, "%s started.", Package::STRING);
	}
}

void setup(bool forceup) {
	// Flush threshold:
	const char *p = std::getenv("XAPIAN_FLUSH_THRESHOLD");
	if (p != nullptr) {
		L_INFO("Flush threshold is now %d. (from XAPIAN_FLUSH_THRESHOLD)", std::atoi(p) || 10000);
	} else {
		if (setenv("XAPIAN_FLUSH_THRESHOLD", string::Number(opts.flush_threshold).c_str(), 0) == -1) {
			L_INFO("Flush threshold is now 10000. (%s)", io::strerrno(errno));
		} else {
			L_INFO("Flush threshold is now %d. (it was originally 10000)", opts.flush_threshold);
		}
	}

	if (opts.chert) {
		L_INFO("Using Chert databases by default.");
	} else {
		L_INFO("Using Glass databases by default.");
	}

	std::vector<std::string> modes;
	if (opts.strict) {
		modes.emplace_back("strict");
	}
	if (opts.optimal) {
		modes.emplace_back("optimal");
	}
	if (!modes.empty()) {
		L_INFO("Activated " + string::join(modes, ", ", " and ") + ((modes.size() == 1) ? " mode by default." : " modes by default."));
	}

	adjustOpenFilesLimit();

	L_INFO("With a maximum of " + string::join(std::vector<std::string>{
		string::format("%zu %s", opts.max_files, opts.max_files == 1 ? "file" : "files"),
		string::format("%zu %s", opts.max_clients, opts.max_clients == 1 ? "client" : "clients"),
		string::format("%zu %s", opts.max_databases, opts.max_databases == 1 ? "database" : "databases"),
	}, ", ", " and ", [](const auto& s) { return s.empty(); }));

	usedir(opts.database.c_str(), forceup);
}


int server() {
	int exit_code = EX_OK;

	try {
		if (opts.detach) {
			L_NOTICE("Xapiand is done with all work here. Daemon on process ID [%d] taking over!", getpid());
		}

		usleep(100000ULL);

		setup(opts.forceup);

		ev::default_loop default_loop(opts.ev_flags);
		L_INFO("Connection processing backend: %s", ev_backend(default_loop.backend()));

		XapiandManager::manager = Worker::make_shared<XapiandManager>(&default_loop, opts.ev_flags);
		XapiandManager::manager->run();

		long managers = XapiandManager::manager.use_count() - 1;
		if (managers == 0) {
			L_NOTICE("Xapiand is cleanly done with all work!");
		} else {
			L_WARNING("Xapiand is uncleanly done with all work (%ld)!\n%s", managers, XapiandManager::manager->dump_tree());
		}
	} catch (const Exit& exc) {
		exit_code = exc.code;
	}

	return exit_code;
}


int dump_metadata() {
	int exit_code = EX_OK;

	int fd = opts.filename.empty() ? STDOUT_FILENO : io::open(opts.filename.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
	if (fd >= 0) {
		try {
			setup(opts.solo);
			XapiandManager::manager = Worker::make_shared<XapiandManager>();
			DatabaseHandler db_handler;
			Endpoints endpoints(Endpoint(opts.dump_metadata));
			L_NOTICE("Dumping metadata database: %s", repr(endpoints.to_string()));
			db_handler.reset(endpoints, DB_OPEN | DB_NOWAL);
			db_handler.dump_metadata(fd);
			L_NOTICE("Dump finished!");
		} catch (...) {
			if (fd != STDOUT_FILENO) {
				io::close(fd);
			}
			throw;
		}
		if (fd != STDOUT_FILENO) {
			io::close(fd);
		}
	} else {
		exit_code = EX_OSFILE;
		L_ERR("Cannot open file: %s", opts.filename);
	}

	return exit_code;
}


int dump_schema() {
	int exit_code = EX_OK;

	int fd = opts.filename.empty() ? STDOUT_FILENO : io::open(opts.filename.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
	if (fd >= 0) {
		try {
			setup(opts.solo);
			XapiandManager::manager = Worker::make_shared<XapiandManager>();
			DatabaseHandler db_handler;
			Endpoints endpoints(Endpoint(opts.dump_schema));
			L_NOTICE("Dumping schema database: %s", repr(endpoints.to_string()));
			db_handler.reset(endpoints, DB_OPEN | DB_NOWAL);
			db_handler.dump_schema(fd);
			L_NOTICE("Dump finished!");
		} catch (...) {
			if (fd != STDOUT_FILENO) {
				io::close(fd);
			}
			throw;
		}
		if (fd != STDOUT_FILENO) {
			io::close(fd);
		}
	} else {
		exit_code = EX_OSFILE;
		L_ERR("Cannot open file: %s", opts.filename);
	}

	return exit_code;
}


int dump_documents() {
	int exit_code = EX_OK;

	int fd = opts.filename.empty() ? STDOUT_FILENO : io::open(opts.filename.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
	if (fd >= 0) {
		try {
			setup(opts.solo);
			XapiandManager::manager = Worker::make_shared<XapiandManager>();
			DatabaseHandler db_handler;
			Endpoints endpoints(Endpoint(opts.dump_documents));
			L_NOTICE("Dumping database: %s", repr(endpoints.to_string()));
			db_handler.reset(endpoints, DB_OPEN | DB_NOWAL);
			db_handler.dump_documents(fd);
			L_NOTICE("Dump finished!");
		} catch (...) {
			if (fd != STDOUT_FILENO) {
				io::close(fd);
			}
			throw;
		}
		if (fd != STDOUT_FILENO) {
			io::close(fd);
		}
	} else {
		exit_code = EX_OSFILE;
		L_ERR("Cannot open file: %s", opts.filename);
	}

	return exit_code;
}


int restore() {
	int exit_code = EX_OK;

	int fd = (opts.filename.empty() || opts.filename == "-") ? STDIN_FILENO : io::open(opts.filename.c_str(), O_RDONLY);
	if (fd >= 0) {
		try {
			setup(opts.solo);
			XapiandManager::manager = Worker::make_shared<XapiandManager>();
			DatabaseHandler db_handler;
			Endpoints endpoints(Endpoint(opts.restore));
			L_NOTICE("Restoring into: %s", repr(endpoints.to_string()));
			db_handler.reset(endpoints, DB_WRITABLE | DB_SPAWN | DB_NOWAL);
			db_handler.restore(fd);
			L_NOTICE("Restore finished!");
		} catch (...) {
			if (fd != STDIN_FILENO) {
				io::close(fd);
			}
			throw;
		}
		if (fd != STDIN_FILENO) {
			io::close(fd);
		}
	} else {
		exit_code = EX_OSFILE;
		L_ERR("Cannot open file: %s", opts.filename);
	}

	return exit_code;
}


int main(int argc, char **argv) {
	int exit_code = EX_OK;

	parseOptions(argc, argv);

	if (opts.detach) {
		detach();
		writepid(opts.pidfile.c_str());
	}

	// Initialize options:
	setup_signal_handlers();
	std::setlocale(LC_CTYPE, "");
	demote(opts.uid.c_str(), opts.gid.c_str());

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

#ifdef XAPIAN_HAS_GLASS_BACKEND
	if (!opts.chert) {
		// Prefer glass database
		if (setenv("XAPIAN_PREFER_GLASS", "1", 0) != 0) {
			opts.chert = true;
		}
	}
#endif

	if (opts.strict) {
		default_spc.flags.strict = true;
	}

	if (opts.optimal) {
		default_spc.flags.optimal = true;
		default_spc.index = TypeIndex::FIELD_ALL;
		default_spc.flags.text_detection = false;
		default_spc.index_uuid_field = UUIDFieldIndex::UUID;
	}

	banner();

	try {
		if (!opts.dump_metadata.empty()) {
			exit_code = dump_metadata();
		} else if (!opts.dump_schema.empty()) {
			exit_code = dump_schema();
		} else if (!opts.dump_documents.empty()) {
			exit_code = dump_documents();
		} else if (!opts.restore.empty()) {
			exit_code = restore();
		} else {
			exit_code = server();
		}
	} catch (const BaseException& exc) {
		exit_code = EX_SOFTWARE;
		L_CRIT("Uncaught exception: %s", *exc.get_context() ? exc.get_context() : "Unkown BaseException!");
	} catch (const Xapian::Error& exc) {
		exit_code = EX_SOFTWARE;
		L_CRIT(exc.get_description());
	} catch (const std::exception& exc) {
		exit_code = EX_SOFTWARE;
		L_CRIT("Uncaught exception: %s", *exc.what() ? exc.what() : "Unkown std::exception!");
	} catch (...) {
		exit_code = EX_SOFTWARE;
		L_CRIT("Uncaught exception!");
	}

	XapiandManager::manager.reset();

	if (opts.detach && !opts.pidfile.empty()) {
		L_INFO("Removing the pid file.");
		unlink(opts.pidfile.c_str());
	}

	Logging::finish();
	return exit_code;
}
