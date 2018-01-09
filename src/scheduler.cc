/*
 * Copyright (C) 2015-2018 deipi.com LLC and contributors. All rights reserved.
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

#include "log.h"         // for L_*
#include "utils.h"       // for time_point_to_ullong, format_string


// #define L_SCHEDULER L_STDOUT
#ifndef L_SCHEDULER
#define L_SCHEDULER_DEFINED
#define L_SCHEDULER L_NOTHING
#endif


ScheduledTask::ScheduledTask(std::chrono::time_point<std::chrono::system_clock> created_at_)
	: wakeup_time(0),
	  created_at(time_point_to_ullong(created_at_)),
	  cleared_at(0) { }


bool
ScheduledTask::clear()
{
	unsigned long long c = 0;
	return cleared_at.compare_exchange_strong(c, time_point_to_ullong(std::chrono::system_clock::now()));
}


std::string
ScheduledTask::__repr__(const std::string& name) const
{
	return format_string("<%s at %p>",
		name.c_str(),
		this
	);
}


SchedulerQueue::SchedulerQueue()
	: ctx(now()),
	  cctx(now()) { }


TaskType
SchedulerQueue::peep(unsigned long long current_key)
{
	ctx.op = StashContext::Operation::peep;
	ctx.cur_key = ctx.atom_first_key.load();
	ctx.current_key = current_key;
	TaskType task;
	queue.next(ctx, &task);
	return task;
}


TaskType
SchedulerQueue::walk()
{
	ctx.op = StashContext::Operation::walk;
	ctx.cur_key = ctx.atom_first_key.load();
	ctx.current_key = time_point_to_ullong(std::chrono::system_clock::now());
	TaskType task;
	queue.next(ctx, &task);
	return task;
}


void
SchedulerQueue::clean_checkpoint()
{
	auto cur_key = ctx.atom_first_key.load();
	if (cur_key < cctx.atom_first_key.load()) {
		cctx.atom_first_key = cur_key;
	}
	cctx.atom_last_key = ctx.atom_last_key.load();
}


void
SchedulerQueue::clean()
{
	cctx.op = StashContext::Operation::clean;
	cctx.cur_key = cctx.atom_first_key.load();
	cctx.current_key = time_point_to_ullong(std::chrono::system_clock::now() - 1s);
	TaskType task;
	queue.next(cctx, &task);
}


void
SchedulerQueue::add(const TaskType& task, unsigned long long key)
{
	try {
		queue.add(ctx, key, task);
	} catch (const std::out_of_range&) {
		fprintf(stderr, "%sStash overflow!%s\n", RED.c_str(), NO_COL.c_str());
	}
}


Scheduler::Scheduler(const std::string& name_)
	: atom_next_wakeup_time(0),
	  name(name_),
	  running(-1),
	  inner_thread(&Scheduler::run, this) { }


Scheduler::Scheduler(const std::string& name_, const std::string& format, size_t num_threads)
	: thread_pool(std::make_unique<ThreadPool<>>(format, num_threads)),
	  atom_next_wakeup_time(0),
	  name(name_),
	  running(-1),
	  inner_thread(&Scheduler::run, this) { }


Scheduler::~Scheduler()
{
	finish(1);
}


size_t
Scheduler::threadpool_capacity()
{
	if (thread_pool) {
		return thread_pool->threadpool_capacity();
	}
	return 0;
}


size_t
Scheduler::threadpool_size()
{
	if (thread_pool) {
		return thread_pool->threadpool_size();
	}
	return 0;
}


size_t
Scheduler::running_size()
{
	if (thread_pool) {
		return thread_pool->running_size();
	}
	return 0;
}


size_t
Scheduler::size()
{
	if (thread_pool) {
		return thread_pool->size();
	}
	return 0;
}


void
Scheduler::finish(int wait)
{
	running = wait;

	{
		std::lock_guard<std::mutex> lk(mtx);
		wakeup_signal.notify_all();
	}

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
Scheduler::add(const TaskType& task, unsigned long long wakeup_time)
{
	if (running != 0) {
		auto now = time_point_to_ullong(std::chrono::system_clock::now());
		if (wakeup_time < now) {
			wakeup_time = now;
		}

		task->wakeup_time = wakeup_time;
		scheduler_queue.add(task, wakeup_time);

		auto next_wakeup_time = atom_next_wakeup_time.load();
		while (next_wakeup_time > wakeup_time && !atom_next_wakeup_time.compare_exchange_weak(next_wakeup_time, wakeup_time));

		if (next_wakeup_time >= wakeup_time || next_wakeup_time <= now) {
			{
				std::lock_guard<std::mutex> lk(mtx);
				wakeup_signal.notify_one();
			}
			L_SCHEDULER("Scheduler::" + LIGHT_GREEN + "ADDED_NOTIFY" + NO_COL + " - now:%llu, next_wakeup_time:%llu, wakeup_time:%llu - %s", now, atom_next_wakeup_time.load(), wakeup_time, task ? task->__repr__().c_str() : "");
		} else {
			L_SCHEDULER("Scheduler::" + GREEN + "ADDED" + NO_COL + " - now:%llu, next_wakeup_time:%llu, wakeup_time:%llu - %s", now, atom_next_wakeup_time.load(), wakeup_time, task ? task->__repr__().c_str() : "");
		}
	}
}


void
Scheduler::add(const TaskType& task, std::chrono::time_point<std::chrono::system_clock> wakeup)
{
	add(task, time_point_to_ullong(wakeup));
}


void
Scheduler::run_one(TaskType& task)
{
	if (*task) {
		if (task->clear()) {
			L_SCHEDULER("Scheduler::" + CYAN + "RUNNING" + NO_COL + " - now:%llu, wakeup_time:%llu", time_point_to_ullong(std::chrono::system_clock::now()), task->wakeup_time);
			if (thread_pool) {
				try {
					thread_pool->enqueue(task);
				} catch (const std::logic_error&) { }
			} else {
				task->run();
			}
			return;
		}
	}
	L_SCHEDULER("Scheduler::" + RED + "ABORTED" + NO_COL + " - now:%llu, wakeup_time:%llu", time_point_to_ullong(std::chrono::system_clock::now()), task->wakeup_time);
}


void
Scheduler::run()
{
	L_SCHEDULER("Scheduler::" + LIGHT_CYAN + "STARTED" + NO_COL);

	set_thread_name(name);

	std::unique_lock<std::mutex> lk(mtx);
	auto next_wakeup_time = atom_next_wakeup_time.load();
	lk.unlock();

	while (running != 0) {
		if (--running < 0) {
			running = -1;
		}

		bool pending = false;

		// Propose a wakeup time some time in the future:
		auto now = std::chrono::system_clock::now();
		auto wakeup_time = time_point_to_ullong(now + (running < 0 ? 30s : 100ms));

		// Then figure out if there's something that needs to be acted upon sooner
		// than that wakeup time in the scheduler queue (an earlier wakeup time needed):
		TaskType task;
		L_SCHEDULER("Scheduler::" + DARK_GREY + "PEEPING" + NO_COL + " - now:%llu, wakeup_time:%llu", time_point_to_ullong(now), wakeup_time);
		if ((task = scheduler_queue.peep(wakeup_time))) {
			if (task) {
				pending = true;  // flag there are still scheduled things pending.
				if (wakeup_time > task->wakeup_time) {
					wakeup_time = task->wakeup_time;
					L_SCHEDULER("Scheduler::" + MAGENTA + "PEEP_UPDATED" + NO_COL + " - now:%llu, wakeup_time:%llu  (%s)", time_point_to_ullong(now), wakeup_time, *task ? "valid" : "cleared");
				} else {
					L_SCHEDULER("Scheduler::" + DARK_GREY + "PEEPED" + NO_COL + " - now:%llu, wakeup_time:%llu  (%s)", time_point_to_ullong(now), wakeup_time, *task ? "valid" : "cleared");
				}
			}
		}

		// Try setting the worked out wakeup time as the real next wakeup time:
		if (atom_next_wakeup_time.compare_exchange_strong(next_wakeup_time, wakeup_time)) {
			if (running >= 0 && !pending) {
				break;
			}
		}
		while (next_wakeup_time > wakeup_time && !atom_next_wakeup_time.compare_exchange_weak(next_wakeup_time, wakeup_time)) { }

		// Sleep until wakeup time arrives or someone adding a task wakes us up;
		// make sure we first lock mutex so there cannot be race condition between
		// the time we load the next_wakeup_time and we actually start waiting:
		L_INFO_HOOK_LOG("Scheduler::LOOP", "Scheduler::" + BLUE + "LOOP" + NO_COL + " - now:%llu, next_wakeup_time:%llu", time_point_to_ullong(now), atom_next_wakeup_time.load());
		lk.lock();
		next_wakeup_time = atom_next_wakeup_time.load();
		auto next_wakeup_time_point = time_point_from_ullong(next_wakeup_time);
		if (next_wakeup_time_point > now) {
			wakeup_signal.wait_until(lk, next_wakeup_time_point);
		}
		lk.unlock();
		L_SCHEDULER("Scheduler::" + LIGHT_BLUE + "WAKEUP" + NO_COL + " - now:%llu, wakeup_time:%llu", time_point_to_ullong(std::chrono::system_clock::now()), wakeup_time);

		// Start walking the queue and running still pending tasks.
		scheduler_queue.clean_checkpoint();
		while ((task = scheduler_queue.walk())) {
			if (task) {
				run_one(task);
			}
		}
		scheduler_queue.clean();
	}
}


#ifdef L_SCHEDULER_DEFINED
#undef L_SCHEDULER_DEFINED
#undef L_SCHEDULER
#endif
