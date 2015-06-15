/*
 * Copyright (C) 2015 deipi.com LLC and contributors. All rights reserved.
 * Copyright (C) 2014, lamerman. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of lamerman nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVERCAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef XAPIAND_INCLUDED_LRU_H
#define	XAPIAND_INCLUDED_LRU_H

#include <cstddef>
#include <stdexcept>
#include <list>

#ifdef HAVE_CXX11
#  include <unordered_map>
#else
#  include <map>
#endif


template<typename key_t, typename value_t>
class lru_map {
	typedef typename std::pair<const key_t, value_t> key_value_pair_t;
	typedef typename std::list<key_value_pair_t>::iterator list_iterator_t;
#ifdef HAVE_CXX11
	typedef std::unordered_map<const key_t, list_iterator_t> lru_map_t;
#else
	typedef std::map<const key_t, list_iterator_t> lru_map_t;
#endif
	typedef typename std::list<key_value_pair_t> lru_list_t;
	typedef typename lru_map_t::iterator map_iterator_t;

protected:
	lru_list_t _items_list;
	lru_map_t _items_map;
	size_t _max_size;

public:
	lru_map(size_t max_size=-1) :
		_max_size(max_size) {
	}

	size_t erase(const key_t & key) {
		map_iterator_t it = _items_map.find(key);
		if (it != _items_map.end()) {
			_items_list.erase(it->second);
			_items_map.erase(it);
			return 1;
		}
		return 0;
	}

	value_t & insert(const key_value_pair_t &p) {
		erase(p.first);

		_items_list.push_front(p);
		list_iterator_t first = _items_list.begin();
		_items_map[p.first] = first;

		if (_max_size != -1 && _items_map.size() > _max_size) {
			list_iterator_t last = _items_list.end();
			last--;
			_items_map.erase(last->first);
			_items_list.pop_back();
		}
		return first->second;
	}

	value_t & at(const key_t & key) {
		map_iterator_t it = _items_map.find(key);
		if (it == _items_map.end()) {
			throw std::range_error("There is no such key in cache");
		} else {
			_items_list.splice(_items_list.begin(), _items_list, it->second);
			return it->second->second;
		}
	}

	value_t & operator[] (const key_t & key) {
		try {
			return at(key);
		} catch (std::range_error) {
			return insert(key_value_pair_t(key, value_t()));
		}
	}

	bool exists(const key_t & key) const {
		return _items_map.find(key) != _items_map.end();
	}

	size_t size() const {
		return _items_map.size();
	}

	size_t empty() const {
		return _items_map.empty();
	}

	size_t max_size() const {
		return (_max_size == -1) ? _items_map.max_size() : _max_size;
	}
};

#endif	/* XAPIAND_INCLUDED_LRU_H */
