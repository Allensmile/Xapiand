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

#pragma once

#include "xapiand.h"

#include <stdio.h>        // for snprintf
#include <time.h>         // for time_t
#include <atomic>         // for atomic, atomic_bool
#include <chrono>         // for time_point, system_clock
#include <memory>         // for shared_ptr, weak_ptr
#include <mutex>          // for mutex, condition_variable
#include <string>         // for string
#include <unordered_map>  // for unordered_map

#include "threadpool.h"   // for Task
#include "worker.h"       // for Worker

class Database;
class Endpoints;
class XapiandManager;


class DatabaseAutocommit : public Task<>, public Worker {
	struct Status {
		std::weak_ptr<const Database> weak_database;
		std::chrono::time_point<std::chrono::system_clock> max_commit_time;
		std::chrono::time_point<std::chrono::system_clock> commit_time;
		std::chrono::time_point<std::chrono::system_clock> next_wakeup_time();
	};

	static std::mutex mtx;
	static std::mutex statuses_mtx;
	static std::condition_variable wakeup_signal;
	static std::unordered_map<Endpoints, Status> statuses;
	static std::atomic_ullong next_wakeup_time;

	std::atomic_bool running;

	void destroyer();

	void destroy_impl() override;
	void shutdown_impl(time_t asap, time_t now) override;

	void run_one(std::unique_lock<std::mutex>& lk);

public:
	std::string __repr__() const override {
		char buffer[100];
		snprintf(buffer, sizeof(buffer), "<DatabaseAutocommit at %p>", this);
		return buffer;
	}

	DatabaseAutocommit(const std::shared_ptr<XapiandManager>& manager_, ev::loop_ref* ev_loop_, unsigned int ev_flags_);
	~DatabaseAutocommit();

	void run() override;

	static void commit(const std::shared_ptr<Database>& database);
};
