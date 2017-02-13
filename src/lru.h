/*
 * Copyright (C) 2015,2016,2017 deipi.com LLC and contributors. All rights reserved.
 * Copyright (C) 2014, lamerman. All rights reserved.
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

#include <cstddef>
#include <list>
#include <stdexcept>
#include <unordered_map>


namespace lru {


enum class DropAction : uint8_t {
	drop,
	leave,
	renew,
};


enum class GetAction : uint8_t {
	leave,
	renew,
};


enum class InsertAction : uint8_t {
	front,
	last,
};


template<typename Key, typename T>
class LRU {
protected:
	using list_t = std::list<std::pair<const Key, T>>;
	using map_t = std::unordered_map<Key, typename list_t::iterator>;

	list_t _items_list;
	map_t _items_map;
	ssize_t _max_size;

public:
	using iterator = typename list_t::iterator;
	using const_iterator = typename list_t::const_iterator;

	LRU(ssize_t max_size=-1)
		: _max_size(max_size) { }

	virtual ~LRU() = default;

	auto begin() noexcept {
		return _items_list.begin();
	}

	auto cbegin() const noexcept {
		return _items_list.cbegin();
	}

	auto end() noexcept {
		return _items_list.end();
	}

	auto cend() const noexcept {
		return _items_list.cend();
	}

	auto find(const Key& key) {
		auto it(_items_map.find(key));
		if (it == _items_map.end()) {
			return _items_list.end();
		}
		return it->second;
	}

	auto find(const Key& key) const {
		auto it(_items_map.find(key));
		if (it == _items_map.cend()) {
			return _items_list.cend();
		}
		return it->second;
	}

	size_t erase(const Key& key) {
		auto it(_items_map.find(key));
		if (it != _items_map.end()) {
			_items_list.erase(it->second);
			_items_map.erase(it);
			return 1;
		}
		return 0;
	}

	template<typename P>
	T& insert(P&& p) {
		erase(p.first);

		_items_list.push_front(std::forward<P>(p));
		auto first(_items_list.begin());
		_items_map[first->first] = first;

		if (_max_size != -1) {
			auto last(_items_list.rbegin());
			for (size_t i = _items_map.size(); i != 0 && static_cast<ssize_t>(_items_map.size()) > _max_size && last != _items_list.rend(); --i) {
				auto it = (++last).base();
				_items_map.erase(it->first);
				_items_list.erase(it);
				last = _items_list.rbegin();
			}
		}

		return first->second;
	}

	template<typename... Args>
	T& emplace(Args&&... args) {
		return insert(std::make_pair(std::forward<Args>(args)...));
	}

	T& at(const iterator& it) {
		_items_list.splice(_items_list.begin(), _items_list, it);
		return it->second;
	}

	T& at(const Key& key) {
		auto it(_items_map.find(key));
		if (it == _items_map.end()) {
			throw std::range_error("There is no such key in cache");
		}
		return at(it->second);
	}

	T& get(const Key& key) {
		auto it(_items_map.find(key));
		if (it == _items_map.end()) {
			return insert(std::make_pair(key, T()));
		}
		return at(it->second);
	}

	T& operator[] (const Key& key) {
		return get(key);
	}

	bool exists(const Key& key) const {
		return _items_map.find(key) != _items_map.end();
	}

	void clear() noexcept {
		_items_map.clear();
		_items_list.clear();
	}

	bool empty() const noexcept {
		return _items_map.empty();
	}

	size_t size() const noexcept {
		return _items_map.size();
	}

	size_t max_size() const noexcept {
		return (_max_size == -1) ? _items_map.max_size() : _max_size;
	}

	template<typename OnDrop>
	void trim(const OnDrop& on_drop, ssize_t m_size) {
		auto last(_items_list.rbegin());
		for (size_t i= _items_map.size(); i != 0 && m_size > _max_size && last != _items_list.rend(); --i) {
			auto it = --last.base();
			switch (on_drop(it->second).second) {
				case DropAction::renew:
					_items_list.splice(_items_list.begin(), _items_list, it);
					break;
				case DropAction::leave:
					break;
				case DropAction::drop:
					_items_map.erase(it->first);
					_items_list.erase(it);
					break;
			}
			last = _items_list.rbegin();
		}
	}

	template<typename OnDrop, typename P>
	T& insert_and(const OnDrop& on_drop, P&& p) {
		erase(p.first);

		switch (on_drop(p.second).first) {
			case InsertAction::front: {
				_items_list.push_front(std::forward<P>(p));
				auto first(_items_list.begin());
				_items_map[first->first] = first;

				if (_max_size != -1) {
					trim(on_drop, static_cast<ssize_t>(_items_map.size()));
				}
				return first->second;
			}
			case InsertAction::last: {
				auto m_size = static_cast<ssize_t>(_items_map.size());
				if (_max_size != -1 && m_size == _max_size) {
					trim(on_drop, m_size + 1);
				}

				_items_list.push_back(std::forward<P>(p));
				auto last(_items_list.rbegin());
				_items_map[last->first] = --last.base();
				return last->second;
			}
		}
	}

	template<typename OnDrop, typename... Args>
	T& emplace_and(OnDrop&& on_drop, Args&&... args) {
		return insert_and(std::forward<OnDrop>(on_drop), std::make_pair(std::forward<Args>(args)...));
	}

	template<typename OnGet>
	T& at_and(const OnGet& on_get, const iterator& it) {
		T& ref = it->second;
		switch (on_get(ref)) {
			case GetAction::leave:
				break;
			case GetAction::renew:
				_items_list.splice(_items_list.begin(), _items_list, it);
				break;
		}
		return ref;
	}

	template<typename OnGet>
	T& at_and(const OnGet& on_get, const Key& key) {
		auto it(_items_map.find(key));
		if (it == _items_map.end()) {
			throw std::range_error("There is no such key in cache");
		}
		return at_and(on_get, it->second);
	}

	template<typename OnGet>
	T& get_and(const OnGet& on_get, const Key& key) {
		auto it(_items_map.find(key));
		if (it == _items_map.end()) {
			T& ref = insert(std::make_pair(key, T()));
			switch (on_get(ref)) {
				case GetAction::leave:
					break;
				case GetAction::renew:
					break;
			}
			return ref;
		}
		return at_and(on_get, it->second);
	}
};

};
