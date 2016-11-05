/*
 * Copyright (C) 2015 deipi.com LLC and contributors. All rights reserved.
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

#include <algorithm>         // for move
#include <ctime>             // for time_t
#include <ratio>             // for ratio
#include <type_traits>       // for remove_reference<>::type
#include <utility>           // for pair

#include "database.h"        // for Database, DatabasePool
#include "database_utils.h"  // for DB_WRITABLE
#include "endpoint.h"        // for Endpoints, hash, operator==
#include "exception.h"       // for Error
#include "log.h"             // for Log, L_OBJ, L_CALL, L_DEBUG, L_WARNING
#include "manager.h"         // for XapiandManager, XapiandManager::manager
#include "utils.h"           // for delta_string, repr


std::mutex DatabaseAutocommit::mtx;
std::mutex DatabaseAutocommit::statuses_mtx;
std::condition_variable DatabaseAutocommit::wakeup_signal;
std::unordered_map<Endpoints, DatabaseAutocommit::Status> DatabaseAutocommit::statuses;
std::atomic<std::time_t> DatabaseAutocommit::next_wakeup_time(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() + 10s));


std::chrono::time_point<std::chrono::system_clock>
DatabaseAutocommit::Status::next_wakeup_time()
{
	return max_commit_time < commit_time ? max_commit_time : commit_time;
}


DatabaseAutocommit::DatabaseAutocommit(const std::shared_ptr<XapiandManager>& manager_, ev::loop_ref* ev_loop_, unsigned int ev_flags_)
	: Worker(std::move(manager_), ev_loop_, ev_flags_),
	  running(true)
{
	L_OBJ(this, "CREATED AUTOCOMMIT!");
}


DatabaseAutocommit::~DatabaseAutocommit()
{
	destroyer();

	L_OBJ(this , "DELETED AUTOCOMMIT!");
}


void
DatabaseAutocommit::destroy_impl()
{
	destroyer();
}


void
DatabaseAutocommit::destroyer()
{
	L_CALL(this, "DatabaseAutocommit::destroyer()");

	running.store(false);
	auto now = std::chrono::system_clock::now();
	DatabaseAutocommit::next_wakeup_time.store(std::chrono::system_clock::to_time_t(now + 100ms));
	wakeup_signal.notify_all();
}


void
DatabaseAutocommit::shutdown_impl(time_t asap, time_t now)
{
	L_CALL(this, "DatabaseAutocommit::shutdown_impl(%d, %d)", (int)asap, (int)now);

	Worker::shutdown_impl(asap, now);

	if (now) {
		destroy();
		detach();
	}
}


void
DatabaseAutocommit::run_loop(std::unique_lock<std::mutex>& lk)
{
	L_CALL(this, "DatabaseAutocommit::run_loop()");

	std::unique_lock<std::mutex> statuses_lk(DatabaseAutocommit::statuses_mtx);

	auto now = std::chrono::system_clock::now();
	DatabaseAutocommit::next_wakeup_time.store(std::chrono::system_clock::to_time_t(now + (running ? 20s : 100ms)));

	for (auto it = DatabaseAutocommit::statuses.begin(); it != DatabaseAutocommit::statuses.end(); ) {
		auto status = it->second;
		if (status.weak_database.lock()) {  // If database still exists, autocommit
			auto next_wakeup_time = status.next_wakeup_time();
			if (next_wakeup_time <= now) {
				auto endpoints = it->first;
				DatabaseAutocommit::statuses.erase(it);
				statuses_lk.unlock();
				lk.unlock();

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
					L_DEBUG(this, "Autocommit: %s%s (took %s)", repr(endpoints.to_string()).c_str(), next_wakeup_time == status.max_commit_time ? " (forced)" : "", delta_string(start, end).c_str());
				} else {
					L_WARNING(this, "Autocommit failed: %s%s (took %s)", repr(endpoints.to_string()).c_str(), next_wakeup_time == status.max_commit_time ? " (forced)" : "", delta_string(start, end).c_str());
				}

				lk.lock();
				statuses_lk.lock();
				it = DatabaseAutocommit::statuses.begin();
			} else if (std::chrono::system_clock::from_time_t(DatabaseAutocommit::next_wakeup_time.load()) > next_wakeup_time) {
				DatabaseAutocommit::next_wakeup_time.store(std::chrono::system_clock::to_time_t(next_wakeup_time));
				++it;
			} else {
				++it;
			}
		} else {
			it = DatabaseAutocommit::statuses.erase(it);
		}
	}
}


void
DatabaseAutocommit::run()
{
	L_CALL(this, "DatabaseAutocommit::run()");

	while (running) {
		std::unique_lock<std::mutex> lk(DatabaseAutocommit::mtx);
		DatabaseAutocommit::wakeup_signal.wait_until(lk, std::chrono::system_clock::from_time_t(DatabaseAutocommit::next_wakeup_time.load()));
		run_loop(lk);
	}

	cleanup();
}


void
DatabaseAutocommit::commit(const std::shared_ptr<Database>& database)
{
	L_CALL(nullptr, "DatabaseAutocommit::commit(<database>)");

	std::lock_guard<std::mutex> statuses_lk(DatabaseAutocommit::statuses_mtx);
	DatabaseAutocommit::Status& status = DatabaseAutocommit::statuses[database->endpoints];

	auto now = std::chrono::system_clock::now();
	if (!status.weak_database.lock()) {
		status.weak_database = database;
		status.max_commit_time = now + 9s;
	}
	status.commit_time = now + 3s;

	if (std::chrono::system_clock::from_time_t(DatabaseAutocommit::next_wakeup_time.load()) > status.next_wakeup_time()) {
		DatabaseAutocommit::wakeup_signal.notify_one();
	}
}
