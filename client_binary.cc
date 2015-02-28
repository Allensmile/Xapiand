#include <sys/socket.h>

#include "net/length.h"

#include "server.h"
#include "utils.h"
#include "client_binary.h"
#include "xapiand.h"

//
// Xapian binary client
//

BinaryClient::BinaryClient(ev::loop_ref *loop, int sock_, DatabasePool *database_pool_, double active_timeout_, double idle_timeout_)
	: BaseClient(loop, sock_, database_pool_, active_timeout_, idle_timeout_),
	  RemoteProtocol(std::vector<std::string>(), active_timeout_, idle_timeout_, true),
	  database(NULL)
{
	LOG_CONN(this, "Got connection (sock=%d), %d binary client(s) connected.\n", sock, ++total_clients);

	try {
		msg_update(std::string());
	} catch (const Xapian::NetworkError &e) {
		LOG_ERR(this, "ERROR: %s\n", e.get_msg().c_str());
	} catch (...) {
		LOG_ERR(this, "ERROR!\n");
	}
}


BinaryClient::~BinaryClient()
{
	if (database) {
		database_pool->checkin(&database);
	}
	total_clients--;
}


void BinaryClient::on_read(const char *buf, ssize_t received)
{
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
		
		Buffer *msg = new Buffer(type, data.c_str(), data.size());
		
		messages_queue.push(msg);
		
		try {
			run_one();
		} catch (const Xapian::NetworkError &e) {
			LOG_ERR(this, "ERROR: %s\n", e.get_msg().c_str());
		} catch (...) {
			LOG_ERR(this, "ERROR!\n");
		}
	}
}


message_type BinaryClient::get_message(double timeout, std::string & result, message_type required_type)
{
	Buffer* msg;
	if (!messages_queue.pop(msg)) {
		throw Xapian::NetworkError("No message available");
	}

	const char *msg_str = msg->dpos();
	size_t msg_size = msg->nbytes();

	std::string message = std::string(msg_str, msg_size);

	std::string buf(&msg->type, 1);
	buf += encode_length(msg_size);
	buf += message;
	LOG_BINARY_PROTO(this, "get_message: '%s'\n", repr(buf).c_str());

	result = message;

	message_type type = static_cast<message_type>(msg->type);

	delete msg;

	return type;
}


void BinaryClient::send_message(reply_type type, const std::string &message) {
	char type_as_char = static_cast<char>(type);
	std::string buf(&type_as_char, 1);
	buf += encode_length(message.size());
	buf += message;

	LOG_BINARY_PROTO(this, "send_message: '%s'\n", repr(buf).c_str());

	write(buf);
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
		Endpoint endpoint = Endpoint(*i, std::string(), XAPIAND_BINARY_SERVERPORT);
		endpoints.push_back(endpoint);
	}
	dbpaths = dbpaths_;
}
