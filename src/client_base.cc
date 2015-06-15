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

#include <assert.h>
#include <sys/socket.h>

#include "xapiand.h"
#include "utils.h"
#include "client_base.h"


const int WRITE_QUEUE_SIZE = -1;


BaseClient::BaseClient(XapiandServer *server_, ev::loop_ref *loop, int sock_, DatabasePool *database_pool_, ThreadPool *thread_pool_, double active_timeout_, double idle_timeout_)
	: server(server_),
	  iterator(server_->clients.end()),
	  io_read(*loop),
	  io_write(*loop),
	  async_write(*loop),
	  closed(false),
	  sock(sock_),
	  database_pool(database_pool_),
	  thread_pool(thread_pool_),
	  write_queue(WRITE_QUEUE_SIZE)
{
	inc_ref();

	pthread_mutex_init(&qmtx, 0);

	pthread_mutex_lock(&qmtx);
	XapiandServer::total_clients++;
	pthread_mutex_unlock(&qmtx);
	
	async_write.set<BaseClient, &BaseClient::async_write_cb>(this);
	async_write.start();

	io_read.set<BaseClient, &BaseClient::io_cb>(this);
	io_read.start(sock, ev::READ);

	io_write.set<BaseClient, &BaseClient::io_cb>(this);
	io_write.set(sock, ev::WRITE);

	attach_client();
}


BaseClient::~BaseClient()
{
	destroy();

	while(!write_queue.empty()) {
		Buffer *buffer;
		if (write_queue.pop(buffer, 0)) {
			delete buffer;
		}
	}

	pthread_mutex_lock(&qmtx);
	XapiandServer::total_clients--;
	pthread_mutex_unlock(&qmtx);

	if (XapiandServer::total_clients == 0 && server->manager->shutdown_asap == 0) {
		server->manager->async_shutdown.send();
	}

	pthread_mutex_destroy(&qmtx);

	detach_client();
}


void BaseClient::destroy()
{
	if (sock == -1) {
		return;
	}

	close();
	
	// Stop and free watcher if client socket is closing
	io_read.stop();
	io_write.stop();
	
	pthread_mutex_lock(&qmtx);
	::close(sock);
	sock = -1;
	pthread_mutex_unlock(&qmtx);

	LOG_OBJ(this, "DESTROYED!\n");
}


void BaseClient::close() {
	if (closed) {
		return;
	}

	closed = true;
	LOG_OBJ(this, "CLOSED!\n");
}


void BaseClient::io_update() {
	if (sock != -1) {
		if (write_queue.empty()) {
			if (closed) {
				destroy();
			} else {
				io_write.stop();
			}
		} else {
			io_write.start();
		}
	}
	
	if (sock == -1) {
		rel_ref();
	}

}


void BaseClient::io_cb(ev::io &watcher, int revents)
{
	if (revents & EV_ERROR) {
		LOG_ERR(this, "ERROR: got invalid event (sock=%d): %s\n", sock, strerror(errno));
		destroy();
		return;
	}

	LOG_EV(this, "%s (sock=%d) %x\n", (revents & EV_WRITE & EV_READ) ? "IO_CB" : (revents & EV_WRITE) ? "WRITE_CB" : (revents & EV_READ) ? "READ_CB" : "IO_CB", sock, revents);

	if (sock == -1) {
		return;
	}
	
	assert(sock == watcher.fd);

	if (sock != -1 && revents & EV_WRITE) {
		write_cb();
	}

	if (sock != -1 && revents & EV_READ) {
		read_cb();
	}
	
	io_update();
}


void BaseClient::write_cb()
{
	if (!write_queue.empty()) {
		Buffer* buffer = write_queue.front();
		
		size_t buf_size = buffer->nbytes();
		const char * buf = buffer->dpos();

		LOG_CONN(this, "(sock=%d) <<-- '%s'\n", sock, repr(buf, buf_size).c_str());

		ssize_t written = ::write(sock, buf, buf_size);
		
		if (written < 0) {
			if (errno != EAGAIN) {
				LOG_ERR(this, "ERROR: write error (sock=%d): %s\n", sock, strerror(errno));
				destroy();
			}
		} else if (written == 0) {
			// nothing written?
		} else {
			buffer->pos += written;
			if (buffer->nbytes() == 0) {
				if(write_queue.pop(buffer)) {
					delete buffer;
				}
			}
		}
	}
}

void BaseClient::read_cb()
{
	char buf[1024];

	ssize_t received = ::read(sock, buf, sizeof(buf));
	
	if (received < 0) {
		if (errno != EAGAIN) {
			LOG_ERR(this, "ERROR: read error (sock=%d): %s\n", sock, strerror(errno));
			destroy();
		}
	} else if (received == 0) {
		// The peer has closed its half side of the connection.
		LOG_CONN(this, "Received EOF (sock=%d)!\n", sock);
		destroy();
	} else {
		LOG_CONN(this, "(sock=%d) -->> '%s'\n", sock, repr(buf, received).c_str());
		on_read(buf, received);
	}
}


void BaseClient::async_write_cb(ev::async &watcher, int revents)
{
	io_update();
}

					
void BaseClient::write(const char *buf, size_t buf_size)
{
	LOG_CONN(this, "(sock=%d) <ENQUEUE> '%s'\n", sock, repr(buf, buf_size).c_str());

	Buffer *buffer = new Buffer('\0', buf, buf_size);
	write_queue.push(buffer);

	async_write.send();
}

void BaseClient::shutdown()
{
	if (server->manager->shutdown_now) {
		LOG_EV(this, "Signaled destroy!!\n");
		destroy();
		rel_ref();
	}
}

void BaseClient::attach_client()
{
	pthread_mutex_lock(&server->clients_mutex);
	assert(iterator == server->clients.end());
	iterator = server->clients.insert(server->clients.end(), this);
	pthread_mutex_unlock(&server->clients_mutex);
}


void BaseClient::detach_client()
{
	pthread_mutex_lock(&server->clients_mutex);
	if (iterator != server->clients.end()) {
		server->clients.erase(iterator);
		iterator = server->clients.end();
	}
	pthread_mutex_unlock(&server->clients_mutex);
}
