#ifndef XAPIAND_INCLUDED_SERVER_H
#define XAPIAND_INCLUDED_SERVER_H

#include <ev++.h>

#include "threadpool.h"
#include "database.h"


class XapiandServer : public Task {
private:
	ev::dynamic_loop dynamic_loop;
	ev::loop_ref *loop;
	ev::sig sig;
	ev::async quit;

	ev::io http_io;
	int http_sock;

	ev::io binary_io;
	int binary_sock;

	DatabasePool database_pool;

	void bind_http();
	void bind_binary();

	void io_accept_http(ev::io &watcher, int revents);
	void io_accept_binary(ev::io &watcher, int revents);

	void signal_cb(ev::sig &signal, int revents);
	void quit_cb(ev::async &watcher, int revents);

public:
	XapiandServer(ev::loop_ref *loop_, int http_sock_, int binary_sock_);
	~XapiandServer();
	
	void run();
};

#endif /* XAPIAND_INCLUDED_SERVER_H */
