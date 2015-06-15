/*
 * Copyright (C) 2015 deipi.com LLC and contributors. All rights reserved.
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

#include "utils.h"
#include "manager.h"

#include "tclap/CmdLine.h"
#include "tclap/ZshCompletionOutput.h"

#include <stdlib.h>

using namespace TCLAP;


XapiandManager *manager_ptr = NULL;


static void sig_shutdown_handler(int sig)
{
	if (manager_ptr) {
		manager_ptr->sig_shutdown_handler(sig);
	}
}


void setup_signal_handlers(void) {
	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	struct sigaction act;

	/* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction is used.
	 * Otherwise, sa_handler is used. */
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = sig_shutdown_handler;
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGINT, &act, NULL);
}


void run(int num_servers, const char *cluster_name_, const char *node_name_, const char *discovery_group, int discovery_port, int http_port, int binary_port)
{
	ev::default_loop default_loop;

	setup_signal_handlers();

	XapiandManager manager(&default_loop, cluster_name_, node_name_, discovery_group, discovery_port, http_port, binary_port);

	manager_ptr = &manager;

	manager.run(num_servers);

	manager_ptr = NULL;
}


// This exemplifies how the output class can be overridden to provide
// user defined output.
class CmdOutput : public StdOutput
{
	public:
		virtual void failure(CmdLineInterface& _cmd, ArgException& e) {
			std::string progName = _cmd.getProgramName();

			std::cerr << "Error: " << e.argId() << std::endl;

			spacePrint(std::cerr, e.error(), 75, 3, 0);

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

			throw ExitException(1);
		}

		virtual void usage(CmdLineInterface& _cmd) {
			std::string message = _cmd.getMessage();

			spacePrint(std::cout, message, 75, 0, 0);

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

typedef struct opts_s {
	int verbosity;
	bool daemonize;
	bool glass;
	std::string cluster_name;
	std::string node_name;
	int http_port;
	int binary_port;
	int discovery_port;
	std::string pidfile;
	std::string uid;
	std::string gid;
	std::string discovery_group;
	int num_servers;
} opts_t;

void parseOptions(int argc, char** argv, opts_t &opts)
{
	try {
		CmdLine cmd("Start " PACKAGE_NAME ".", ' ', PACKAGE_STRING);

		// ZshCompletionOutput zshoutput;
		// cmd.setOutput(&zshoutput);

		CmdOutput output;
		cmd.setOutput(&output);

		MultiSwitchArg verbosity("v", "verbose", "Increase verbosity.", cmd);
		SwitchArg daemonize("d", "daemon", "daemonize (run in background).", cmd);

#ifdef XAPIAN_HAS_GLASS_BACKEND
		SwitchArg glass("", "glass", "Try using glass databases.", cmd, false);
#endif

		ValueArg<std::string> cluster_name("", "cluster", "Cluster name to join.", false, XAPIAND_CLUSTER_NAME, "cluster", cmd);
		ValueArg<std::string> node_name("n", "name", "Node name.", false, "", "node", cmd);

		ValueArg<int> http_port("", "http", "HTTP REST API port", false, XAPIAND_HTTP_SERVERPORT, "port", cmd);
		ValueArg<int> binary_port("", "xapian", "Xapian binary protocol port", false, XAPIAND_BINARY_SERVERPORT, "port", cmd);
		ValueArg<int> discovery_port("", "discovery", "Discovery UDP port", false, XAPIAND_DISCOVERY_SERVERPORT, "port", cmd);
		ValueArg<std::string> discovery_group("", "group", "Discovery UDP group", false, XAPIAND_DISCOVERY_GROUP, "group", cmd);

		ValueArg<std::string> pidfile("p", "pid", "Write PID to <pidfile>.", false, "xapiand.pid", "pidfile", cmd);
		ValueArg<std::string> uid("u", "uid", "User ID.", false, "xapiand", "uid", cmd);
		ValueArg<std::string> gid("g", "gid", "Group ID.", false, "xapiand", "uid", cmd);

		ValueArg<int> num_servers("", "workers", "Number of worker servers.", false, 8, "servers", cmd);

		cmd.parse( argc, argv );

		opts.verbosity = verbosity.getValue();
		opts.daemonize = daemonize.getValue();
#ifdef XAPIAN_HAS_GLASS_BACKEND
		opts.glass = glass.getValue();
#endif
		opts.cluster_name = cluster_name.getValue();
		opts.node_name = node_name.getValue();
		opts.http_port = http_port.getValue();
		opts.binary_port = binary_port.getValue();
		opts.discovery_port = discovery_port.getValue();
		opts.discovery_group = discovery_group.getValue();
		opts.pidfile = pidfile.getValue();
		opts.uid = uid.getValue();
		opts.gid = gid.getValue();
		opts.num_servers = num_servers.getValue();

	} catch (ArgException &e) { // catch any exceptions
		std::cerr << "error: " << e.error() << " for arg " << e.argId() << std::endl;
	}
}


int main(int argc, char **argv)
{
	opts_t opts;

	parseOptions(argc, argv, opts);

	int http_port = (opts.http_port == XAPIAND_HTTP_SERVERPORT) ? 0 : opts.http_port;
	int binary_port = (opts.binary_port == XAPIAND_BINARY_SERVERPORT) ? 0 : opts.binary_port;
	int discovery_port = (opts.discovery_port == XAPIAND_DISCOVERY_SERVERPORT) ? 0 : opts.discovery_port;
	const char *discovery_group = (opts.discovery_group.empty() || opts.discovery_group == XAPIAND_DISCOVERY_GROUP) ? NULL : opts.discovery_group.c_str();
	const char *cluster_name = opts.cluster_name.c_str();;
	const char *node_name = opts.node_name.c_str();;

	INFO((void *)NULL,
		"\n\n"
		"  __  __           _                 _\n"
		"  \\ \\/ /__ _ _ __ (_) __ _ _ __   __| |\n"
		"   \\  // _` | '_ \\| |/ _` | '_ \\ / _` |\n"
		"   /  \\ (_| | |_) | | (_| | | | | (_| |\n"
		"  /_/\\_\\__,_| .__/|_|\\__,_|_| |_|\\__,_|\n"
		"            |_|  v%s\n"
		"   [%s]\n"
		"          Using Xapian v%s\n\n", PACKAGE_VERSION, PACKAGE_BUGREPORT, XAPIAN_VERSION);

	INFO((void *)NULL, "Joined cluster: %s\n", cluster_name);

#ifdef XAPIAN_HAS_GLASS_BACKEND
	if (opts.glass) {
		// Prefer glass database
		if (setenv("XAPIAN_PREFER_GLASS", "1", false) == 0) {
			INFO((void *)NULL, "Enabled glass database.\n");
		}
	}
#endif

	// Enable changesets
	if (setenv("XAPIAN_MAX_CHANGESETS", "10", false) == 0) {
		INFO((void *)NULL, "Database changesets set to 10.\n");
	}

	// Flush threshold increased
	int flush_threshold = 10000;  // Default is 10000 (if no set)
	const char *p = getenv("XAPIAN_FLUSH_THRESHOLD");
	if (p) flush_threshold = atoi(p);
	if (flush_threshold < 100000 && setenv("XAPIAN_FLUSH_THRESHOLD", "100000", false) == 0) {
		INFO((void *)NULL, "Increased flush threshold to 100000 (it was originally set to %d).\n", flush_threshold);
	}

	time(&init_time);
	struct tm *timeinfo = localtime(&init_time);
	timeinfo->tm_hour   = 0;
	timeinfo->tm_min    = 0;
	timeinfo->tm_sec    = 0;
	int diff_t = (int)(init_time - mktime(timeinfo));
	b_time.minute = diff_t / SLOT_TIME_SECOND;
	b_time.second =  diff_t % SLOT_TIME_SECOND;

	run(opts.num_servers, cluster_name, node_name, discovery_group, discovery_port, http_port, binary_port);

	INFO((void *)NULL, "Done with all work!\n");
	return 0;
}
