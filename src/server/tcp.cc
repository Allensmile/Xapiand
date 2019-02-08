/*
 * Copyright (c) 2015-2019 Dubalu LLC
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

#include "config.h"                 // for HAVE_SYS_SYSCTL_H, XAPIAND_TCP_BACKLOG

#include "tcp.h"

#include <arpa/inet.h>              // for htonl, htons
#include <cstring>                  // for memset
#include <errno.h>                  // for errno
#include <fcntl.h>                  // for fcntl, F_GETFL, F_SETFL, O_NONBLOCK
#include <netdb.h>                  // for addrinfo, freeaddrinfo, getaddrinfo
#include <netinet/in.h>             // for sockaddr_in, INADDR_ANY, IPPROTO_TCP
#include <netinet/tcp.h>            // for TCP_NODELAY
#include <sys/types.h>              // for SOL_SOCKET, SO_NOSIGPIPE
#include <sys/socket.h>             // for setsockopt, bind, connect
#include <netdb.h>                  // for getaddrinfo, addrinfo
#include <utility>
#ifdef HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>             // for sysctl, CTL_KERN, KIPC_SOMAXCONN
#endif
#include <sysexits.h>               // for EX_CONFIG, EX_IOERR

#include "error.hh"                 // for error:name, error::description
#include "io.hh"                    // for close, ignored_errno
#include "log.h"                    // for L_ERR, L_OBJ, L_CRIT, L_CONN
#include "manager.h"                // for sig_exit


// #undef L_DEBUG
// #define L_DEBUG L_GREY
// #undef L_CALL
// #define L_CALL L_STACKED_DIM_GREY
// #undef L_CONN
// #define L_CONN L_LIGHT_GREEN


TCP::TCP(const char* description, int flags)
	: sock(-1),
	  closed(true),
	  flags(flags),
	  description(description),
	  addr{}
{}


TCP::~TCP() noexcept
{
	try {
		if (sock != -1) {
			if (io::close(sock) == -1) {
				L_WARNING("WARNING: close {{sock:{}}} - {} ({}): {}", sock, error::name(errno), errno, error::description(errno));
			}
		}
	} catch (...) {
		L_EXC("Unhandled exception in destructor");
	}
}


bool
TCP::close(bool close) {
	L_CALL("TCP::close({})", close ? "true" : "false");

	bool was_closed = closed.exchange(true);
	if (!was_closed && sock != -1) {
		if (close) {
			// Dangerously close socket!
			// (make sure no threads are using the file descriptor)
			if (io::close(sock) == -1) {
				L_WARNING("WARNING: close {{sock:{}}} - {} ({}): {}", sock, error::name(errno), errno, error::description(errno));
			}
			sock = -1;
		} else {
			io::shutdown(sock, SHUT_RDWR);
		}
	}
	return was_closed;
}


void
TCP::bind(const char* hostname, unsigned int serv, int tries)
{
	L_CALL("TCP::bind({})", tries);

	if (!closed.exchange(false) || !tries) {
		return;
	}

	int optval = 1;

	L_CONN("Binding TCP {}:{}", hostname ? hostname : "0.0.0.0", serv);

	for (; --tries >= 0; ++serv) {
		char servname[6];  // strlen("65535") + 1
		snprintf(servname, sizeof(servname), "%d", serv);

		struct addrinfo hints = {};
		hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV;  // No effect if hostname != nullptr
		hints.ai_family = PF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		struct addrinfo *addrinfo;
		if (int err = getaddrinfo(hostname, servname, &hints, &addrinfo)) {
			L_CRIT("ERROR: getaddrinfo {}:{} {{sock:{}}}: {}", hostname ? hostname : "0.0.0.0", servname, sock, gai_strerror(err));
			sig_exit(-EX_CONFIG);
			return;
		}

		for (auto ai = addrinfo; ai != nullptr; ai = ai->ai_next) {
			if ((sock = io::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1) {
				if (ai->ai_next == nullptr) {
					freeaddrinfo(addrinfo);
					L_CRIT("ERROR: {} socket: {} ({}): {}", description, error::name(errno), errno, error::description(errno));
					sig_exit(-EX_IOERR);
					return;
				}
				L_CONN("ERROR: {} socket: {} ({}): {}", description, error::name(errno), errno, error::description(errno));
				continue;
			}

			if (io::fcntl(sock, F_SETFL, io::fcntl(sock, F_GETFL, 0) | O_NONBLOCK) == -1) {
				freeaddrinfo(addrinfo);
				if (!tries) {
					L_CRIT("ERROR: {} fcntl O_NONBLOCK {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
					close();
					sig_exit(-EX_CONFIG);
					return;
				}
				L_CONN("ERROR: {} fcntl O_NONBLOCK {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
				break;
			}

			if (io::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
				freeaddrinfo(addrinfo);
				if (!tries) {
					L_CRIT("ERROR: {} setsockopt SO_REUSEADDR {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
					close();
					sig_exit(-EX_CONFIG);
					return;
				}
				L_CONN("ERROR: {} setsockopt SO_REUSEADDR {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
				close();
				break;
			}

			if ((flags & TCP_SO_REUSEPORT) != 0) {
#ifdef SO_REUSEPORT_LB
				if (io::setsockopt(sock, SOL_SOCKET, SO_REUSEPORT_LB, &optval, sizeof(optval)) == -1) {
					freeaddrinfo(addrinfo);
					if (!tries) {
						L_CRIT("ERROR: {} setsockopt SO_REUSEPORT_LB {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
						close();
						sig_exit(-EX_CONFIG);
						return;
					}
					L_CONN("ERROR: {} setsockopt SO_REUSEPORT_LB {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
					close();
					break;
				}
#else
				if (io::setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) == -1) {
					freeaddrinfo(addrinfo);
					if (!tries) {
						L_CRIT("ERROR: {} setsockopt SO_REUSEPORT {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
						close();
						sig_exit(-EX_CONFIG);
						return;
					}
					L_CONN("ERROR: {} setsockopt SO_REUSEPORT {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
					close();
					break;
				}
#endif
			}

#ifdef SO_NOSIGPIPE
			if (io::setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &optval, sizeof(optval)) == -1) {
				freeaddrinfo(addrinfo);
				if (!tries) {
					L_CRIT("ERROR: {} setsockopt SO_NOSIGPIPE {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
					close();
					sig_exit(-EX_CONFIG);
					return;
				}
				L_CONN("ERROR: {} setsockopt SO_NOSIGPIPE {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
				close();
				break;
			}
#endif

			if (io::setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) == -1) {
				freeaddrinfo(addrinfo);
				if (!tries) {
					L_CRIT("ERROR: {} setsockopt SO_KEEPALIVE {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
					close();
					sig_exit(-EX_CONFIG);
					return;
				}
				L_CONN("ERROR: {} setsockopt SO_KEEPALIVE {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
				close();
				break;
			}

			struct linger linger;
			linger.l_onoff = 1;
			linger.l_linger = 0;
			if (io::setsockopt(sock, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger)) == -1) {
				freeaddrinfo(addrinfo);
				if (!tries) {
					L_CRIT("ERROR: {} setsockopt SO_LINGER {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
					close();
					sig_exit(-EX_CONFIG);
					return;
				}
				L_CONN("ERROR: {} setsockopt SO_LINGER {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
				close();
				break;
			}

			if ((flags & TCP_TCP_DEFER_ACCEPT) != 0) {
				// Activate TCP_DEFER_ACCEPT (dataready's SO_ACCEPTFILTER) for HTTP connections only.
				// We want the HTTP server to wakeup accepting connections that already have some data
				// to read; this is not the case for binary servers where the server is the one first
				// sending data.

#ifdef SO_ACCEPTFILTER
				struct accept_filter_arg af = {"dataready", ""};

				if (io::setsockopt(sock, SOL_SOCKET, SO_ACCEPTFILTER, &af, sizeof(af)) == -1) {
					freeaddrinfo(addrinfo);
					if (!tries) {
						L_CRIT("ERROR: Failed to enable the 'dataready' Accept Filter: setsockopt SO_ACCEPTFILTER {{sock:{}}}: {} ({}): {}", sock, error::name(errno), errno, error::description(errno));
						close();
						sig_exit(-EX_CONFIG);
						return;
					}
					L_CONN("ERROR: Failed to enable the 'dataready' Accept Filter: setsockopt SO_ACCEPTFILTER {{sock:{}}}: {} ({}): {}", sock, error::name(errno), errno, error::description(errno));
					close();
					break;
				}
#endif

#ifdef TCP_DEFER_ACCEPT
				if (io::setsockopt(sock, IPPROTO_TCP, TCP_DEFER_ACCEPT, &optval, sizeof(optval)) == -1) {
					freeaddrinfo(addrinfo);
					if (!tries) {
						L_CRIT("ERROR: setsockopt TCP_DEFER_ACCEPT {{sock:{}}}: {} ({}): {}", sock, error::name(errno), errno, error::description(errno));
						close();
						sig_exit(-EX_CONFIG);
						return;
					}
					L_CONN("ERROR: setsockopt TCP_DEFER_ACCEPT {{sock:{}}}: {} ({}): {}", sock, error::name(errno), errno, error::description(errno));
					close();
					break;
				}
#endif
			}

			addr = *reinterpret_cast<struct sockaddr_in*>(ai->ai_addr);

			if (io::bind(sock, ai->ai_addr, ai->ai_addrlen) == -1) {
				freeaddrinfo(addrinfo);
				if (!tries) {
					L_CRIT("ERROR: {} bind error {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
					close();
					sig_exit(-EX_CONFIG);
					return;
				}
				L_CONN("ERROR: {} bind error {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
				close();
				break;
			}

			if (io::listen(sock, checked_tcp_backlog(XAPIAND_TCP_BACKLOG)) == -1) {
				freeaddrinfo(addrinfo);
				if (!tries) {
					L_CRIT("ERROR: {} listen error {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
					close();
					sig_exit(-EX_CONFIG);
					return;
				}
				L_CONN("ERROR: {} listen error {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
				close();
				break;
			}

			// L_RED("TCP addr -> {}:{}", inet_ntop(addr), ntohs(addr.sin_port));

			freeaddrinfo(addrinfo);
			return;
		}
	}

	L_CRIT("ERROR: {} unknown bind error {{sock:{}}}: {} ({}): {}", description, sock, error::name(errno), errno, error::description(errno));
	close();
	sig_exit(-EX_CONFIG);
}


int
TCP::accept()
{
	L_CALL("TCP::accept() {{sock={}}}", sock);

	int client_sock;

	int optval = 1;

	struct sockaddr_in client_addr;
	socklen_t addrlen = sizeof(client_addr);

	if ((client_sock = io::accept(sock, (struct sockaddr *)&client_addr, &addrlen)) == -1) {
		if (!io::ignored_errno(errno, true, true, true)) {
			L_ERR("ERROR: accept error {{sock:{}}}: {} ({}): {}", sock, error::name(errno), errno, error::description(errno));
		}
		return -1;
	}

	if (io::fcntl(client_sock, F_SETFL, fcntl(client_sock, F_GETFL, 0) | O_NONBLOCK) == -1) {
		L_ERR("ERROR: fcntl O_NONBLOCK {{client_sock:{}}}: {} ({}): {}", client_sock, error::name(errno), errno, error::description(errno));
		io::close(client_sock);
		return -1;
	}

#ifdef SO_NOSIGPIPE
	if (io::setsockopt(client_sock, SOL_SOCKET, SO_NOSIGPIPE, &optval, sizeof(optval)) == -1) {
		L_ERR("ERROR: setsockopt SO_NOSIGPIPE {{client_sock:{}}}: {} ({}): {}", client_sock, error::name(errno), errno, error::description(errno));
		io::close(client_sock);
		return -1;
	}
#endif

	if (io::setsockopt(client_sock, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) == -1) {
		L_ERR("ERROR: setsockopt SO_KEEPALIVE {{client_sock:{}}}: {} ({}): {}", client_sock, error::name(errno), errno, error::description(errno));
		io::close(client_sock);
		return -1;
	}

	struct linger linger;
	linger.l_onoff = 1;
	linger.l_linger = 0;
	if (io::setsockopt(client_sock, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger)) == -1) {
		L_ERR("ERROR: setsockopt SO_LINGER {{client_sock:{}}}: {} ({}): {}", client_sock, error::name(errno), errno, error::description(errno));
		io::close(client_sock);
		return -1;
	}

	if ((flags & TCP_TCP_NODELAY) != 0) {
		if (io::setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) == -1) {
			L_ERR("ERROR: setsockopt TCP_NODELAY {{client_sock:{}}}: {} ({}): {}", client_sock, error::name(errno), errno, error::description(errno));
			io::close(client_sock);
			return -1;
		}
	}

	return client_sock;
}


int
TCP::checked_tcp_backlog(int tcp_backlog)
{
#ifdef HAVE_SYS_SYSCTL_H
#if defined(KIPC_SOMAXCONN)
#define _SYSCTL_NAME "kern.ipc.somaxconn"  // FreeBSD, Apple
	int mib[] = {CTL_KERN, KERN_IPC, KIPC_SOMAXCONN};
	size_t mib_len = sizeof(mib) / sizeof(int);
#endif
#endif
#ifdef _SYSCTL_NAME
	int somaxconn;
	size_t somaxconn_len = sizeof(somaxconn);
	if (sysctl(mib, mib_len, &somaxconn, &somaxconn_len, nullptr, 0) < 0) {
		L_ERR("ERROR: sysctl(" _SYSCTL_NAME "): {} ({}): {}", error::name(errno), errno, error::description(errno));
		return tcp_backlog;
	}
	if (somaxconn > 0 && somaxconn < tcp_backlog) {
		L_WARNING_ONCE("WARNING: The TCP backlog setting of {} cannot be enforced because "
				_SYSCTL_NAME
				" is set to the lower value of {}.", tcp_backlog, somaxconn);
	}
#undef _SYSCTL_NAME
#elif defined(__linux__)
	int fd = io::open("/proc/sys/net/core/somaxconn", O_RDONLY);
	if unlikely(fd == -1) {
		L_ERR("ERROR: Unable to open /proc/sys/net/core/somaxconn: {} ({}): {}", error::name(errno), errno, error::description(errno));
		return tcp_backlog;
	}
	char line[100];
	ssize_t n = io::read(fd, line, sizeof(line));
	if unlikely(n == -1) {
		L_ERR("ERROR: Unable to read from /proc/sys/net/core/somaxconn: {} ({}): {}", error::name(errno), errno, error::description(errno));
		return tcp_backlog;
	}
	int somaxconn = atoi(line);
	if (somaxconn > 0 && somaxconn < tcp_backlog) {
		L_WARNING_ONCE("WARNING: The TCP backlog setting of {} cannot be enforced because "
				"/proc/sys/net/core/somaxconn"
				" is set to the lower value of {}.", tcp_backlog, somaxconn);
	}
#else
	L_WARNING_ONCE("WARNING: No way of getting TCP backlog setting of {}.", tcp_backlog);
#endif
	return tcp_backlog;
}


int
TCP::connect(const char* hostname, const char* servname)
{
	L_CALL("TCP::connect({}, {})", hostname, servname);

	struct addrinfo hints = {};
	hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV;
	hints.ai_family = PF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	struct addrinfo *addrinfo;
	if (int err = getaddrinfo(hostname, servname, &hints, &addrinfo)) {
		L_ERR("Couldn't resolve host {}:{}: {}", hostname, servname, gai_strerror(err));
		return -1;
	}

	for (auto ai = addrinfo; ai != nullptr; ai = ai->ai_next) {
		int conn_sock;
		int optval = 1;

		if ((conn_sock = io::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1) {
			if (ai->ai_next == nullptr) {
				L_CRIT("ERROR: {}:{} socket: {} ({}): {}", hostname, servname, error::name(errno), errno, error::description(errno));
				return -1;
			}
			L_CONN("ERROR: {}:{} socket: {} ({}): {}", hostname, servname, error::name(errno), errno, error::description(errno));
			continue;
		}

		if (io::fcntl(conn_sock, F_SETFL, io::fcntl(conn_sock, F_GETFL, 0) | O_NONBLOCK) == -1) {
			L_ERR("ERROR: fcntl O_NONBLOCK {{conn_sock:{}}}: {} ({}): {}", conn_sock, error::name(errno), errno, error::description(errno));
			io::close(conn_sock);
			return -1;
		}

#ifdef SO_NOSIGPIPE
		if (io::setsockopt(conn_sock, SOL_SOCKET, SO_NOSIGPIPE, &optval, sizeof(optval)) == -1) {
			L_ERR("ERROR: setsockopt SO_NOSIGPIPE {{conn_sock:{}}}: {} ({}): {}", conn_sock, error::name(errno), errno, error::description(errno));
			io::close(conn_sock);
			return -1;
		}
#endif

		if (io::setsockopt(conn_sock, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) == -1) {
			L_ERR("ERROR: setsockopt SO_KEEPALIVE {{conn_sock:{}}}: {} ({}): {}", conn_sock, error::name(errno), errno, error::description(errno));
			io::close(conn_sock);
			return -1;
		}

		struct linger linger;
		linger.l_onoff = 1;
		linger.l_linger = 0;
		if (io::setsockopt(conn_sock, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger)) == -1) {
			L_ERR("ERROR: setsockopt SO_LINGER {{conn_sock:{}}}: {} ({}): {}", conn_sock, error::name(errno), errno, error::description(errno));
			io::close(conn_sock);
			return -1;
		}

		if (io::setsockopt(conn_sock, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) == -1) {
			L_ERR("ERROR: setsockopt TCP_NODELAY {{conn_sock:{}}}: {} ({}): {}", conn_sock, error::name(errno), errno, error::description(errno));
			io::close(conn_sock);
			return -1;
		}

		if (io::connect(conn_sock, ai->ai_addr, ai->ai_addrlen) == -1 && errno != EINPROGRESS && errno != EALREADY) {
			freeaddrinfo(addrinfo);
			io::close(conn_sock);
			return -1;
		}

		freeaddrinfo(addrinfo);
		return conn_sock;
	}

	L_ERR("ERROR: connect error to {}:{}: {} ({}): {}", hostname, servname, error::name(errno), errno, error::description(errno));
	freeaddrinfo(addrinfo);
	return -1;

}


BaseTCP::BaseTCP(const std::shared_ptr<Worker>& parent_, ev::loop_ref* ev_loop_, unsigned int ev_flags_, const char* description, int flags)
	: TCP(description, flags),
	  Worker(parent_, ev_loop_, ev_flags_)
{
}


BaseTCP::~BaseTCP() noexcept
{
	try {
		Worker::deinit();

		TCP::close();
	} catch (...) {
		L_EXC("Unhandled exception in destructor");
	}
}


void
BaseTCP::shutdown_impl(long long asap, long long now)
{
	L_CALL("BaseTCP::shutdown_impl({}, {})", asap, now);

	Worker::shutdown_impl(asap, now);

	if (asap) {
		stop(false);
		destroy(false);

		if (now != 0) {
			if (is_runner()) {
				break_loop(false);
			} else {
				detach(false);
			}
		}
	}
}


void
BaseTCP::destroy_impl()
{
	L_CALL("BaseTCP::destroy_impl()");

	Worker::destroy_impl();

	TCP::close();
}
