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

#include "worker.h"

#include "log.h"
#include "utils.h"

#undef L_CALL
#define L_CALL L_NOTHING

// #define L_WORKER L_DEBUG


Worker::~Worker()
{
	destroyer();

	L_OBJ(this, "DELETED WORKER!");
}


void
Worker::_init()
{
	L_CALL(this, "Worker::_init()");

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


void
Worker::destroyer()
{
	L_CALL(this, "Worker::destroyer()");

	_async_shutdown.stop();
	L_EV(this, "Stop Worker async shutdown event");
	_async_break_loop.stop();
	L_EV(this, "Stop Worker async break_loop event");
	_async_destroy.stop();
	L_EV(this, "Stop Worker async destroy event");
	_async_detach.stop();
	L_EV(this, "Stop Worker async detach event");
}


void
Worker::_async_shutdown_cb()
{
	L_CALL(this, "Worker::_async_shutdown_cb() [%s]", __repr__().c_str());

	L_EV_BEGIN(this, "Worker::_async_shutdown_cb:BEGIN");
	shutdown_impl(_asap, _now);
	L_EV_END(this, "Worker::_async_shutdown_cb:END");
}


void
Worker::_async_break_loop_cb(ev::async&, int revents)
{
	L_CALL(this, "Worker::_async_break_loop_cb(<watcher>, 0x%x (%s)) [%s]", revents, readable_revents(revents).c_str(), __repr__().c_str()); (void)revents;

	L_EV_BEGIN(this, "Worker::_async_break_loop_cb:BEGIN");
	break_loop_impl();
	L_EV_END(this, "Worker::_async_break_loop_cb:END");
}


void
Worker::_async_destroy_cb(ev::async&, int revents)
{
	L_CALL(this, "Worker::_async_destroy_cb(<watcher>, 0x%x (%s)) [%s]", revents, readable_revents(revents).c_str(), __repr__().c_str()); (void)revents;

	L_EV_BEGIN(this, "Worker::_async_destroy_cb:BEGIN");
	destroy_impl();
	L_EV_END(this, "Worker::_async_destroy_cb:END");
}


void
Worker::_async_detach_cb(ev::async&, int revents)
{
	L_CALL(this, "Worker::_async_detach_cb(<watcher>, 0x%x (%s)) [%s]", revents, readable_revents(revents).c_str(), __repr__().c_str()); (void)revents;

	L_EV_BEGIN(this, "Worker::_async_detach_cb:BEGIN");
	detach_impl();
	L_EV_END(this, "Worker::_async_detach_cb:END");
}


std::vector<std::weak_ptr<Worker>>
Worker::_gather_children()
{
	L_CALL(this, "Worker::_gather_children() [%s]", __repr__().c_str());

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


void
Worker::_detach_impl(const std::weak_ptr<Worker>& weak_child)
{
	L_CALL(this, "Worker::_detach_impl(<weak_child>) [%s]", __repr__().c_str());

#ifdef L_WORKER
	std::string child_repr;
	long child_use_count;
#endif

	std::lock_guard<std::mutex> lk(_mtx);
	if (auto child = weak_child.lock()) {
		_detach(child);
#ifdef L_WORKER
		child_repr = child->__repr__();
		child_use_count = child.use_count();
#endif
	} else {
		return;
	}
	if (auto child = weak_child.lock()) {
		_attach(child);
#ifdef L_WORKER
		L_WORKER(this, RED "Worker child %s (cnt: %ld) cannot be detached from %s (cnt: %ld)", child_repr.c_str(), child_use_count - 1, __repr__().c_str(), shared_from_this().use_count() - 1);
	} else {
		L_WORKER(this, GREEN "Worker child %s (cnt: %ld) detached from %s (cnt: %ld)", child_repr.c_str(), child_use_count - 1, __repr__().c_str(), shared_from_this().use_count() - 1);
#endif
	}
}


auto
Worker::_ancestor(int levels)
{
	L_CALL(this, "Worker::_ancestor(%d) [%s]", levels, __repr__().c_str());

	auto ancestor = shared_from_this();
	while (ancestor->_parent && levels-- != 0) {
		ancestor = ancestor->_parent;
	}
	return ancestor;
}


std::string
Worker::__repr__() const
{
	char buffer[100];
	snprintf(buffer, sizeof(buffer), "<Worker at %p>", this);
	return buffer;
}


std::string
Worker::dump_tree(int level)
{
	std::lock_guard<std::mutex> lk(_mtx);
	std::string ret = "\n";
	for (int l = 0; l < level; ++l) ret += "    ";
	ret += __repr__() + " (cnt: " + std::to_string(shared_from_this().use_count() - 1) + (ev_loop->depth() ? ") in a running loop" : ")");
	for (const auto& c : _children) {
		ret += c->dump_tree(level + 1);
	}
	return ret;
}


void
Worker::shutdown_impl(time_t asap, time_t now)
{
	L_CALL(this, "Worker::shutdown_impl(%d, %d) [%s]", (int)asap, (int)now, __repr__().c_str());

	auto weak_children = _gather_children();
	for (auto& weak_child : weak_children) {
		if (auto child = weak_child.lock()) {
			child->shutdown_impl(asap, now);
		}
	}
}


void
Worker::break_loop_impl()
{
	L_CALL(this, "Worker::break_loop_impl() [%s]", __repr__().c_str());

	ev_loop->break_loop();
}


void
Worker::detach_impl()
{
	L_CALL(this, "Worker::detach_impl() [%s]", __repr__().c_str());

	auto weak_children = _gather_children();
	for (auto& weak_child : weak_children) {
		if (auto child = weak_child.lock()) {
			child->detach_impl();
			if (!child->_detaching) {
				if (ev_loop->depth()) {
#ifdef L_WORKER
					L_WORKER(this, LIGHT_RED "Worker child (in a running loop) %s (cnt: %ld) cannot be detached from %s (cnt: %ld)", child->__repr__().c_str(), child.use_count() - 1, __repr__().c_str(), shared_from_this().use_count() - 1);
#endif
					continue;
				}
			}
		}
		_detach_impl(weak_child);
	}
}


void
Worker::shutdown(time_t asap, time_t now)
{
	L_CALL(this, "Worker::shutdown(%d, %d) [%s]", (int)asap, (int)now, __repr__().c_str());

	_asap = asap;
	_now = now;
	_async_shutdown.send();
	if (!ev_loop->depth()) {
		shutdown_impl(asap, now);
	}
}


void
Worker::shutdown()
{
	L_CALL(this, "Worker::shutdown() [%s]", __repr__().c_str());

	auto now = epoch::now<>();
	shutdown(now, now);
}


void
Worker::break_loop()
{
	L_CALL(this, "Worker::break_loop() [%s]", __repr__().c_str());

	_async_break_loop.send();
	if (!ev_loop->depth()) {
		break_loop_impl();
	}
}


void
Worker::destroy()
{
	L_CALL(this, "Worker::destroy() [%s]", __repr__().c_str());

	_async_destroy.send();
	if (!ev_loop->depth()) {
		destroy_impl();
	}
}


void
Worker::detach()
{
	L_CALL(this, "Worker::detach() [%s]", __repr__().c_str());

	_detaching = true;
	cleanup();
}


void
Worker::cleanup()
{
	L_CALL(this, "Worker::cleanup() [%s]", __repr__().c_str());

	_ancestor(1)->_async_detach.send();
	if (!ev_loop->depth()) {
		_ancestor(1)->detach_impl();
	}
}


void
Worker::run_loop()
{
	L_CALL(this, "Worker::run_loop() [%s]", __repr__().c_str());

	ev_loop->run();
	if (_detaching) {
		detach();
	}
}
