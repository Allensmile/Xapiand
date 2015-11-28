/*
 * Copyright (C) 2015 deipi.com LLC and contributors. All rights reserved.
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

#include "log.h"
#include "utils.h"

std::mutex log_mutex;
slist<std::shared_ptr<Log>> log_list;
std::unique_ptr<ThreadLog> log_thread(ThreadLog::create());


Log::Log(int timeout, const std::string& str) :
	str_start(str),
	epoch_end(epoch::now<std::chrono::milliseconds>() + timeout),
	finished(false)
{}


std::string
Log::str_format(const char *file, int line, const char *suffix, const char *prefix, void *, const char *format, va_list argptr) {
	char buffer[1024 * 1024];
	snprintf(buffer, sizeof(buffer), "tid(%s): ../%s:%d: %s", get_thread_name().c_str(), file, line, prefix);
	size_t buffer_len = strlen(buffer);
	vsnprintf(&buffer[buffer_len], sizeof(buffer) - buffer_len, format, argptr);
	buffer_len = strlen(buffer);
	snprintf(&buffer[buffer_len], sizeof(buffer) - buffer_len, "%s", suffix);
	return buffer;

}


std::shared_ptr<Log>
Log::timed(const char *file, int line, int timeout, const char *suffix, const char *prefix, void *obj, const char *format, ...)
{
	// std::make_shared only can call a public constructor, for this reason
	// it is neccesary wrap the constructor in a struct.
	struct enable_make_shared : Log {
		enable_make_shared(int timeout, const std::string& str) : Log(timeout, str) {}
	};

	va_list argptr;
	va_start(argptr, format);
	std::string str(str_format(file, line, suffix, prefix, obj, format, argptr));
	va_end(argptr);

	std::shared_ptr<Log> l_ptr;
	if (timeout) {
		l_ptr = std::make_shared<enable_make_shared>(timeout, str);
		log_list.push_front(l_ptr->shared_from_this());
	} else {
		std::lock_guard<std::mutex> lk(log_mutex);
		std::cerr << str << std::endl;
	}
	return l_ptr;
}


void
Log::log(const char *file, int line, const char *suffix, const char *prefix, void *obj, const char *format, ...)
{
	va_list argptr;
	va_start(argptr, format);
	std::string str(str_format(file, line, suffix, prefix, obj, format, argptr));
	va_end(argptr);

	std::lock_guard<std::mutex> lk(log_mutex);
	std::cerr << str << std::endl;
}


void
ThreadLog::thread_function() const
{
	while (running.load()) {
		auto now = epoch::now<std::chrono::milliseconds>();
		for (auto it = log_list.begin(); it != log_list.end(); ) {
			if ((*it)->finished) {
				it = log_list.erase(it);
			} else if (now > (*it)->epoch_end) {
				(*it)->finished = true;
				std::lock_guard<std::mutex> lk(log_mutex);
				std::cerr << (*it)->str_start;
				it = log_list.erase(it);
			} else {
				++it;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}
