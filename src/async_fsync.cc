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

#include "async_fsync.h"

#include <algorithm>    // for move
#include <ctime>        // for time_t

#include "manager.h"    // for XapiandManager
#include "io_utils.h"   // for fsync, full_fsync
#include "log.h"        // for L_OBJ, Log, L_DEBUG, L_WARNING
#include "utils.h"      // for delta_string


std::mutex AsyncFsync::mtx;
std::mutex AsyncFsync::statuses_mtx;
std::condition_variable AsyncFsync::wakeup_signal;
std::unordered_map<int, AsyncFsync::Status> AsyncFsync::statuses;
std::atomic<std::time_t> AsyncFsync::next_wakeup_time(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() + 10s));


std::chrono::time_point<std::chrono::system_clock>
AsyncFsync::Status::next_wakeup_time()
{
	return max_fsync_time < fsync_time ? max_fsync_time : fsync_time;
}


AsyncFsync::AsyncFsync(const std::shared_ptr<XapiandManager>& manager_, ev::loop_ref* ev_loop_, unsigned int ev_flags_)
	: Worker(std::move(manager_), ev_loop_, ev_flags_),
	  running(true)
{
	L_OBJ(this, "CREATED ASYNC FSYNC!");
}


AsyncFsync::~AsyncFsync()
{
	destroyer();

	L_OBJ(this , "DELETED ASYNC FSYNC!");
}


void
AsyncFsync::destroy_impl()
{
	destroyer();
}


void
AsyncFsync::destroyer()
{
	L_CALL(this, "AsyncFsync::destroyer()");

	running.store(false);
	auto now = std::chrono::system_clock::now();
	AsyncFsync::next_wakeup_time.store(std::chrono::system_clock::to_time_t(now + 100ms));
	wakeup_signal.notify_all();
}


void
AsyncFsync::shutdown_impl(time_t asap, time_t now)
{
	L_CALL(this, "AsyncFsync::shutdown_impl(%d, %d)", (int)asap, (int)now);

	Worker::shutdown_impl(asap, now);

	if (now) {
		destroy();
		detach();
	}
}


void
AsyncFsync::run_loop(std::unique_lock<std::mutex>& lk)
{
	L_CALL(this, "AsyncFsync::run_loop()");

	std::unique_lock<std::mutex> statuses_lk(AsyncFsync::statuses_mtx);

	auto now = std::chrono::system_clock::now();
	AsyncFsync::next_wakeup_time.store(std::chrono::system_clock::to_time_t(now + (running ? 20s : 100ms)));

	for (auto it = AsyncFsync::statuses.begin(); it != AsyncFsync::statuses.end(); ) {
		auto status = it->second;
		auto next_wakeup_time = status.next_wakeup_time();
		if (next_wakeup_time <= now) {
			int fd = it->first;
			AsyncFsync::statuses.erase(it);
			statuses_lk.unlock();
			lk.unlock();

			bool successful = false;
			auto start = std::chrono::system_clock::now();
			switch (status.mode) {
				case 1:
					successful = (io::full_fsync(fd) == 0);
					break;
				case 2:
					successful = (io::fsync(fd) == 0);
					break;
			}
			auto end = std::chrono::system_clock::now();

			if (successful) {
				L_DEBUG(this, "Async Fsync %d: %d%s (took %s)", status.mode, fd, next_wakeup_time == status.max_fsync_time ? " (forced)" : "", delta_string(start, end).c_str());
			} else {
				L_WARNING(this, "Async Fsync %d falied: %d%s (took %s)", status.mode, fd, next_wakeup_time == status.max_fsync_time ? " (forced)" : "", delta_string(start, end).c_str());
			}

			lk.lock();
			statuses_lk.lock();
			it = AsyncFsync::statuses.begin();
		} else if (std::chrono::system_clock::from_time_t(AsyncFsync::next_wakeup_time.load()) > next_wakeup_time) {
			AsyncFsync::next_wakeup_time.store(std::chrono::system_clock::to_time_t(next_wakeup_time));
			++it;
		} else {
			++it;
		}
	}
}


void
AsyncFsync::run()
{
	L_CALL(this, "AsyncFsync::run()");

	while (running) {
		std::unique_lock<std::mutex> lk(AsyncFsync::mtx);
		AsyncFsync::wakeup_signal.wait_until(lk, std::chrono::system_clock::from_time_t(AsyncFsync::next_wakeup_time.load()));
		run_loop(lk);
	}

	cleanup();
}


int
AsyncFsync::_fsync(int fd, bool full_fsync)
{
	L_CALL(nullptr, "AsyncFsync::_fsync(%d, %s)", fd, full_fsync ? "true" : "false");

	std::lock_guard<std::mutex> statuses_lk(AsyncFsync::statuses_mtx);
	AsyncFsync::Status& status = AsyncFsync::statuses[fd];

	auto now = std::chrono::system_clock::now();
	if (!status.mode) {
		status.mode = full_fsync ? 1 : 2;
		status.max_fsync_time = now + 3s;
	}
	status.fsync_time = now + 500ms;

	if (std::chrono::system_clock::from_time_t(AsyncFsync::next_wakeup_time.load()) > status.next_wakeup_time()) {
		AsyncFsync::wakeup_signal.notify_one();
	}

	return 0;
}
