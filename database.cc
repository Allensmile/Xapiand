//
//  database.cpp
//  Xapiand
//
//  Created by Germán M. Bravo on 2/23/15.
//  Copyright (c) 2015 Germán M. Bravo. All rights reserved.
//

#include "database.h"


Database::Database(Endpoints &endpoints_, bool writable_)
	: endpoints(endpoints_),
	  writable(writable_)
{
	hash = endpoints.hash(writable);
	reopen();
}


void
Database::reopen()
{
	// FIXME: Handle remote endpoints and figure out if the endpoint is a local database
	if (writable) {
		db = new Xapian::WritableDatabase(endpoints[0].path, Xapian::DB_CREATE_OR_OPEN);
	} else {
		db = new Xapian::Database(endpoints[0].path, Xapian::DB_CREATE_OR_OPEN);
		if (!writable) {
			std::vector<Endpoint>::const_iterator i(endpoints.begin());
			for (++i; i != endpoints.end(); ++i) {
				db->add_database(Xapian::Database((*i).path));
			}
		} else if (endpoints.size() != 1) {
			printf("ERROR: Expecting exactly one database.");
		}
	}
}


Database::~Database()
{
	delete db;
}


DatabaseQueue::~DatabaseQueue()
{
	//		std::queue<Database *>::const_iterator i(queue.begin());
	//		for (; i != databases.end(); ++i) {
	//			(*i).second.finish();
	//		}
}


DatabasePool::DatabasePool()
{
	pthread_mutex_init(&qmtx, 0);
}


DatabasePool::~DatabasePool()
{
	finish();
	pthread_mutex_destroy(&qmtx);
}


void DatabasePool::finish() {
	pthread_mutex_lock(&qmtx);
	
	finished = true;
	
	pthread_mutex_unlock(&qmtx);
}


bool
DatabasePool::checkout(Database **database, Endpoints &endpoints, bool writable)
{
	Database *database_ = NULL;
	
	pthread_mutex_lock(&qmtx);
	
	if (!finished && *database == NULL) {
		size_t hash = endpoints.hash(writable);
		DatabaseQueue &queue = databases[hash];
		
		if (!queue.pop(database_, 0)) {
			if (!writable || queue.instances_count == 0) {
				database_ = new Database(endpoints, writable);
				queue.instances_count++;
			}
			// FIXME: lock until a database is available if it can't get one
		}
		*database = database_;
	}
	
	pthread_mutex_unlock(&qmtx);
	
	return database_ != NULL;
}


void
DatabasePool::checkin(Database **database)
{
	pthread_mutex_lock(&qmtx);
	
	DatabaseQueue &queue = databases[(*database)->hash];
	
	queue.push(*database);
	
	*database = NULL;
	
	pthread_mutex_unlock(&qmtx);
}
