/*
 * Copyright (c) 2015-2019 Dubalu LLC
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

#include "config.h"              // for HAVE_PWRITE, HAVE_FSYNC

#include <atomic>                // for std::atomic_bool
#include <errno.h>               // for errno, EINTR
#include <fcntl.h>               // for fchmod, open, O_RDONLY
#include <stddef.h>              // for size_t
#include <stdlib.h>              // for mkdtemp
#include <sys/stat.h>            // for fstat
#include <sys/socket.h>          // for send
#include <unistd.h>              // for off_t, ssize_t, close, lseek, unlink
#include <type_traits>           // for std::forward

#ifdef XAPIAND_RANDOM_ERRORS
#include "random.hh"                // for random_real
#include "opts.h"                   // for opts
#define RANDOM_ERRORS_IO_ERRNO_RETURN(errnum) \
	if (opts.random_errors_io) { \
		auto prob = random_real(0, 1); \
		if (prob < opts.random_errors_io) { \
			errno = errnum; \
			return -1; \
		} \
	}
#define RANDOM_ERRORS_NET_ERRNO_RETURN(errnum, sock) \
	if (opts.random_errors_net) { \
		auto prob = random_real(0, 1); \
		if (prob < opts.random_errors_net) { \
			if (sock) { \
				::shutdown(sock, SHUT_RDWR); \
			} \
			errno = errnum; \
			return -1; \
		} \
	}
#else
#define RANDOM_ERRORS_IO_ERRNO_RETURN(...)
#define RANDOM_ERRORS_NET_ERRNO_RETURN(...)
#endif


#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-statement-expression"

// Do not accept any file descriptor less than this value, in order to avoid
// opening database file using file descriptors that are commonly used for
// standard input, output, and error.
#ifndef XAPIAND_MINIMUM_FILE_DESCRIPTOR
#define XAPIAND_MINIMUM_FILE_DESCRIPTOR STDERR_FILENO + 1
#endif


namespace io {

#ifdef XAPIAND_CHECK_IO_FDES
#define OPENED 1
#define SOCKET 2
#define CLOSED 4
int check(const char* msg, int fd, int check_set, int check_unset, int set, const char* function, const char* filename, int line) noexcept;
#define CHECK_OPEN(fd) io::check("while opening as file", fd, 0, OPENED | CLOSED, OPENED, __func__, __FILE__, __LINE__)
#define CHECK_OPEN_SOCKET(fd) io::check("while opening as socket", fd, 0, OPENED | SOCKET | CLOSED, OPENED | SOCKET, __func__, __FILE__, __LINE__)
#define CHECK_CLOSING(fd) io::check("while closing", fd, OPENED, 0, 0, __func__, __FILE__, __LINE__)
#define CHECK_CLOSE(fd) io::check("while closing", fd, 0, CLOSED, CLOSED, __func__, __FILE__, __LINE__)
#define CHECK_OPENED(msg, fd) io::check(msg, fd, OPENED, CLOSED, 0, __func__, __FILE__, __LINE__)
#define CHECK_OPENED_SOCKET(msg, fd) io::check(msg, fd, OPENED | SOCKET, CLOSED, 0, __func__, __FILE__, __LINE__)
#else
#define CHECK_OPEN(fd)
#define CHECK_OPEN_SOCKET(fd)
#define CHECK_CLOSING(fd)
#define CHECK_CLOSE(fd)
#define CHECK_OPENED(msg, fd)
#define CHECK_OPENED_SOCKET(msg, fd)
#endif


#if defined HAVE_FDATASYNC
#define __io_fsync ::fdatasync
#elif defined HAVE_FSYNC
#define __io_fsync ::fsync
#else
inline int __noop(int /*unused*/) noexcept { return 0; }
#define __io_fsync io::__noop
#endif


std::atomic_bool& ignore_eintr() noexcept;


inline bool ignored_errno(int e, bool again, bool tcp, bool udp) noexcept {
	switch(e) {
		case EINTR:
			return ignore_eintr().load();  //  Always ignore error
		case EAGAIN:
#if EAGAIN != EWOULDBLOCK
		case EWOULDBLOCK:
#endif
			return again;  //  Ignore not-available error
		case EALREADY:
		case EINPROGRESS:
			return tcp;  //  Ignore error on TCP sockets

		case ENETDOWN:
		case EPROTO:
		case ENOPROTOOPT:
		case EHOSTDOWN:
#ifdef ENONET  // Linux-specific
		case ENONET:
#endif
		case EHOSTUNREACH:
		case EOPNOTSUPP:
		case ENETUNREACH:
		case ECONNRESET:
			return udp;  //  Ignore error on UDP sockets

		default:
			return false;  // Do not ignore error
	}
}


template <typename Fun, typename... Args>
inline auto RetryAfterSignal(const Fun &F, const Args &... As) noexcept -> decltype(F(As...)) {
	decltype(F(As...)) _err;
	do {
		errno = 0;
		_err = F(As...);
	} while (_err == -1 && errno == EINTR && ignore_eintr().load());
	return _err;
}


int open(const char* path, int oflag=O_RDONLY, int mode = 0644) noexcept;
int close(int fd) noexcept;

ssize_t write(int fd, const void* buf, size_t nbyte) noexcept;
ssize_t pwrite(int fd, const void* buf, size_t nbyte, off_t offset) noexcept;

ssize_t read(int fd, void* buf, size_t nbyte) noexcept;
ssize_t pread(int fd, void* buf, size_t nbyte, off_t offset) noexcept;


inline int mkstemp(char* template_) noexcept {
	RANDOM_ERRORS_IO_ERRNO_RETURN(EIO);

	int fd = ::mkstemp(template_);
	CHECK_OPEN(fd);
	return fd;
}

inline char* mkdtemp(char* template_) noexcept {
	return ::mkdtemp(template_);
}


inline int unlink(const char* path) noexcept {
	return ::unlink(path);
}


inline off_t lseek(int fd, off_t offset, int whence) noexcept {
	CHECK_OPENED("during lseek()", fd);

	RANDOM_ERRORS_IO_ERRNO_RETURN(EIO);

	return ::lseek(fd, offset, whence);
}


template <typename... Args>
inline int unchecked_fcntl(int fd, int cmd, Args&&... args) noexcept {
	RANDOM_ERRORS_IO_ERRNO_RETURN(EIO);

	return RetryAfterSignal(::fcntl, fd, cmd, std::forward<Args>(args)...);
}


template <typename... Args>
inline int fcntl(int fd, int cmd, Args&&... args) noexcept {
	CHECK_OPENED("during fcntl()", fd);

	RANDOM_ERRORS_IO_ERRNO_RETURN(EIO);

	return io::unchecked_fcntl(fd, cmd, std::forward<Args>(args)...);
}


inline int fstat(int fd, struct stat* buf) noexcept {
	CHECK_OPENED("during fstat()", fd);

	RANDOM_ERRORS_IO_ERRNO_RETURN(EIO);

	return ::fstat(fd, buf);
}


inline int dup(int fd) noexcept {
	CHECK_OPENED("during dup()", fd);

	RANDOM_ERRORS_IO_ERRNO_RETURN(EIO);

	return ::dup(fd);
}


inline int dup2(int fd, int fd2) noexcept {
	CHECK_OPENED("during dup2()", fd);

	RANDOM_ERRORS_IO_ERRNO_RETURN(EIO);

	return ::dup2(fd, fd2);  // RetryAfterSignal?
}


inline int shutdown(int socket, int how) noexcept {
	CHECK_OPENED_SOCKET("during shutdown()", socket);

	RANDOM_ERRORS_NET_ERRNO_RETURN(ECONNABORTED, 0);

	return ::shutdown(socket, how);
}


inline ssize_t send(int socket, const void* buffer, size_t length, int flags) noexcept {
	CHECK_OPENED_SOCKET("during send()", socket);

	RANDOM_ERRORS_NET_ERRNO_RETURN(ECONNABORTED, socket);

	return RetryAfterSignal(::send, socket, buffer, length, flags);
}


inline ssize_t sendto(int socket, const void* buffer, size_t length, int flags, const struct sockaddr* dest_addr, socklen_t dest_len) noexcept {
	CHECK_OPENED_SOCKET("during sendto()", socket);

	RANDOM_ERRORS_NET_ERRNO_RETURN(ECONNABORTED, socket);

	return RetryAfterSignal(::sendto, socket, buffer, length, flags, dest_addr, dest_len);
}


inline ssize_t recv(int socket, void* buffer, size_t length, int flags) noexcept {
	CHECK_OPENED_SOCKET("during recv()", socket);

	RANDOM_ERRORS_NET_ERRNO_RETURN(ECONNABORTED, socket);

	return RetryAfterSignal(::recv, socket, buffer, length, flags);
}


inline ssize_t recvfrom(int socket, void* buffer, size_t length, int flags, struct sockaddr* address, socklen_t* address_len) noexcept {
	CHECK_OPENED_SOCKET("during recvfrom()", socket);

	RANDOM_ERRORS_NET_ERRNO_RETURN(ECONNABORTED, socket);

	return RetryAfterSignal(::recvfrom, socket, buffer, length, flags, address, address_len);
}


inline int socket(int domain, int type, int protocol) noexcept {
	RANDOM_ERRORS_NET_ERRNO_RETURN(ENETDOWN, 0);

	int socket = ::socket(domain, type, protocol);
	CHECK_OPEN_SOCKET(socket);
	return socket;
}


inline int getsockopt(int socket, int level, int option_name, void* option_value, socklen_t* option_len) noexcept {
	CHECK_OPENED_SOCKET("during getsockopt()", socket);

	RANDOM_ERRORS_NET_ERRNO_RETURN(EOPNOTSUPP, 0);

	return ::getsockopt(socket, level, option_name, option_value, option_len);
}


inline int setsockopt(int socket, int level, int option_name, const void* option_value, socklen_t option_len) noexcept {
	CHECK_OPENED_SOCKET("during setsockopt()", socket);

	RANDOM_ERRORS_NET_ERRNO_RETURN(EOPNOTSUPP, 0);

	return ::setsockopt(socket, level, option_name, option_value, option_len);
}


inline int listen(int socket, int backlog) noexcept {
	CHECK_OPENED_SOCKET("during listen()", socket);

	RANDOM_ERRORS_NET_ERRNO_RETURN(ENETDOWN, 0);

	return ::listen(socket, backlog);
}


inline int accept(int socket, struct sockaddr* address, socklen_t* address_len) noexcept {
	CHECK_OPENED_SOCKET("during accept()", socket);
	int new_socket = RetryAfterSignal(::accept, socket, address, address_len);
	CHECK_OPEN_SOCKET(new_socket);

	RANDOM_ERRORS_NET_ERRNO_RETURN(ENETDOWN, 0);

	return new_socket;
}


inline int bind(int socket, const struct sockaddr *address, socklen_t address_len) noexcept {
	CHECK_OPENED_SOCKET("during bind()", socket);

	RANDOM_ERRORS_NET_ERRNO_RETURN(EOPNOTSUPP, 0);

	return ::bind(socket, address, address_len);
}


inline int connect(int socket, const struct sockaddr* address, socklen_t address_len) noexcept {
	CHECK_OPENED_SOCKET("during connect()", socket);

	RANDOM_ERRORS_NET_ERRNO_RETURN(ENETDOWN, 0);

	return RetryAfterSignal(::connect, socket, address, address_len);
}


inline int unchecked_fsync(int fd) noexcept {
	RANDOM_ERRORS_IO_ERRNO_RETURN(EIO);

	return RetryAfterSignal(__io_fsync, fd);
}


inline int fsync(int fd) noexcept {
	CHECK_OPENED("during fsync()", fd);

	RANDOM_ERRORS_IO_ERRNO_RETURN(EIO);

	return io::unchecked_fsync(fd);
}


inline int unchecked_full_fsync(int fd) noexcept {
	RANDOM_ERRORS_IO_ERRNO_RETURN(EIO);

#ifdef F_FULLFSYNC
	return RetryAfterSignal(::fcntl, fd, F_FULLFSYNC, 0);
#else
	return RetryAfterSignal(__io_fsync, fd);
#endif
}


inline int full_fsync(int fd) noexcept {
	CHECK_OPENED("during full_fsync()", fd);

	RANDOM_ERRORS_IO_ERRNO_RETURN(EIO);

	return io::unchecked_full_fsync(fd);
}


#ifdef HAVE_FALLOCATE
inline int fallocate(int fd, int mode, off_t offset, off_t len) noexcept {
	CHECK_OPENED("during fallocate()", fd);

	RANDOM_ERRORS_IO_ERRNO_RETURN(EIO);

	return RetryAfterSignal(::fallocate, fd, mode, offset, len);
}
#else
int fallocate(int fd, int mode, off_t offset, off_t len) noexcept;
#endif


#ifdef HAVE_POSIX_FADVISE
inline int fadvise(int fd, off_t offset, off_t len, int advice) noexcept {
	CHECK_OPENED("during fadvise()", fd);

	RANDOM_ERRORS_IO_ERRNO_RETURN(EIO);

	return ::posix_fadvise(fd, offset, len, advice) == 0;
}
#else
#define POSIX_FADV_NORMAL     0
#define POSIX_FADV_SEQUENTIAL 1
#define POSIX_FADV_RANDOM     2
#define POSIX_FADV_WILLNEED   3
#define POSIX_FADV_DONTNEED   4
#define POSIX_FADV_NOREUSE    5

inline int fadvise([[maybe_unused]] int fd, off_t, off_t, int) noexcept {
	CHECK_OPENED("during fadvise()", fd);

	RANDOM_ERRORS_IO_ERRNO_RETURN(EIO);

	return 0;
}
#endif

} /* namespace io */

#pragma clang diagnostic pop
