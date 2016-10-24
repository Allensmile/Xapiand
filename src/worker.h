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

#pragma once

#include "log.h"
#include "utils.h"

#include "ev/ev++.h"
#include "exception.h"

#include <list>
#include <mutex>
#include <memory>
#include <cassert>


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


	ev::async _async_shutdown;
	ev::async _async_break_loop;
	ev::async _async_destroy;
	ev::async _async_detach;

	std::mutex _mtx;
	std::atomic_bool _detaching;
	std::atomic_bool _running;

	const WorkerShared _parent;
	WorkerList _children;

	// _iterator should be const_iterator but in linux, std::list member functions
	// use a standard iterator and not const_iterator.
	WorkerList::iterator _iterator;

	template<typename T>
	inline auto _attach(T&& child) {
		assert(child);
		auto it = _children.insert(_children.end(), std::forward<T>(child));
		child->_iterator = it;

		auto parent = shared_from_this();
		while (parent) {
			if (parent->ev_loop == child->ev_loop) {
				child->_running = parent->_running.load();
				break;
			}
			parent = parent->_parent;
		}

		return it;
	}

	template<typename T>
	inline decltype(auto) _detach(T&& child) {
		if (child->_parent && child->_iterator != _children.end()) {
			auto it = _children.erase(child->_iterator);
			child->_iterator = _children.end();
			return it;
		}
		return _children.end();
	}

	inline void _set_running(ev::loop_ref* loop, bool running) {
		std::lock_guard<std::mutex> lk(_mtx);
		if (ev_loop == loop) {
			_running = running;
		}
		for (const auto& c : _children) {
			c->_set_running(loop, running);
		}
	}

protected:
	template<typename T, typename L>
	Worker(T&& parent, L&& ev_loop_, unsigned int ev_flags_)
		: _dynamic_ev_loop(ev_flags_),
		  ev_flags(ev_flags_),
		  ev_loop(ev_loop_ ? std::forward<L>(ev_loop_) : &_dynamic_ev_loop),
		  _async_shutdown(*ev_loop),
		  _async_break_loop(*ev_loop),
		  _async_destroy(*ev_loop),
		  _async_detach(*ev_loop),
		  _parent(std::forward<T>(parent))
	{
		if (_parent) {
			_iterator = _parent->_children.end();
		}

		_async_shutdown.set<Worker, &Worker::_async_shutdown_cb>(this);
		_async_shutdown.start();
		L_EV(this, "Start Worker async shutdown event");

		_async_break_loop.set<Worker, &Worker::_async_break_loop_cb>(this);
		_async_break_loop.start();
		L_EV(this, "Start Worker async break_loop event");

		_async_destroy.set<Worker, &Worker::_async_destroy_cb>(this);
		_async_destroy.start();
		L_EV(this, "Start Worker async destroy event");

		_async_detach.set<Worker, &Worker::_async_detach_cb>(this);
		_async_detach.start();
		L_EV(this, "Start Worker async detach event");

		L_OBJ(this, "CREATED WORKER!");
	}

	void destroyer() {
		L_OBJ(this, "DESTROYING WORKER!");

		_async_shutdown.stop();
		L_EV(this, "Stop Worker async shutdown event");
		_async_break_loop.stop();
		L_EV(this, "Stop Worker async break_loop event");
		_async_destroy.stop();
		L_EV(this, "Stop Worker async destroy event");
		_async_detach.stop();
		L_EV(this, "Stop Worker async detach event");

		L_OBJ(this, "DESTROYED WORKER!");
	}

private:
	void _async_shutdown_cb() {
		L_CALL(this, "Worker::_async_shutdown_cb()");

		L_EV_BEGIN(this, "Worker::_async_shutdown_cb:BEGIN");
		shutdown_impl(_asap, _now);
		L_EV_END(this, "Worker::_async_shutdown_cb:END");
	}

	void _async_break_loop_cb(ev::async&, int revents) {
		L_CALL(this, "Worker::_async_break_loop_cb(<watcher>, 0x%02x (%s))", revents, readable_revents(revents).c_str()); (void)revents;

		L_EV_BEGIN(this, "Worker::_async_break_loop_cb:BEGIN");
		break_loop_impl();
		L_EV_END(this, "Worker::_async_break_loop_cb:END");
	}

	void _async_destroy_cb(ev::async&, int revents) {
		L_CALL(this, "Worker::_async_destroy_cb(<watcher>, 0x%02x (%s))", revents, readable_revents(revents).c_str()); (void)revents;

		L_EV_BEGIN(this, "Worker::_async_destroy_cb:BEGIN");
		destroy_impl();
		L_EV_END(this, "Worker::_async_destroy_cb:END");
	}

	void _async_detach_cb(ev::async&, int revents) {
		L_CALL(this, "Worker::_async_detach_cb(<watcher>, 0x%02x (%s))", revents, readable_revents(revents).c_str()); (void)revents;

		L_EV_BEGIN(this, "Worker::_async_detach_cb:BEGIN");
		detach_impl();
		L_EV_END(this, "Worker::_async_detach_cb:END");
	}

	std::vector<std::weak_ptr<Worker>> _gather_children() {
		std::vector<std::weak_ptr<Worker>> weak_children;
		// Collect active children
		std::lock_guard<std::mutex> lk(_mtx);
		weak_children.reserve(_children.size());
		for (auto it = _children.begin(); it != _children.end();) {
			auto child = *it;
			if (child) {
				weak_children.push_back(child);
				++it;
			} else {
				it = _children.erase(it);
			}
		}
		return weak_children;
	}

	void _detach_impl(const std::weak_ptr<Worker>& weak_child) {
		std::lock_guard<std::mutex> lk(_mtx);
		std::string repr;
		if (auto child = weak_child.lock()) {
			_detach(child);
			repr = child->__repr__();
		} else {
			return;
		}
		if (auto child = weak_child.lock()) {
			L_OBJ(this, "Worker child %s cannot be detached from %s (cnt: %u)", repr.c_str(), __repr__().c_str(), child.use_count() - 1);
			_attach(child);
		} else {
			L_OBJ(this, "Worker child %s detached from %s", repr.c_str(), __repr__().c_str());
		}
	}

	auto _ancestor(int levels=-1) {
		auto ancestor = shared_from_this();
		while (ancestor->_parent && levels-- != 0) {
			ancestor = ancestor->_parent;
		}
		return ancestor;
	}

public:
	std::string dump_tree(int level=1) {
		std::lock_guard<std::mutex> lk(_mtx);
		std::string ret;
		for (int l = 0; l < level; ++l) ret += "    ";
		ret += __repr__() + " (cnt: " + std::to_string(shared_from_this().use_count() - 1) + (_running ? ") in a running loop\n" : ")\n");
		for (const auto& c : _children) {
			ret += c->dump_tree(level + 1);
		}
		return ret;
	}

	virtual std::string __repr__() const = 0;

	virtual ~Worker() {
		destroyer();

		L_OBJ(this, "DELETED WORKER!");
	}

	virtual void shutdown_impl(time_t asap, time_t now) {
		auto weak_children = _gather_children();
		L_OBJ(this , "SHUTDOWN WORKER! (%d %d): %zu children", asap, now, weak_children.size());
		for (auto& weak_child : weak_children) {
			if (auto child = weak_child.lock()) {
				child->shutdown_impl(asap, now);
			}
		}
	}

	virtual void destroy_impl() = 0;

	void break_loop_impl() {
		ev_loop->break_loop();
	}

	void detach_impl() {
		auto weak_children = _gather_children();
		L_OBJ(this , "CLEANUP WORKER: %zu children", weak_children.size());
		for (auto& weak_child : weak_children) {
			if (auto child = weak_child.lock()) {
				child->detach_impl();
				if (!child->_detaching) continue;
			}
			_detach_impl(weak_child);
		}
	}

	inline void shutdown(time_t asap, time_t now) {
		if (_running) {
			_asap = asap;
			_now = now;
			_async_shutdown.send();
		} else {
			shutdown_impl(asap, now);
		}
	}

	inline void shutdown() {
		auto now = epoch::now<>();
		if (_running) {
			_asap = now;
			_now = now;
			_async_shutdown.send();
		} else {
			shutdown_impl(now, now);
		}
	}

	inline void break_loop() {
		if (_running) {
			_async_break_loop.send();
		} else {
			break_loop_impl();
		}
	}

	inline void destroy() {
		if (_running) {
			_async_destroy.send();
		} else {
			destroy_impl();
		}
	}

	inline void detach() {
		_detaching = true;
		cleanup();
	}

	inline void cleanup() {
		if (_running) {
			_ancestor(1)->_async_detach.send();
		} else {
			_ancestor(1)->detach_impl();
		}
	}

	void set_running(bool running) {
		_ancestor()->_set_running(ev_loop, running);
	}

	void run_loop() {
		set_running(true);
		ev_loop->run();
		set_running(false);
	}

	template<typename T, typename... Args, typename = std::enable_if_t<std::is_base_of<Worker, std::decay_t<T>>::value>>
	static inline decltype(auto) make_shared(Args&&... args) {
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
			child->_parent->_attach(child);
			L_OBJ(child.get(), "child child %s attached to %s", child->__repr__().c_str(), child->_parent->__repr__().c_str());
		}

		return child;
	}

	template<typename T, typename = std::enable_if_t<std::is_base_of<Worker, std::decay_t<T>>::value>>
	inline decltype(auto) share_parent() noexcept {
		return std::static_pointer_cast<T>(_parent);
	}

	template<typename T, typename = std::enable_if_t<std::is_base_of<Worker, std::decay_t<T>>::value>>
	inline decltype(auto) share_this() noexcept {
		return std::static_pointer_cast<T>(shared_from_this());
	}
};
