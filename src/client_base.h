/*
 * Copyright (C) 2015-2018 deipi.com LLC and contributors
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

#pragma once

#include "xapiand.h"

#include <atomic>        // for atomic_bool, atomic_int
#include <memory>        // for shared_ptr, unique_ptr
#include <string.h>      // for size_t, memcpy, strlen
#include <string>        // for string
#include <sys/types.h>   // for ssize_t
#include <time.h>        // for time_t

#include "endpoint.h"    // for Endpoints
#include "ev/ev++.h"     // for async, io, loop_ref (ptr only)
#include "queue.h"       // for Queue
#include "threadpool.h"  // for Task
#include "worker.h"      // for Worker


class BaseServer;
class LZ4CompressFile;

//
//   Buffer class - allow for output buffering such that it can be written out
//                                 into async pieces
//

class Buffer {
	size_t len;
	char *data;

public:
	size_t pos;
	char type;

	Buffer(char type_, const char *bytes, size_t nbytes)
		: len(nbytes),
		  data(new char [len]),
		  pos(0),
		  type(type_)
	{
		memcpy(data, bytes, len);
	}

	Buffer(const Buffer&) = delete;
	Buffer& operator=(const Buffer&) = delete;

	virtual ~Buffer() {
		delete [] data;
	}

	const char *dpos() {
		return data + pos;
	}

	size_t nbytes() {
		return len - pos;
	}
};


enum class MODE;
enum class WR;


class ClientDecompressor;


class BaseClient : public Task<>, public Worker {
	friend LZ4CompressFile;

	WR _write(int fd);

	void destroyer();
	void stop();

	std::mutex _mutex;

protected:
	BaseClient(const std::shared_ptr<BaseServer>& server_, ev::loop_ref* ev_loop_, unsigned int ev_flags_, int sock_);

public:
	virtual ~BaseClient();

	virtual void on_read_file(const char *buf, ssize_t received) = 0;

	virtual void on_read_file_done() = 0;

	virtual void on_read(const char *buf, ssize_t received) = 0;

	void close();

	bool write(const char *buf, size_t buf_size);

	inline bool write(const char *buf) {
		return write(buf, strlen(buf));
	}

	inline bool write(const std::string &buf) {
		return write(buf.c_str(), buf.size());
	}

protected:
	ev::io io_read;
	ev::io io_write;
	ev::async update_async;
	ev::async read_start_async;

	std::atomic_bool idle;
	std::atomic_bool shutting_down;
	std::atomic_bool closed;
	std::atomic_int sock;
	int written;
	std::string length_buffer;

	std::unique_ptr<ClientDecompressor> decompressor;
	char *read_buffer;

	MODE mode;
	ssize_t file_size;
	size_t block_size;
	bool receive_checksum;

	Endpoints endpoints;

	queue::Queue<std::shared_ptr<Buffer>> write_queue;

	void update_async_cb(ev::async &watcher, int revents);
	void read_start_async_cb(ev::async &watcher, int revents);

	void io_cb_update();

	// Generic callback
	void io_cb(ev::io &watcher, int revents);

	// Receive message from client socket
	void io_cb_read(int fd);

	// Socket is writable
	void io_cb_write(int fd);

	WR write_directly(int fd);

	void read_file();
	bool send_file(int fd, size_t offset=0);

	void destroy_impl() override;
	void shutdown_impl(time_t asap, time_t now) override;
};
