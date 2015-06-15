#ifndef XAPIAND_INCLUDED_SERVER_H
#define XAPIAND_INCLUDED_SERVER_H

#include <list>

#include <unistd.h>
#include <ev++.h>

#include "queue.h"
#include "threadpool.h"

#include "net/remoteserver.h"

struct Buffer;


class XapiandServer {
private:
	ev::io io;
	ev::sig sig;

	int sock;
	ThreadPool *thread_pool;

public:
	void io_accept(ev::io &watcher, int revents);

	static void signal_cb(ev::sig &signal, int revents);

	XapiandServer(int port, ThreadPool *thread_pool);

	virtual ~XapiandServer();
};


//
//   A single instance of a non-blocking Xapiand handler
//
class XapiandClient : public RemoteProtocol {
private:
	ev::io io;
	ev::async async;
	ev::sig sig;

	int sock;
	ThreadPool *thread_pool;

	std::vector<std::string> dbpaths;

	static int total_clients;

	pthread_mutex_t qmtx;

	// Buffers that are pending write
	Queue<Buffer *> messages_queue;
	std::list<Buffer *> write_queue;
	std::string buffer;

	void async_cb(ev::async &watcher, int revents);

	// Generic callback
	void callback(ev::io &watcher, int revents);

	// Socket is writable
	void write_cb(ev::io &watcher);

	// Receive message from client socket
	void read_cb(ev::io &watcher);

	void signal_cb(ev::sig &signal, int revents);

	// effictivly a close and a destroy
	virtual ~XapiandClient();

public:
    void run();

	message_type get_message(double timeout, std::string & result, message_type required_type = MSG_MAX);
	void send_message(reply_type type, const std::string &message);
	void send_message(reply_type type, const std::string &message, double end_time);
	
	Xapian::Database * get_db(bool);
	void release_db(Xapian::Database *);
	Xapian::Database * select_db(const std::vector<std::string> &, bool);
	
	XapiandClient(int s, ThreadPool *thread_pool, double active_timeout_, double idle_timeout_);
};

#endif /* XAPIAND_INCLUDED_SERVER_H */
