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

#include "udp.h"

#include <arpa/inet.h>              // for inet_addr, htonl, htons
#include <cstring>                  // for strerror, memset
#include <utility>
#include <errno.h>                  // for __error, errno
#include <fcntl.h>                  // for fcntl, F_GETFL, F_SETFL, O_NONBLOCK
#include <sys/socket.h>             // for setsockopt, bind, recvfrom, sendto
#include <sysexits.h>               // for EX_CONFIG

#include "exception.h"              // for MSG_NetworkError, NetworkError
#include "io.hh"                    // for close, ignored_errno
#include "length.h"                 // for serialise_string, unserialise_string
#include "log.h"                    // for L_ERR, L_OBJ, L_CRIT, L_CONN
#include "manager.h"                // for XapiandManager, sig_exit, Xapiand...
#include "opts.h"                   // for opts


UDP::UDP(int port, const char* description, uint8_t major_version, uint8_t minor_version, const std::string& group, int flags, int tries)
	: port(port),
	  sock(-1),
	  closed(false),
	  flags(flags),
	  description(description),
	  major_version(major_version),
	  minor_version(minor_version)
{
	bind(tries, group);
}


UDP::~UDP()
{
	if (sock != -1) {
		io::close(sock);
	}
}


void
UDP::close() {
	if (!closed.exchange(true)) {
		io::shutdown(sock, SHUT_RDWR);
	}
}


void
UDP::bind(int tries, const std::string& group)
{
	int optval = 1;
	unsigned char ttl = 3;
	struct ip_mreq mreq;

	if ((sock = io::socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
		L_CRIT("ERROR: %s socket: [%d] %s", description, errno, strerror(errno));
		sig_exit(-EX_CONFIG);
	}

	if (io::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
		L_ERR("ERROR: %s setsockopt SO_REUSEADDR (sock=%d): [%d] %s", description, sock, errno, strerror(errno));
	}

	if ((flags & UDP_SO_REUSEPORT) != 0) {
		if (io::setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) == -1) {
			L_ERR("ERROR: %s setsockopt SO_REUSEPORT (sock=%d): [%d] %s", description, sock, errno, strerror(errno));
		}
	}

	if (io::setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &optval, sizeof(optval)) == -1) {
		L_ERR("ERROR: %s setsockopt IP_MULTICAST_LOOP (sock=%d): [%d] %s", description, sock, errno, strerror(errno));
	}

	if (io::setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) == -1) {
		L_ERR("ERROR: %s setsockopt IP_MULTICAST_TTL (sock=%d): [%d] %s", description, sock, errno, strerror(errno));
	}

	// use io::setsockopt() to request that the kernel join a multicast group
	memset(&mreq, 0, sizeof(mreq));
	mreq.imr_multiaddr.s_addr = inet_addr(group.c_str());
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);
	if (io::setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == -1) {
		L_CRIT("ERROR: %s setsockopt IP_ADD_MEMBERSHIP (sock=%d): [%d] %s", description, sock, errno, strerror(errno));
		close();
		sig_exit(-EX_CONFIG);
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);  // bind to all addresses (differs from sender)

	for (int i = 0; i < tries; ++i, ++port) {
		addr.sin_port = htons(port);

		if (io::bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
			if (!io::ignored_errno(errno, true, true, true)) {
				if (i == tries - 1) { break; }
				L_DEBUG("ERROR: %s bind error (sock=%d): [%d] %s", description, sock, errno, strerror(errno));
				continue;
			}
		}

		if (io::fcntl(sock, F_SETFL, io::fcntl(sock, F_GETFL, 0) | O_NONBLOCK) == -1) {
			L_CRIT("ERROR: fcntl O_NONBLOCK (sock=%d): [%d] %s", sock, errno, strerror(errno));
			sig_exit(-EX_CONFIG);
		}

		addr.sin_addr.s_addr = inet_addr(group.c_str());  // setup s_addr for sender (send to group)

		// Flush socket
		L_DELAYED_1000("UDP flush is taking too long...");
		while (true) {
			char buf[1024];
			ssize_t received = io::recv(sock, buf, sizeof(buf), 0);
			if (received < 0 && !io::ignored_errno(errno, false, true, true)) {
				break;
			}
		}
		L_DELAYED_N_CLEAR();

		return;
	}

	L_CRIT("ERROR: %s bind error (sock=%d): [%d] %s", description, sock, errno, strerror(errno));
	close();
	sig_exit(-EX_CONFIG);
}


void
UDP::sending_message(const std::string& message)
{
	if (!closed) {
		L_UDP_WIRE("(sock=%d) <<-- %s", sock, repr(message));

#ifdef MSG_NOSIGNAL
		ssize_t written = io::sendto(sock, message.c_str(), message.size(), MSG_NOSIGNAL, (struct sockaddr *)&addr, sizeof(addr));
#else
		ssize_t written = io::sendto(sock, message.c_str(), message.size(), 0, (struct sockaddr *)&addr, sizeof(addr));
#endif

		if (written < 0) {
			if (!io::ignored_errno(errno, true, true, true)) {
				L_ERR("ERROR: sendto error (sock=%d): %s", sock, strerror(errno));
				XapiandManager::manager->shutdown();
			}
		}
	}
}


void
UDP::send_message(char type, const std::string& content)
{
	if (!content.empty()) {
		std::string message;
		message.push_back(major_version);
		message.push_back(minor_version);
		message.push_back(type);
		message.append(serialise_string(opts.cluster_name));
		message.append(content);
		sending_message(message);
	}
}


char
UDP::get_message(std::string& result, char max_type)
{
	char buf[1024];
	ssize_t received = io::recv(sock, buf, sizeof(buf), 0);
	if (received < 0) {
		if (!io::ignored_errno(errno, true, true, true)) {
			L_ERR("ERROR: read error (sock=%d): %s", sock, strerror(errno));
			THROW(NetworkError, strerror(errno));
		}
		L_CONN("Received EOF (sock=%d)!", sock);
		return '\xff';
	} else if (received == 0) {
		// If no messages are available to be received and the peer has performed an orderly shutdown.
		L_CONN("Received EOF (sock=%d)!", sock);
		return '\xff';
	} else if (received < 4) {
		L_CONN("Badly formed message: Incomplete!");
	}

	L_UDP_WIRE("(sock=%d) -->> %s", sock, repr(buf, received));

	const char *p = buf;
	const char *p_end = p + received;

	uint8_t received_major_version = *p++;
	uint8_t received_minor_version = *p++;
	if (received_major_version > major_version || (received_major_version == major_version && received_minor_version > minor_version)) {
		L_CONN("Badly formed message: Protocol version mismatch!");
		return '\xff';
	}

	char type = *p++;
	if (type >= max_type) {
		L_CONN("Badly formed message: Invalid message type %u", unsigned(type));
		return '\xff';
	}

	auto remote_cluster_name = unserialise_string(&p, p_end);
	if (remote_cluster_name.empty()) {
		L_CONN("Badly formed message: No cluster name!");
		return '\xff';
	}

	if (remote_cluster_name != opts.cluster_name) {
		return '\xff';
	}

	result = std::string(p, p_end - p);
	return type;
}
