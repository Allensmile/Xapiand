/*
 * Copyright deipi.com LLC and contributors. All rights reserved.
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

#include "utils.h"
#include "config.h"
#include "server.h"

#include "xapiand.h"

#include <stdlib.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h> /* for TCP_NODELAY */
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unordered_set>


time_t xapiand::shutdown = (time_t)0;
time_t xapiand::shutdown_asap = (time_t)0;
ev::async xapiand::async_shutdown;


static ev::default_loop default_loop;
static std::unordered_set<XapiandServer *> servers;


void check_tcp_backlog(int tcp_backlog) {
#if defined(NET_CORE_SOMAXCONN)
	int name[3] = {CTL_NET, NET_CORE, NET_CORE_SOMAXCONN};
#elif defined(KIPC_SOMAXCONN)
	int name[3] = {CTL_KERN, KERN_IPC, KIPC_SOMAXCONN};
#else
	return;
#endif
	int somaxconn;
	size_t somaxconn_len = sizeof(somaxconn);
	if (sysctl(name, 3, &somaxconn, &somaxconn_len, 0, 0) < 0) {
		LOG_CONN((void *)NULL, "ERROR: sysctl: %s\n", strerror(errno));
		return;
	}
	if (somaxconn > 0 && somaxconn < tcp_backlog) {
		LOG_ERR((void *)NULL, "WARNING: The TCP backlog setting of %d cannot be enforced because "
#if defined(NET_CORE_SOMAXCONN)
		"net.core.somaxconn"
#elif defined(KIPC_SOMAXCONN)
		"kern.ipc.somaxconn"
#endif
		" is set to the lower value of %d.\n", tcp_backlog, somaxconn);
	}
}


int bind_http(int http_port)
{
	int tcp_backlog = XAPIAND_TCP_BACKLOG;
	int error;
	int optval = 1;
	struct sockaddr_in addr;
	struct linger ling = {0, 0};
	
	int http_sock = socket(PF_INET, SOCK_STREAM, 0);

	setsockopt(http_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&optval, sizeof(optval));
	error = setsockopt(http_sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&optval, sizeof(optval));
	if (error != 0)
		LOG_CONN((void *)NULL, "ERROR: setsockopt (sock=%d): %s\n", http_sock, strerror(errno));
	
	error = setsockopt(http_sock, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
	if (error != 0)
		LOG_CONN((void *)NULL, "ERROR: setsockopt (sock=%d): %s\n", http_sock, strerror(errno));

	error = setsockopt(http_sock, IPPROTO_TCP, TCP_NODELAY, (void *)&optval, sizeof(optval));
	if (error != 0)
		LOG_CONN((void *)NULL, "ERROR: setsockopt (sock=%d): %s\n", http_sock, strerror(errno));
	
	addr.sin_family = AF_INET;
	addr.sin_port = htons(http_port);
	addr.sin_addr.s_addr = INADDR_ANY;
	
	if (bind(http_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		LOG_ERR((void *)NULL, "ERROR: http bind error (sock=%d): %s\n", http_sock, strerror(errno));
		close(http_sock);
		http_sock = -1;
	} else {
		fcntl(http_sock, F_SETFL, fcntl(http_sock, F_GETFL, 0) | O_NONBLOCK);
		
		check_tcp_backlog(tcp_backlog);
		listen(http_sock, tcp_backlog);
	}

	return http_sock;
}


int bind_binary(int binary_port)
{
	int tcp_backlog = XAPIAND_TCP_BACKLOG;
	int error;
	int optval = 1;
	struct sockaddr_in addr;
	struct linger ling = {0, 0};

	int binary_sock = socket(PF_INET, SOCK_STREAM, 0);

	setsockopt(binary_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&optval, sizeof(optval));
	error = setsockopt(binary_sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&optval, sizeof(optval));
	if (error != 0)
		LOG_CONN((void *)NULL, "ERROR: setsockopt (sock=%d): %s\n", binary_sock, strerror(errno));
	
	error = setsockopt(binary_sock, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
	if (error != 0)
		LOG_CONN((void *)NULL, "ERROR: setsockopt (sock=%d): %s\n", binary_sock, strerror(errno));
	
	error = setsockopt(binary_sock, IPPROTO_TCP, TCP_NODELAY, (void *)&optval, sizeof(optval));
	if (error != 0)
		LOG_CONN((void *)NULL, "ERROR: setsockopt (sock=%d): %s\n", binary_sock, strerror(errno));
	
	addr.sin_family = AF_INET;
	addr.sin_port = htons(binary_port);
	addr.sin_addr.s_addr = INADDR_ANY;
	
	if (bind(binary_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		LOG_ERR((void *)NULL, "ERROR: binary bind error (sock=%d): %s\n", binary_sock, strerror(errno));
		close(binary_sock);
		binary_sock = -1;
	} else {
		fcntl(binary_sock, F_SETFL, fcntl(binary_sock, F_GETFL, 0) | O_NONBLOCK);
		
		check_tcp_backlog(tcp_backlog);
		listen(binary_sock, tcp_backlog);
	}

	return binary_sock;
}


static void shutdown(int signum)
{
	if (xapiand::shutdown_asap) {
		LOG_OBJ((void *)NULL, "Breaking default loop!\n");
		default_loop.break_loop();
	}
	std::unordered_set<XapiandServer *>::const_iterator it(servers.begin());
	for (; it != servers.end(); it++) {
		(*it)->shutdown(signum);
	}
}


void sig_shutdown_handler(int sig) {
	/* SIGINT is often delivered via Ctrl+C in an interactive session.
	 * If we receive the signal the second time, we interpret this as
	 * the user really wanting to quit ASAP without waiting to persist
	 * on disk. */
	time_t now = time(NULL);
	if (xapiand::shutdown_asap && sig != SIGTERM) {
		if (xapiand::shutdown_asap + 1 < now) {
			LOG((void *)NULL, "You insist... exiting now.\n");
			// remove pid file here, use: getpid();
			exit(1); /* Exit with an error since this was not a clean shutdown. */
		}
	} else if (xapiand::shutdown && sig != SIGTERM) {
		if (xapiand::shutdown + 1 < now) {
			xapiand::shutdown_asap = now;
			LOG((void *)NULL, "Trying immediate shutdown.\n");
		}
	} else {
		xapiand::shutdown = now;
		switch (sig) {
			case SIGINT:
				LOG((void *)NULL, "Received SIGINT scheduling shutdown...\n");
				break;
			case SIGTERM:
				LOG((void *)NULL, "Received SIGTERM scheduling shutdown...\n");
				break;
			default:
				LOG((void *)NULL, "Received shutdown signal, scheduling shutdown...\n");
		};
	}
	shutdown(sig);
}


static void shutdown_cb(ev::async &watcher, int revents)
{
	sig_shutdown_handler(0);
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


int main(int argc, char **argv)
{

	int http_port = XAPIAND_HTTP_SERVERPORT;
	int binary_port = XAPIAND_BINARY_SERVERPORT;

	if (argc > 2) {
		http_port = atoi(argv[1]);
		binary_port = atoi(argv[2]);
	}

	LOG((void *)NULL, "Starting %s (%s).\n", PACKAGE_STRING, PACKAGE_BUGREPORT);

	int http_sock = bind_http(http_port);
	int binary_sock = bind_binary(binary_port);

	int tasks = 8;

	if (http_sock != -1 && binary_sock != -1) {
		setup_signal_handlers();

		LOG((void *)NULL, "Listening on %d (http), %d (xapian)...\n", http_port, binary_port);

		DatabasePool database_pool;
		ThreadPool thread_pool(10);

		xapiand::async_shutdown.set<shutdown_cb>();
		xapiand::async_shutdown.start();

		if (tasks) {
			ThreadPool server_pool(tasks);

			for (int i = 0; i < tasks; i++) {
				XapiandServer *server = new XapiandServer(NULL, http_sock, binary_sock, &database_pool, &thread_pool);
				servers.insert(server);
				server_pool.addTask(server, (void *)0);
			}
			
			default_loop.run();
			
			LOG_OBJ((void *)NULL, "Waiting for threads...\n");
			
			server_pool.finish();
			server_pool.join();
		} else {
			XapiandServer * server = new XapiandServer(&default_loop, http_sock, binary_sock, &database_pool, &thread_pool);
			servers.insert(server);
			server->run(NULL);
			delete server;
		}
	}

	if (http_sock != -1) {
		close(http_sock);
	}

	if (binary_sock != -1) {
		close(binary_sock);
	}
	
	LOG((void *)NULL, "Done with all work!\n");

	return 0;
}
