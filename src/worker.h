/*
 * Copyright (C) 2015,2016,2017 deipi.com LLC and contributors. All rights reserved.
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

#include <atomic>	 // for std::atomic_bool
#include <list>      // for list
#include <memory>    // for shared_ptr, enable_shared_from_this
#include <mutex>     // for mutex
#include <string>	 // for string
#include <vector>    // for vector

#include "ev/ev++.h"


class Worker : public std::enable_shared_from_this<Worker> {
	using WorkerShared = std::shared_ptr<Worker>;
	using WorkerList = std::list<WorkerShared>;

	ev::dynamic_loop _dynamic_ev_loop;

protected:
	unsigned int ev_flags;
	ev::loop_ref *ev_loop;

private:
	time_t _asap;
	time_t _now;

	ev::async _shutdown_async;
	ev::async _break_loop_async;
	ev::async _destroy_async;
	ev::async _detach_children_async;

	std::mutex _mtx;
	std::atomic_bool _runner;
	std::atomic_bool _detaching;

	const WorkerShared _parent;
	WorkerList _children;

	// _iterator should be const_iterator but in linux, std::list member functions
	// use a standard iterator and not const_iterator.
	WorkerList::iterator _iterator;

protected:
	template<typename T, typename L>
	Worker(T&& parent, L&& ev_loop_, unsigned int ev_flags_)
		: _dynamic_ev_loop(ev_flags_),
		  ev_flags(ev_flags_),
		  ev_loop(ev_loop_ ? std::forward<L>(ev_loop_) : &_dynamic_ev_loop),
		  _shutdown_async(*ev_loop),
		  _break_loop_async(*ev_loop),
		  _destroy_async(*ev_loop),
		  _detach_children_async(*ev_loop),
		  _runner(false),
		  _detaching(false),
		  _parent(std::forward<T>(parent))
	{
		_init();
	}

	void destroyer();

private:
	template<typename T>
	auto __attach(T&& child) {
		ASSERT(child);
		auto it = _children.insert(_children.end(), std::forward<T>(child));
		(*it)->_iterator = it;
		return it;
	}

	template<typename T>
	auto __detach(T&& child) {
		ASSERT(child);
		if (child->_iterator != _children.end()) {
			ASSERT(child->_parent.get() == this);
			auto it = _children.erase(child->_iterator);
			child->_iterator = _children.end();
			return it;
		}
		return _children.end();
	}

	void _init();

	void _shutdown_async_cb();
	void _break_loop_async_cb(ev::async&, int revents);
	void _destroy_async_cb(ev::async&, int revents);
	void _detach_children_async_cb(ev::async&, int revents);
	std::vector<std::weak_ptr<Worker>> _gather_children();
	void _detach_impl(const std::weak_ptr<Worker>& weak_child);
	auto _ancestor(int levels=-1);
	void _detach_children();

public:
	std::string dump_tree(int level=1);

	std::string __repr__(const std::string& name) const;

	virtual std::string __repr__() const {
		return __repr__("Worker");
	}

	virtual ~Worker();

	virtual void shutdown_impl(time_t asap, time_t now);
	virtual void destroy_impl() = 0;

	void break_loop_impl();
	void detach_children_impl();

	void shutdown(time_t asap, time_t now);
	void shutdown();

	void break_loop();

	void destroy();
	void detach();

	void run_loop();

	template<typename T, typename... Args, typename = std::enable_if_t<std::is_base_of<Worker, std::decay_t<T>>::value>>
	static auto make_shared(Args&&... args) {
		/*
		 * std::make_shared only can call a public constructor, for this reason
		 * it is neccesary wrap the constructor in a struct.
		 */
		struct enable_make_shared : T {
			enable_make_shared(Args&&... args) : T(std::forward<Args>(args)...) { }
		};
		auto child = std::make_shared<enable_make_shared>(std::forward<Args>(args)...);
		if (child->_parent) {
			std::lock_guard<std::mutex> lk(child->_parent->_mtx);
			child->_parent->__attach(child);
		}

		return child;
	}

	template<typename T, typename = std::enable_if_t<std::is_base_of<Worker, std::decay_t<T>>::value>>
	auto share_parent() noexcept {
		return std::static_pointer_cast<T>(_parent);
	}

	template<typename T, typename = std::enable_if_t<std::is_base_of<Worker, std::decay_t<T>>::value>>
	auto share_this() noexcept {
		return std::static_pointer_cast<T>(shared_from_this());
	}
};
