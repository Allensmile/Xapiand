#include <netinet/in.h>
#include <sys/socket.h>

#include <xapian.h>

#include "utils.h"

#include "server.h"
#include "client_http.h"
#include "client_binary.h"


const int MSECS_IDLE_TIMEOUT_DEFAULT = 60000;
const int MSECS_ACTIVE_TIMEOUT_DEFAULT = 15000;


//
// Xapian Server
//

XapiandServer::XapiandServer(int http_sock_, int binary_sock_)
	: http_io(loop),
	  binary_io(loop),
	  quit(loop),
	  http_sock(http_sock_),
	  binary_sock(binary_sock_)
{
	http_io.set<XapiandServer, &XapiandServer::io_accept_http>(this);
	http_io.start(http_sock, ev::READ);

	binary_io.set<XapiandServer, &XapiandServer::io_accept_binary>(this);
	binary_io.start(binary_sock, ev::READ);

	quit.set<XapiandServer, &XapiandServer::quit_cb>(this);
	quit.start();

	sig.set<XapiandServer, &XapiandServer::signal_cb>(this);
	sig.start(SIGINT);
}


XapiandServer::~XapiandServer()
{
	http_io.stop();
	binary_io.stop();
	quit.stop();
	sig.stop();
}


void XapiandServer::run()
{
	log(this, "Starting loop...\n");
	loop.run(0);
}

void XapiandServer::quit_cb(ev::async &watcher, int revents)
{
	log(this, "Breaking loop!\n");
	loop.break_loop();
}

void XapiandServer::io_accept_http(ev::io &watcher, int revents)
{
	if (EV_ERROR & revents) {
		perror("got invalid event");
		return;
	}

	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);

	int client_sock = accept(watcher.fd, (struct sockaddr *)&client_addr, &client_len);

	if (client_sock < 0) {
		perror("accept error");
		http_io.stop();
		http_sock = 0;
		if (!http_sock && !binary_sock) {
			loop.break_loop();
		}
		return;
	}

	double active_timeout = MSECS_ACTIVE_TIMEOUT_DEFAULT * 1e-3;
	double idle_timeout = MSECS_IDLE_TIMEOUT_DEFAULT * 1e-3;
	new HttpClient(loop, client_sock, &database_pool, active_timeout, idle_timeout);
}


void XapiandServer::io_accept_binary(ev::io &watcher, int revents)
{
	if (EV_ERROR & revents) {
		perror("got invalid event");
		return;
	}

	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);

	int client_sock = accept(watcher.fd, (struct sockaddr *)&client_addr, &client_len);

	if (client_sock < 0) {
		perror("accept error");
		binary_io.stop();
		binary_sock = 0;
		if (!http_sock && !binary_sock) {
			loop.break_loop();
		}
		return;
	}

	double active_timeout = MSECS_ACTIVE_TIMEOUT_DEFAULT * 1e-3;
	double idle_timeout = MSECS_IDLE_TIMEOUT_DEFAULT * 1e-3;
	new BinaryClient(loop, client_sock, &database_pool, active_timeout, idle_timeout);
}


void XapiandServer::signal_cb(ev::sig &signal, int revents)
{
	log(this, "Breaking default loop!\n");
	quit.send();
	signal.loop.break_loop();
}
