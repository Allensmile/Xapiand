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

#ifndef IO_UTILS_H
#define IO_UTILS_H

#include "xapiand.h"
#include "ignore_unused.h"       // for ignore_unused

#include <errno.h>               // for errno
#include <fcntl.h>               // for fchmod, open, O_RDONLY
#include <stddef.h>              // for size_t
#include <sys/stat.h>            // for fstat
#include <sys/socket.h>          // for send
#include <unistd.h>              // for off_t, ssize_t, close, lseek, unlink


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
int check(const char* msg, int fd, int check_set, int check_unset, int set, const char* function, const char* filename, int line);
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


inline int unlink(const char* path) {
	return ::unlink(path);
}


inline int open(const char* path, int oflag=O_RDONLY, int mode=0644) {
	int fd;
	while (true) {
#ifdef O_CLOEXEC
		fd = ::open(path, oflag | O_CLOEXEC, mode);
#else
		fd = ::open(path, oflag, mode);
#endif
		if (fd == -1) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (fd >= XAPIAND_MINIMUM_FILE_DESCRIPTOR) {
			break;
		}
		::close(fd);
		fd = -1;
		if (::open("/dev/null", oflag, mode) == -1) {
			break;
		}
	}
	if (fd != -1) {
		if (mode != 0) {
			struct stat statbuf;
			if (::fstat(fd, &statbuf) == 0 && statbuf.st_size == 0 && (statbuf.st_mode & 0777) != mode) {
				::fchmod(fd, mode);
			}
		}
#if defined(FD_CLOEXEC) && (!defined(O_CLOEXEC) || O_CLOEXEC == 0)
		::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
#endif
	}
	CHECK_OPEN(fd);
	return fd;
}


inline int close(int fd) {
	// Make sure we don't ever close 0, 1 or 2 file descriptors
	assert(fd != -1 && fd >= XAPIAND_MINIMUM_FILE_DESCRIPTOR);
	if (fd == -1 || fd >= XAPIAND_MINIMUM_FILE_DESCRIPTOR) {
		CHECK_CLOSING(fd);
		return ::close(fd);
	}
	return -1;
}


inline off_t lseek(int fd, off_t offset, int whence) {
	CHECK_OPENED("during lseek()", fd);
	return ::lseek(fd, offset, whence);
}


inline int fcntl(int fd, int cmd, int arg) {
	CHECK_OPENED("during fcntl()", fd);
	return ::fcntl(fd, cmd, arg);
}


inline int fstat(int fd, struct stat* buf) {
	CHECK_OPENED("during fstat()", fd);
	return ::fstat(fd, buf);
}


inline int dup(int fd) {
	CHECK_OPENED("during dup()", fd);
	return ::dup(fd);
}


inline int dup2(int fd, int fd2) {
	CHECK_OPENED("during dup2()", fd);
	return ::dup2(fd, fd2);
}


inline int shutdown(int socket, int how) {
	CHECK_OPENED_SOCKET("during shutdown()", socket);
	return ::shutdown(socket, how);
}


inline ssize_t send(int socket, const void* buffer, size_t length, int flags) {
	CHECK_OPENED_SOCKET("during send()", socket);
	return ::send(socket, buffer, length, flags);
}


inline ssize_t sendto(int socket, const void* buffer, size_t length, int flags, const struct sockaddr* dest_addr, socklen_t dest_len) {
	CHECK_OPENED_SOCKET("during sendto()", socket);
	return ::sendto(socket, buffer, length, flags, dest_addr, dest_len);
}


inline ssize_t recv(int socket, void* buffer, size_t length, int flags) {
	CHECK_OPENED_SOCKET("during recv()", socket);
	return ::recv(socket, buffer, length, flags);
}

inline int socket(int domain, int type, int protocol) {
	int socket = ::socket(domain, type, protocol);
	CHECK_OPEN_SOCKET(socket);
	return socket;
}

inline ssize_t recvfrom(int socket, void* buffer, size_t length, int flags, struct sockaddr* address, socklen_t* address_len) {
	CHECK_OPENED_SOCKET("during recvfrom()", socket);
	return ::recvfrom(socket, buffer, length, flags, address, address_len);
}


inline int getsockopt(int socket, int level, int option_name, void* option_value, socklen_t* option_len){
	CHECK_OPENED_SOCKET("during getsockopt()", socket);
	return ::getsockopt(socket, level, option_name, option_value, option_len);
}


inline int setsockopt(int socket, int level, int option_name, const void* option_value, socklen_t option_len){
	CHECK_OPENED_SOCKET("during setsockopt()", socket);
	return ::setsockopt(socket, level, option_name, option_value, option_len);
}


inline int listen(int socket, int backlog) {
	CHECK_OPENED_SOCKET("during listen()", socket);
	return ::listen(socket, backlog);
}


inline int accept(int socket, struct sockaddr* address, socklen_t* address_len) {
	CHECK_OPENED_SOCKET("during accept()", socket);
	int new_socket = ::accept(socket, address, address_len);
	CHECK_OPEN_SOCKET(new_socket);
	return new_socket;
}


inline int bind(int socket, const struct sockaddr *address, socklen_t address_len) {
	CHECK_OPENED_SOCKET("during bind()", socket);
	return ::bind(socket, address, address_len);
}


inline int connect(int socket, const struct sockaddr* address, socklen_t address_len) {
	CHECK_OPENED_SOCKET("during connect()", socket);
	return ::connect(socket, address, address_len);
}


const char* strerrno(int errnum);

ssize_t write(int fd, const void* buf, size_t nbyte);
ssize_t pwrite(int fd, const void* buf, size_t nbyte, off_t offset);

ssize_t read(int fd, void* buf, size_t nbyte);
ssize_t pread(int fd, void* buf, size_t nbyte, off_t offset);

int fsync(int fd);
int full_fsync(int fd);


#ifdef HAVE_FALLOCATE
inline int fallocate(int fd, int mode, off_t offset, off_t len) {
	CHECK_OPENED("during fallocate()", fd);
	return ::fallocate(fd, mode, offset, len);
}
#else
int fallocate(int fd, int mode, off_t offset, off_t len);
#endif


#ifdef HAVE_POSIX_FADVISE
inline int fadvise(int fd, off_t offset, off_t len, int advice) {
	CHECK_OPENED("during fadvise()", fd);
	return posix_fadvise(fd, offset, len, advice) == 0;
}
#else
#define POSIX_FADV_NORMAL     0
#define POSIX_FADV_SEQUENTIAL 1
#define POSIX_FADV_RANDOM     2
#define POSIX_FADV_WILLNEED   3
#define POSIX_FADV_DONTNEED   4
#define POSIX_FADV_NOREUSE    5


inline int fadvise(int fd, off_t, off_t, int) {
	CHECK_OPENED("during fadvise()", fd);
	ignore_unused(fd);
	return 0;
}
#endif

} /* namespace io */

#endif // IO_UTILS_H
