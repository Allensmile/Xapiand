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

#pragma once

#include "exception.h"
#include "lz4/lz4.h"
#include "lz4/xxhash.h"
#include "xapiand.h"

#include <fcntl.h>
#include <iostream>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>


#define LZ4_BLOCK_SIZE (1024 * 2)
#define LZ4_FILE_READ_SIZE (LZ4_BLOCK_SIZE * 2)	// it must be greater than or equal to LZ4_COMPRESSBOUND(LZ4_BLOCK_SIZE).
#define LZ4_RING_BUFFER_BYTES (1024 * 256 + LZ4_BLOCK_SIZE)


class LZ4Exception : public Error {
public:
	template<typename... Args>
	LZ4Exception(Args&&... args) : Error(std::forward<Args>(args)...) { }
};

#define MSG_LZ4Exception(...) LZ4Exception(__FILE__, __LINE__, __VA_ARGS__)


class LZ4IOError : public LZ4Exception {
public:
	template<typename... Args>
	LZ4IOError(Args&&... args) : LZ4Exception(std::forward<Args>(args)...) { }
};

#define MSG_LZ4IOError(...) LZ4IOError(__FILE__, __LINE__, __VA_ARGS__)


class LZ4CorruptVolume : public LZ4Exception {
public:
	template<typename... Args>
	LZ4CorruptVolume(Args&&... args) : LZ4Exception(std::forward<Args>(args)...) { }
};

#define MSG_LZ4CorruptVolume(...) LZ4CorruptVolume(__FILE__, __LINE__, __VA_ARGS__)


template<typename Impl>
class LZ4BlockStreaming {
protected:
	// These variables must be defined in init function.
	size_t _size;
	bool _finish;
	size_t _offset;

	const int block_size;
	const int cmpBuf_size;

	char* const cmpBuf;
	char* const buffer;

	XXH32_state_t* xxh_state;

	inline std::string _init() {
		return static_cast<Impl*>(this)->init();
	}

	inline std::string _next() {
		return static_cast<Impl*>(this)->next();
	}

public:
	LZ4BlockStreaming(int block_size_, int seed)
		: block_size(block_size_),
		  cmpBuf_size(LZ4_COMPRESSBOUND(block_size)),
		  cmpBuf((char*)malloc(cmpBuf_size)),
		  buffer((char*)malloc(LZ4_RING_BUFFER_BYTES)),
		  xxh_state(XXH32_createState()) {
		XXH32_reset(xxh_state, seed);
	}

	// This class is not CopyConstructible or CopyAssignable.
	LZ4BlockStreaming(const LZ4BlockStreaming&) = delete;
	LZ4BlockStreaming operator=(const LZ4BlockStreaming&) = delete;

	~LZ4BlockStreaming() {
		free(cmpBuf);
		free(buffer);
		XXH32_freeState(xxh_state);
	}

	class iterator : public std::iterator<std::input_iterator_tag, LZ4BlockStreaming> {
		LZ4BlockStreaming* obj;
		std::string current_str;
		size_t offset;

		friend class LZ4BlockStreaming;

	public:
		iterator()
			: obj(nullptr),
			  offset(0) { }

		iterator(LZ4BlockStreaming* o, std::string&& str)
			: obj(o),
			  current_str(std::move(str)),
			  offset(0) { }

		iterator(iterator&& it)
			: obj(std::move(it.obj)),
			  current_str(std::move(it.current_str)),
			  offset(std::move(it.offset)) { }

		iterator& operator=(iterator&& it) {
			obj = std::move(it.obj);
			current_str = std::move(it.current_str);
			offset = std::move(it.offset);
			return *this;
		}

		// iterator is not CopyConstructible or CopyAssignable.
		iterator(const iterator&) = delete;
		iterator& operator=(const iterator&) = delete;

		iterator& operator++() {
			current_str = obj->_next();
			offset = 0;
			return *this;
		}

		inline std::string operator*() const {
			return current_str;
		}

		inline const std::string* operator->() const {
			return &current_str;
		}

		inline size_t size() const noexcept {
			return current_str.size();
		}

		bool operator==(const iterator& other) const {
			return current_str == other.current_str;
		}

		bool operator!=(const iterator& other) const {
			return !operator==(other);
		}

		inline explicit operator bool() const {
			return !current_str.empty();
		}

		inline size_t read(char* buf, size_t buf_size) {
			size_t res_size = current_str.size() - offset;
			if (!res_size) {
				current_str = obj->_next();
				offset = 0;
				res_size = current_str.size();
			}

			if (res_size < buf_size) {
				buf_size = res_size;
			}
			memcpy(buf, current_str.c_str() + offset, buf_size);
			offset += buf_size;
			return buf_size;
		}
	};

	iterator begin() {
		return iterator(this, _init());
	}

	iterator end() {
		return iterator(this, std::string());
	}

	inline size_t size() const noexcept {
		return _size;
	}

	inline uint32_t get_digest() {
		return XXH32_digest(xxh_state);
	}
};


/*
 * Compress Data.
 */
class LZ4CompressData : public LZ4BlockStreaming<LZ4CompressData> {
	LZ4_stream_t* const lz4Stream;

	const char* data;
	const size_t data_size;
	size_t data_offset;

	std::string init();
	std::string next();

	friend class LZ4BlockStreaming<LZ4CompressData>;

public:
	LZ4CompressData(const char* data_, size_t data_size_, int seed=0);

	~LZ4CompressData();
};


/*
 * Compress a file.
 */
class LZ4CompressFile : public LZ4BlockStreaming<LZ4CompressFile> {
	LZ4_stream_t* const lz4Stream;

	int fd;

	std::string init();
	std::string next();

	friend class LZ4BlockStreaming<LZ4CompressFile>;

public:
	LZ4CompressFile(const std::string& filename, int seed=0);

	~LZ4CompressFile();
};


/*
 * Compress read_bytes of the descriptor file (fd) from the current position.
 * Each time that call begin(), compress read_bytes from the current position.
 */
class LZ4CompressDescriptor : public LZ4BlockStreaming<LZ4CompressDescriptor> {
	LZ4_stream_t* const lz4Stream;

	int& fd;
	size_t read_bytes;

	std::string init();
	std::string next();

	friend class LZ4BlockStreaming<LZ4CompressDescriptor>;

public:
	LZ4CompressDescriptor(int& fildes, int seed=0);

	~LZ4CompressDescriptor();

	inline void reset(size_t read_bytes_, int seed=0) noexcept {
		read_bytes = read_bytes_;
		XXH32_reset(xxh_state, seed);
	}
};


/*
 * Decompress Data.
 */
class LZ4DecompressData : public LZ4BlockStreaming<LZ4DecompressData> {
	LZ4_streamDecode_t* const lz4StreamDecode;

	const char* data;
	const size_t data_size;
	size_t data_offset;

	std::string init();
	std::string next();

	friend class LZ4BlockStreaming<LZ4DecompressData>;

public:
	LZ4DecompressData(const char* data_, size_t data_size_, int seed=0);

	~LZ4DecompressData();
};


/*
 * Decompress a file.
 */
class LZ4DecompressFile : public LZ4BlockStreaming<LZ4DecompressFile> {
	LZ4_streamDecode_t* const lz4StreamDecode;

	int fd;

	char* const data;
	ssize_t data_size;
	size_t data_offset;

	std::string init();
	std::string next();

	friend class LZ4BlockStreaming<LZ4DecompressFile>;

public:
	LZ4DecompressFile(const std::string& filename, int seed=0);

	~LZ4DecompressFile();
};


/*
 * Decompress read_bytes of the descriptor file (fd) from the current position.
 * Each time that call begin(), decompress read_bytes from the current position.
 */
class LZ4DecompressDescriptor : public LZ4BlockStreaming<LZ4DecompressDescriptor> {
	LZ4_streamDecode_t* const lz4StreamDecode;

	int& fd;
	size_t read_bytes;

	char* const data;
	ssize_t data_size;
	size_t data_offset;

	std::string init();
	std::string next();

	friend class LZ4BlockStreaming<LZ4DecompressDescriptor>;

public:
	LZ4DecompressDescriptor(int& fildes, int seed=0);

	~LZ4DecompressDescriptor();

	inline void reset(size_t read_bytes_, int seed=0) noexcept {
		read_bytes = read_bytes_;
		XXH32_reset(xxh_state, seed);
	}
};
