#include <sys/socket.h>

#include "net/length.h"

#include "server.h"
#include "utils.h"
#include "client_binary.h"

//
// Xapian binary client
//

BinaryClient::BinaryClient(int sock_, ThreadPool *thread_pool_, DatabasePool *database_pool_, double active_timeout_, double idle_timeout_)
	: BaseClient(sock_, thread_pool_, database_pool_, active_timeout_, idle_timeout_),
	  RemoteProtocol(std::vector<std::string>(), active_timeout_, idle_timeout_, true),
	  database(NULL)
{
	printf("Got connection, %d binary client(s) connected.\n", ++total_clients);

	try {
		msg_update(std::string());
	} catch (const Xapian::NetworkError &e) {
		printf("ERROR: %s\n", e.get_msg().c_str());
	} catch (...) {
		printf("ERROR!\n");
	}
}


BinaryClient::~BinaryClient()
{
	printf("Lost connection, %d binary client(s) connected.\n", --total_clients);
}


void BinaryClient::read_cb(ev::io &watcher)
{
	char buf[1024];

	ssize_t received = recv(watcher.fd, buf, sizeof(buf), 0);

	if (received < 0) {
		perror("read error");
		return;
	}

	if (received == 0) {
		// Gack - we're deleting ourself inside of ourself!
		delete this;
	} else {
		buffer.append(buf, received);
		if (buffer.length() >= 2) {
			const char *o = buffer.data();
			const char *p = o;
			const char *p_end = p + buffer.size();

			message_type type = static_cast<message_type>(*p++);
			size_t len;
			try {
				len = decode_length(&p, p_end, true);
			} catch (const Xapian::NetworkError & e) {
				return;
			}
			std::string data = std::string(p, len);
			buffer.erase(0, p - o + len);

			// printf("<<< ");
			// print_string(data);

			Buffer *msg = new Buffer(type, data.c_str(), data.size());

			messages_queue.push(msg);

			if (type != MSG_GETMSET && type != MSG_SHUTDOWN) {
				thread_pool->addTask(new ClientWorker(this));
			}
		}
	}
}


message_type BinaryClient::get_message(double timeout, std::string & result, message_type required_type)
{
	Buffer* msg;
	if (!messages_queue.pop(msg)) {
		throw Xapian::NetworkError("No message available");
	}

	std::string buf(&msg->type, 1);
	buf += encode_length(msg->nbytes());
	buf += std::string(msg->dpos(), msg->nbytes());
	printf("get_message:");
	print_string(buf);

	message_type type = static_cast<message_type>(msg->type);
	result.assign(msg->dpos(), msg->nbytes());

	delete msg;
	return type;
}


void BinaryClient::send_message(reply_type type, const std::string &message) {
	char type_as_char = static_cast<char>(type);
	std::string buf(&type_as_char, 1);
	buf += encode_length(message.size());
	buf += message;

	printf("send_message:");
	print_string(buf);

	send(buf.c_str(), buf.size());
}


void BinaryClient::send_message(reply_type type, const std::string &message, double end_time)
{
	send_message(type, message);
}


Xapian::Database * BinaryClient::get_db(bool writable_)
{
	if (endpoints.empty()) {
		return NULL;
	}
	if (!database_pool->checkout(&database, endpoints, writable_)) {
		return NULL;
	}
	return database->db;
}


void BinaryClient::release_db(Xapian::Database *db_)
{
	if (database) {
		database_pool->checkin(&database);
	}
}


void BinaryClient::select_db(const std::vector<std::string> &dbpaths_, bool writable_)
{
	std::vector<std::string>::const_iterator i(dbpaths_.begin());
	for (; i != dbpaths_.end(); i++) {
		Endpoint endpoint = Endpoint(*i, std::string(), XAPIAND_BINARY_PORT_DEFAULT);
		endpoints.push_back(endpoint);
	}
	dbpaths = dbpaths_;
}


void BinaryClient::run()
{
	try {
		run_one();
	} catch (const Xapian::NetworkError &e) {
		printf("ERROR: %s\n", e.get_msg().c_str());
	} catch (...) {
		printf("ERROR!\n");
	}
}
