/*
 * Copyright (C) 2016 deipi.com LLC and contributors. All rights reserved.
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

#include <memory>

#include "scheduler.h"

#include "utils.h"       // for time_point_to_ullong


ScheduledTask::ScheduledTask(std::chrono::time_point<std::chrono::system_clock> created_at_)
	: wakeup(time_point_from_ullong(0)),
	  created_at(time_point_to_ullong(created_at_)),
	  cleared_at(0) { }


ScheduledTask::~ScheduledTask() { }


bool
ScheduledTask::clear()
{
	unsigned long long c = 0;
	return cleared_at.compare_exchange_strong(c, time_point_to_ullong(std::chrono::system_clock::now()));
}


SchedulerQueue::SchedulerQueue() { }


TaskType*
SchedulerQueue::next(unsigned long long final_key, bool keep_going)
{
	TaskType* task = nullptr;
	queue.next(&task, final_key, keep_going, false);
	return task;
}


TaskType*
SchedulerQueue::peep()
{
	TaskType* task = nullptr;
	queue.next(&task, 0, true, true);
	return task;
}


unsigned long long
SchedulerQueue::add(const TaskType& task, unsigned long long key)
{
	return queue.add(task, key);
}


Scheduler::Scheduler(const std::string& name_)
	: name(name_),
	  running(-1),
	  inner_thread(&Scheduler::run, this) { }


Scheduler::Scheduler(const std::string& name_, const std::string format, size_t num_threads)
	: thread_pool(std::make_unique<ThreadPool<>>(format, num_threads)),
	  name(name_),
	  running(-1),
	  inner_thread(&Scheduler::run, this) { }


Scheduler::~Scheduler()
{
	finish(1);
}


size_t
Scheduler::running_size()
{
	if (thread_pool) {
		thread_pool->running_size();
	}
	return 0;
}


void
Scheduler::finish(int wait)
{
	running = wait;
	wakeup_signal.notify_all();

	if (thread_pool) {
		thread_pool->finish();
	}

	if (wait) {
		join();
	}
}


void
Scheduler::join()
{
	try {
		if (inner_thread.joinable()) {
			inner_thread.join();
		}
	} catch (const std::system_error&) { }

	if (thread_pool) {
		thread_pool->join();
		thread_pool.reset();
	}
}


void
Scheduler::add(const TaskType& task, std::chrono::time_point<std::chrono::system_clock> wakeup)
{
	if (running != 0) {
		task->wakeup = time_point_from_ullong(scheduler_queue.add(task, time_point_to_ullong(wakeup)));

		bool notify = false;
		auto now = std::chrono::system_clock::now();
		wakeup = task->wakeup;
		{
			std::lock_guard<std::mutex> lk(mtx);
			if (next_wakeup > wakeup) {
				next_wakeup = wakeup;
				notify = true;
			} else if (next_wakeup < now) {
				notify = true;
			}
		}

		if (notify) {
			// L_INFO_HOOK_LOG("Scheduler::NOTIFY", this, "Scheduler::" BLUE "NOTIFY" NO_COL " - now:%llu, next_wakeup:%llu, wakeup:%llu", time_point_to_ullong(now), time_point_to_ullong(next_wakeup), task->wakeup);
			wakeup_signal.notify_one();
		}
	}
}


void
Scheduler::run_one(TaskType& task)
{
	if (!task->cleared_at) {
		if (task->clear()) {
			if (thread_pool) {
				try {
					thread_pool->enqueue(task);
				} catch (const std::logic_error&) { }
			} else {
				task->run();
			}
		}
	}
}


void
Scheduler::run()
{
	set_thread_name(name);

	std::unique_lock<std::mutex> lk(mtx);
	lk.unlock();

	while (running != 0) {
		if (--running < 0) {
			running = -1;
		}

		auto now = std::chrono::system_clock::now();
		auto wakeup = now + (running < 0 ? 5s : 100ms);

		TaskType* task;

		if ((task = scheduler_queue.peep()) && *task) {
			wakeup = (*task)->wakeup;
			// L_INFO_HOOK_LOG("Scheduler::PEEP", this, "Scheduler::" MAGENTA "PEEP" NO_COL " - now:%llu, wakeup:%llu", time_point_to_ullong(now), time_point_to_ullong(wakeup));
		}

		lk.lock();
		if (next_wakeup > wakeup) {
			next_wakeup = wakeup;
		}
		L_INFO_HOOK_LOG("Scheduler::LOOP", this, "Scheduler::" CYAN "LOOP" NO_COL " - now:%llu, next_wakeup:%llu", time_point_to_ullong(now), time_point_to_ullong(next_wakeup));
		if (next_wakeup > now) {
			if (wakeup_signal.wait_until(lk, next_wakeup) == std::cv_status::no_timeout) {
				lk.unlock();
			}
		} else {
			lk.unlock();
		}

		// L_INFO_HOOK_LOG("Scheduler::WAKEUP", this, "Scheduler::" GREEN "WAKEUP" NO_COL " - now:%llu, wakeup:%llu", time_point_to_ullong(std::chrono::system_clock::now()), time_point_to_ullong(wakeup));

		while ((task = scheduler_queue.next())) {
			if (*task) {
				run_one(*task);
				(*task).reset();
			}
		}

		if (running >= 0) {
			break;
		}
	}
}
