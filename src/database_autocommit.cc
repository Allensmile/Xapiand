/*
 * Copyright (C) 2015,2016 deipi.com LLC and contributors. All rights reserved.
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

#include "database_autocommit.h"

#include "database.h"        // for Database, DatabasePool
#include "endpoint.h"        // for Endpoints
#include "log.h"             // for Log, L_OBJ, L_CALL, L_DEBUG, L_WARNING
#include "manager.h"         // for XapiandManager
#include "utils.h"           // for delta_string


std::mutex DatabaseAutocommit::statuses_mtx;
std::unordered_map<Endpoints, std::shared_ptr<DatabaseAutocommit::Status>> DatabaseAutocommit::statuses;


DatabaseAutocommit::DatabaseAutocommit(bool forced_, Endpoints endpoints_, std::weak_ptr<const Database> weak_database_)
	: forced(forced_),
	  endpoints(endpoints_),
	  weak_database(weak_database_) { }


void
DatabaseAutocommit::commit(const std::shared_ptr<Database>& database)
{
	L_CALL(nullptr, "DatabaseAutocommit::commit(<database>)");

	std::shared_ptr<DatabaseAutocommit> task;
	std::chrono::time_point<std::chrono::system_clock> next_wakeup_time;

	{
		auto now = std::chrono::system_clock::now();

		std::lock_guard<std::mutex> statuses_lk(DatabaseAutocommit::statuses_mtx);
		auto& status = DatabaseAutocommit::statuses[database->endpoints];

		if (!status) {
			status = std::make_shared<DatabaseAutocommit::Status>();
			status->max_wakeup_time = now + 9s;
			status->wakeup_time = now;
		}

		bool forced;
		next_wakeup_time = now + 3s;
		if (next_wakeup_time > status->max_wakeup_time) {
			next_wakeup_time = status->max_wakeup_time;
			forced = true;
		} else {
			forced = false;
		}
		if (next_wakeup_time == status->wakeup_time) {
			return;
		}

		if (status->task) {
			status->task->clear();
		}
		status->wakeup_time = next_wakeup_time;
		status->task = std::make_shared<DatabaseAutocommit>(forced, database->endpoints, database);
		task = status->task;
	}

	scheduler().add(task, next_wakeup_time);
}


void
DatabaseAutocommit::run()
{
	L_CALL(this, "DatabaseAutocommit::run()");

	{
		std::lock_guard<std::mutex> statuses_lk(DatabaseAutocommit::statuses_mtx);
		DatabaseAutocommit::statuses.erase(endpoints);
	}

	if (weak_database.lock()) {
		bool successful = false;
		auto start = std::chrono::system_clock::now();
		std::shared_ptr<Database> database;
		if (XapiandManager::manager->database_pool.checkout(database, endpoints, DB_WRITABLE)) {
			try {
				database->commit();
				successful = true;
			} catch (const Error& e) {}
			XapiandManager::manager->database_pool.checkin(database);
		}
		auto end = std::chrono::system_clock::now();

		if (successful) {
			L_DEBUG(this, "Autocommit: %s%s (took %s)", repr(endpoints.to_string()).c_str(), forced ? " (forced)" : "", delta_string(start, end).c_str());
		} else {
			L_WARNING(this, "Autocommit falied: %s%s (took %s)", repr(endpoints.to_string()).c_str(), forced ? " (forced)" : "", delta_string(start, end).c_str());
		}
	}
}
