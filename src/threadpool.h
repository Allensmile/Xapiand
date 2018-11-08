/*
 * Copyright (C) 2015-2018 Dubalu LLC. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

#include <chrono>         // for std::chrono
#include <cstddef>        // for std::size_t
#include <functional>     // for std::function
#include <future>         // for std::packaged_task
#include <tuple>          // for std::make_tuple, std::apply
#include <vector>         // for std::vector

#include "cassert.hh"     // for assert

#include "blocking_concurrent_queue.h"
#include "exception.h"    // for BaseException
#include "likely.h"       // for likely, unlikely
#include "log.h"          // for L_EXC
#include "string.hh"      // for string::format
#include "thread.hh"      // for Thread, set_thread_name


/* Since std::packaged_task cannot be copied, and std::function requires it can,
 * we add a dummy copy constructor to std::packaged_task. (We need to make sure
 * we actually really never copy the object though!)
 * [https://stackoverflow.com/q/39996132/167522]
 */
template <typename Result>
class PackagedTask : public std::packaged_task<Result> {
  public:
	PackagedTask() noexcept
	  : std::packaged_task<Result>() {}

	template <typename F>
	explicit PackagedTask(F&& f)
	  : std::packaged_task<Result>(std::forward<F>(f)) {}

	PackagedTask(const PackagedTask& /*unused*/) noexcept {
		// Adding this borks the compile
		assert(false);  // but should never be called!
	}

	PackagedTask(PackagedTask&& other) noexcept = default;

	PackagedTask& operator=(PackagedTask&& other) noexcept = default;
};


class ThreadPool;
class ThreadPoolThread : public Thread {
	ThreadPool* _pool;
	std::size_t _idx;

public:
	ThreadPoolThread() noexcept;

	ThreadPoolThread(std::size_t idx, ThreadPool* pool) noexcept;

	void operator()() override;
};


inline ThreadPoolThread::ThreadPoolThread() noexcept :
	_pool(nullptr),
	_idx(0)
{}

inline ThreadPoolThread::ThreadPoolThread(std::size_t idx, ThreadPool* pool) noexcept :
	_pool(pool),
	_idx(idx)
{}


class ThreadPool {
	friend ThreadPoolThread;

	std::vector<ThreadPoolThread> _threads;
	BlockingConcurrentQueue<std::function<void()>> _queue;

	const char* _format;

	std::atomic_bool _ending;
	std::atomic_bool _finished;
	std::atomic_size_t _enqueued;
	std::atomic_size_t _running;

public:
	ThreadPool(const char* format, std::size_t num_threads, std::size_t queue_size = 1000);

	~ThreadPool();

	void clear();
	std::size_t size();
	std::size_t running_size();
	std::size_t threadpool_capacity();
	std::size_t threadpool_size();

	bool join(const std::chrono::time_point<std::chrono::system_clock>& wakeup);

	template <typename T, typename R>
	bool join(std::chrono::duration<T, R> timeout) {
		return join(std::chrono::system_clock::now() + timeout);
	}

	bool join(int timeout = 60000) {
		return join(std::chrono::milliseconds(timeout));
	}

	// Flag the pool as ending, so all threads exit as soon as all queued tasks end
	void end();

	// Tell the tasks to finish so all threads exit as soon as possible
	void finish();

	template <typename Func, typename... Args>
	auto package(Func&& func, Args&&... args);

	template <typename It>
	auto enqueue_bulk(It itemFirst, std::size_t count);

	template <typename Func>
	auto enqueue(Func&& func);

	template <typename Func, typename... Args>
	auto async(Func&& func, Args&&... args);

	auto finished();
};


inline
ThreadPool::ThreadPool(const char* format, std::size_t num_threads, std::size_t queue_size)
	: _threads(num_threads),
	  _queue(queue_size),
	  _format(format),
	  _ending(false),
	  _finished(false),
	  _enqueued(0),
	  _running(0)
{
	for (std::size_t idx = 0; idx < num_threads; ++idx) {
		_threads[idx] = ThreadPoolThread(idx, this);
		_threads[idx].start();
	}
}

inline void
ThreadPoolThread::operator()()
{
	set_thread_name(string::format(_pool->_format, _idx));

	while (!_pool->_finished.load(std::memory_order_acquire)) {
		std::function<void()> task;
		_pool->_queue.wait_dequeue(task);
		if likely(task != nullptr) {
			_pool->_running.fetch_add(1, std::memory_order_relaxed);
			_pool->_enqueued.fetch_sub(1, std::memory_order_release);
			try {
				task();
			} catch (const BaseException& exc) {
				L_EXC("Task died with an unhandled exception: %s", *exc.get_context() ? exc.get_context() : "Unkown BaseException!");
			} catch (const Xapian::Error& exc) {
				L_EXC("Task died with an unhandled exception: %s", exc.get_description());
			} catch (const std::exception& exc) {
				L_EXC("Task died with an unhandled exception: %s", *exc.what() != 0 ? exc.what() : "Unkown std::exception!");
			} catch (...) {
				std::exception exc;
				L_EXC("Task died with an unhandled exception: Unkown exception!");
			}
			_pool->_running.fetch_sub(1, std::memory_order_release);
		} else if (_pool->_ending.load(std::memory_order_acquire)) {
			break;
		}
	}
}


inline
ThreadPool::~ThreadPool()
{
	finish();
	join();
}

inline void
ThreadPool::clear() {
	std::function<void()> task;
	while (_queue.try_dequeue(task)) {
		if likely(task != nullptr) {
			_enqueued.fetch_sub(1, std::memory_order_relaxed);
		}
	}
}

// Return size of the tasks queue
inline std::size_t
ThreadPool::size()
{
	return _enqueued.load(std::memory_order_relaxed);
}

inline std::size_t
ThreadPool::running_size()
{
	return _running.load(std::memory_order_relaxed);
}

inline std::size_t
ThreadPool::threadpool_capacity()
{
	return _threads.capacity();
}

inline std::size_t
ThreadPool::threadpool_size()
{
	return _threads.size();
}

inline void
ThreadPool::end()
{
	if (!_ending.exchange(true, std::memory_order_release)) {
		for (std::size_t idx = 0; idx < _threads.size(); ++idx) {
			_queue.enqueue(nullptr);
		}
	}
}

inline void
ThreadPool::finish()
{
	if (!_finished.exchange(true, std::memory_order_release)) {
		for (std::size_t idx = 0; idx < _threads.size(); ++idx) {
			_queue.enqueue(nullptr);
		}
	}
}

inline bool
ThreadPool::join(const std::chrono::time_point<std::chrono::system_clock>& wakeup)
{
	for (auto& _thread : _threads) {
		if (!_thread.join(wakeup)) {
			return false;
		}
	}
	return true;
}

template <typename Func, typename... Args>
inline auto
ThreadPool::package(Func&& func, Args&&... args)
{
	auto packaged_task = PackagedTask<std::result_of_t<Func(Args...)>()>([
		func = std::forward<Func>(func),
		args = std::make_tuple(std::forward<Args>(args)...)
	]() mutable {
		return std::apply(std::move(func), std::move(args));
	});
	return packaged_task;
}

template <typename It>
inline auto
ThreadPool::enqueue_bulk(It itemFirst, std::size_t count)
{
	_enqueued.fetch_add(count, std::memory_order_release);
	if unlikely(!_queue.enqueue_bulk(std::forward<It>(itemFirst), count)) {
		_enqueued.fetch_sub(count, std::memory_order_release);
		return false;
	}
	return true;
}

template <typename Func>
inline auto
ThreadPool::enqueue(Func&& func)
{
	_enqueued.fetch_add(1, std::memory_order_release);
	if unlikely(!_queue.enqueue(std::forward<Func>(func))) {
		_enqueued.fetch_sub(1, std::memory_order_release);
		return false;
	}
	return true;
}

template <typename Func, typename... Args>
inline auto
ThreadPool::async(Func&& func, Args&&... args)
{
	auto packaged_task = package(std::forward<Func>(func), std::forward<Args>(args)...);
	auto future = packaged_task.get_future();
	if unlikely(!enqueue([packaged_task = std::move(packaged_task)]() mutable {
		packaged_task();
	})) {
		throw std::runtime_error("Cannot enqueue task to threadpool");
	}
	return future;
}

inline auto ThreadPool::finished()
{
	return _finished.load(std::memory_order_relaxed);
}


////////////////////////////////////////////////////////////////////////////////

template <typename>
class TaskQueue;

template <typename R, typename... Args>
class TaskQueue<R(Args...)> {
	using Queue = ConcurrentQueue<std::packaged_task<R(Args...)>>;
	Queue _queue;

public:
	template <typename Func>
	auto enqueue(Func&& func) {
		auto packaged_task = std::packaged_task<R(Args...)>(std::forward<Func>(func));
		auto future = packaged_task.get_future();
		_queue.enqueue(std::move(packaged_task));
		return future;
	}

	bool call(Args&&... args) {
		std::packaged_task<R(Args...)> task;
		if (_queue.try_dequeue(task)) {
			task(std::forward<Args>(args)...);
			return true;
		}
		return false;
	}

	std::size_t clear() {
		std::array<std::packaged_task<R(Args...)>, Queue::BLOCK_SIZE> tasks;
		std::size_t cleared = 0;
		while (auto dequeued = _queue.try_dequeue_bulk(tasks.begin(), tasks.size())) {
			cleared += dequeued;
		}
		return cleared;
	}
};
