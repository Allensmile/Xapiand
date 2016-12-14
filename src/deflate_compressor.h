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

#include <stdlib.h>      // for malloc, free
#include <zlib.h>        // for z_stream
#include <string>        // for string

#include "io_utils.h"    // for close, open
#include "exception.h"   // for Error


#define DEFLATE_BLOCK_SIZE 16384


class DeflateException : public Error {
public:
	template<typename... Args>
	DeflateException(Args&&... args) : Error(std::forward<Args>(args)...) { }
};

class DeflateIOError : public DeflateException {
public:
	template<typename... Args>
	DeflateIOError(Args&&... args) : DeflateException(std::forward<Args>(args)...) { }
};


template<typename Impl>
class DeflateBlockStreaming {
protected:
	enum class DeflateState : uint8_t {
		INT,
		END
	};

	int stream;
	DeflateState state;
	const int cmpBuf_size;
	char* cmpBuf;
	char* buffer;

	inline std::string _init() {
		return static_cast<Impl*>(this)->init();
	}

	inline std::string _next() {
		return static_cast<Impl*>(this)->next();
	}

public:
	DeflateBlockStreaming()
		: stream(0),
	      state(DeflateState::INT),
	      cmpBuf_size(DEFLATE_BLOCK_SIZE),
		  cmpBuf((char*)malloc(cmpBuf_size)),
		  buffer((char*)malloc(DEFLATE_BLOCK_SIZE)) { }

	// This class is not CopyConstructible or CopyAssignable.
	DeflateBlockStreaming(const DeflateBlockStreaming&) = delete;
	DeflateBlockStreaming operator=(const DeflateBlockStreaming&) = delete;

	~DeflateBlockStreaming() {
		free(cmpBuf);
		free(buffer);
	}

	class iterator : public std::iterator<std::input_iterator_tag, DeflateBlockStreaming> {
		DeflateBlockStreaming* obj;
		std::string current_str;
		size_t offset;

		friend class DeflateBlockStreaming;

	public:
		iterator()
			: obj(nullptr),
			  offset(0) { }

		iterator(DeflateBlockStreaming* o, std::string&& str)
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
			return obj->state != DeflateState::END;
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
			memcpy(buf, current_str.data() + offset, buf_size);
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
};


class DeflateData {
protected:
	const char* data;
	size_t data_size;
	size_t data_offset;

	DeflateData(const char* data_, size_t data_size_)
		: data(data_),
		  data_size(data_size_),
		  data_offset(0) { }

	~DeflateData() = default;

public:
	inline void add_data(const char* data_, size_t data_size_) {
		data = data_;
		data_size = data_size_;
	}
};

/*
 * Compress Data.
 */
class DeflateCompressData : public DeflateData, public DeflateBlockStreaming<DeflateCompressData> {

	z_stream strm;
	std::string next();

	friend class DeflateBlockStreaming<DeflateCompressData>;

public:
	DeflateCompressData(const char* data_=nullptr, size_t data_size_=0);
	~DeflateCompressData();

	std::string init(bool start=true);
	std::string next(const char* input, size_t input_size, int flush=Z_NO_FLUSH);

	static int FINISH_COMPRESS;

	inline void reset(const char* data_, size_t data_size_) {
		add_data(data_, data_size_);
	}
};


/*
 * Decompress Data.
 */
class DeflateDecompressData : public DeflateData, public DeflateBlockStreaming<DeflateDecompressData> {

	z_stream strm;
	std::string init();
	std::string next();

	friend class DeflateBlockStreaming<DeflateDecompressData>;

public:
	DeflateDecompressData(const char* data_=nullptr, size_t data_size_=0);
	~DeflateDecompressData();

	inline void reset(const char* data_, size_t data_size_) {
		add_data(data_, data_size_);
	}
};


class DeflateFile {
protected:
	int fd;
	off_t fd_offset;
	off_t fd_nbytes;
	bool fd_internal;

	size_t bytes_readed;
	size_t size_file;

	DeflateFile(const std::string& filename)
		: bytes_readed(0),
		  size_file(0)
	{
		open(filename);
	}

	DeflateFile(int fd_, off_t fd_offset_, off_t fd_nbytes_)
		: bytes_readed(0),
		  size_file(0)
	{
		add_fildes(fd_, fd_offset_, fd_nbytes_);
	}

	~DeflateFile() {
		if (fd_internal) {
			io::close(fd);
		}
	}

public:
	inline void open(const std::string& filename) {
		fd = io::open(filename.c_str(), O_RDONLY, 0644);
		 if unlikely(fd < 0) {
		 	THROW(DeflateIOError, "Cannot open file: %s", filename.c_str());
		 }
		fd_offset = 0;
		fd_nbytes = -1;
		fd_internal = true;
	}

	inline void add_fildes(int fd_, size_t fd_offset_, size_t fd_nbytes_) {
		fd = fd_;
		fd_offset = fd_offset_;
		fd_nbytes = fd_nbytes_;
		fd_internal = false;
	}

	inline void add_file(const std::string& filename) {
		open(filename);
	}
};


/*
 * Compress a file.
 */
class DeflateCompressFile : public DeflateFile, public DeflateBlockStreaming<DeflateCompressFile> {

	z_stream strm;
	std::string init();
	std::string next();

	friend class DeflateBlockStreaming<DeflateCompressFile>;

public:
	DeflateCompressFile(const std::string& filename);
	DeflateCompressFile(int fd_=0, off_t fd_offset_=-1, off_t fd_nbytes_=-1);
	~DeflateCompressFile();

	inline void reset(int fd_, size_t fd_offset_, size_t fd_nbytes_) {
		add_fildes(fd_, fd_offset_, fd_nbytes_);
	}

	inline void reset(const std::string& filename) {
		open(filename);
	}
};


/*
 * Decompress a file.
 */
class DeflateDecompressFile : public DeflateFile, public DeflateBlockStreaming<DeflateDecompressFile> {

	z_stream strm;
	ssize_t data_size;
	size_t data_offset;

	std::string init();
	std::string next();

	friend class DeflateBlockStreaming<DeflateDecompressFile>;

public:
	DeflateDecompressFile(const std::string& filename);
	DeflateDecompressFile(int fd_=0, off_t fd_offset_=-1, off_t fd_nbytes_=-1);
	~DeflateDecompressFile();

	inline void reset(int fd_, size_t fd_offset_, size_t fd_nbytes_) {
	 	add_fildes(fd_, fd_offset_, fd_nbytes_);
	 }

	inline void reset(const std::string& filename) {
		open(filename);
	}
};
