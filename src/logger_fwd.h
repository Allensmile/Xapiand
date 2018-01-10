/*
 * Copyright (C) 2015-2018 deipi.com LLC and contributors. All rights reserved.
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

#include <atomic>             // for std::atomic
#include <chrono>             // for system_clock, time_point, duration, millise...
#include <cstdarg>            // for va_list, va_end
#include <syslog.h>           // for LOG_DEBUG, LOG_WARNING, LOG_CRIT, LOG_ALERT

#include "exception.h"        // for BaseException
#include "utils.h"
#include "xxh64.hpp"          // for xxh64

#define ASYNC_LOG_LEVEL LOG_ERR  // The minimum log_level that is asynchronous

extern std::atomic<uint64_t> logger_info_hook;


using namespace std::chrono_literals;


class Logging;
using LogType = std::shared_ptr<Logging>;


class Log {
	LogType log;

	Log(const Log&) = delete;
	Log& operator=(const Log&) = delete;

	bool _unlog(int priority, const char* file, int line, const char *suffix, const char *prefix, const char *format, ...);

public:
	Log(Log&& o);
	Log& operator=(Log&& o);

	explicit Log(LogType log_);
	~Log();

	template <typename S, typename P, typename F, typename... Args>
	bool unlog(int priority, const char* file, int line, S&& suffix, P&& prefix, F&& format, Args&&... args) {
		return _unlog(priority, file, line, cstr(std::forward<S>(suffix)), cstr(std::forward<P>(prefix)), cstr(std::forward<F>(format)), std::forward<Args>(args)...);
	}
	bool vunlog(int priority, const char *file, int line, const char *suffix, const char *prefix, const char *format, va_list argptr);

	bool clear();
	long double age();
	LogType release();
};


void vprintln(bool collect, bool with_endl, const char* format, va_list argptr);
void _println(bool collect, bool with_endl, const char* format, ...);


template <typename F, typename... Args>
static void print(F&& format, Args&&... args) {
	return _println(false, true, cstr(std::forward<F>(format)), std::forward<Args>(args)...);
}


template <typename F, typename... Args>
static void collect(F&& format, Args&&... args) {
	return _println(true, true, cstr(std::forward<F>(format)), std::forward<Args>(args)...);
}


Log vlog(bool cleanup, bool info, bool stacked, std::chrono::time_point<std::chrono::system_clock> wakeup, bool async, int priority, const std::string& exc, const char *file, int line, const char *suffix, const char *prefix, const char *format, va_list argptr);
Log _log(bool cleanup, bool info, bool stacked, std::chrono::time_point<std::chrono::system_clock> wakeup, bool async, int priority, const std::string& exc, const char *file, int line, const char *suffix, const char *prefix, const char *format, ...);


template <typename T, typename S, typename P, typename F, typename... Args, typename = std::enable_if_t<std::is_base_of<BaseException, std::decay_t<T>>::value>>
inline Log log(bool cleanup, bool info, bool stacked, std::chrono::time_point<std::chrono::system_clock> wakeup, bool async, int priority, const T* exc, const char *file, int line, S&& suffix, P&& prefix, F&& format, Args&&... args) {
	return _log(cleanup, info, stacked, wakeup, async, priority, std::string(exc->get_traceback()), file, line, cstr(std::forward<S>(suffix)), cstr(std::forward<P>(prefix)), cstr(std::forward<F>(format)), std::forward<Args>(args)...);
}


template <typename S, typename P, typename F, typename... Args>
inline Log log(bool cleanup, bool info, bool stacked, std::chrono::time_point<std::chrono::system_clock> wakeup, bool async, int priority, const void*, const char *file, int line, S&& suffix, P&& prefix, F&& format, Args&&... args) {
	return _log(cleanup, info, stacked, wakeup, async, priority, std::string(), file, line, cstr(std::forward<S>(suffix)), cstr(std::forward<P>(prefix)), cstr(std::forward<F>(format)), std::forward<Args>(args)...);
}


template <typename T, typename R, typename... Args>
inline Log log(bool cleanup, bool info, bool stacked, std::chrono::duration<T, R> timeout, bool async, int priority, Args&&... args) {
	return log(cleanup, info, stacked, std::chrono::system_clock::now() + timeout, async, priority, std::forward<Args>(args)...);
}


template <typename... Args>
inline Log log(bool cleanup, bool info, bool stacked, int timeout, bool async, int priority, Args&&... args) {
	return log(cleanup, info, stacked, std::chrono::milliseconds(timeout), async, priority, std::forward<Args>(args)...);
}

#define MERGE_(a,b)  a##b
#define LABEL_(a) MERGE_(__unique, a)
#define UNIQUE_NAME LABEL_(__LINE__)

#define L_DELAYED(cleanup, delay, priority, color, args...) ::log(cleanup, true, false, delay, true, priority, nullptr, __FILE__, __LINE__, NO_COL, color, args)
#define L_DELAYED_UNLOG(priority, color, args...) unlog(priority, __FILE__, __LINE__, NO_COL, color, args)
#define L_DELAYED_CLEAR() clear()

#define L_DELAYED_200(args...) auto __log_timed = L_DELAYED(true, 200ms, LOG_WARNING, LIGHT_PURPLE, args)
#define L_DELAYED_600(args...) auto __log_timed = L_DELAYED(true, 600ms, LOG_WARNING, LIGHT_PURPLE, args)
#define L_DELAYED_1000(args...) auto __log_timed = L_DELAYED(true, 1000ms, LOG_WARNING, LIGHT_PURPLE, args)
#define L_DELAYED_N_UNLOG(args...) __log_timed.L_DELAYED_UNLOG(LOG_WARNING, PURPLE, args)
#define L_DELAYED_N_CLEAR() __log_timed.L_DELAYED_CLEAR()

#define L_NOTHING(args...)

#define LOG(stacked, level, color, args...) ::log(false, true, stacked, 0ms, level >= ASYNC_LOG_LEVEL, level, nullptr, __FILE__, __LINE__, NO_COL, color, args)

#define L_INFO(args...) LOG(true, LOG_INFO, INFO_COL, args)
#define L_NOTICE(args...) LOG(true, LOG_NOTICE, NOTICE_COL, args)
#define L_WARNING(args...) LOG(true, LOG_WARNING, WARNING_COL, args)
#define L_ERR(args...) LOG(true, LOG_ERR, ERR_COL, args)
#define L_CRIT(args...) LOG(true, LOG_CRIT, CRIT_COL, args)
#define L_ALERT(args...) LOG(true, -LOG_ALERT, ALERT_COL, args)
#define L_EMERG(args...) LOG(true, -LOG_EMERG, EMERG_COL, args)
#define L_EXC(args...) ::log(false, true, true, 0ms, true, -LOG_CRIT, &exc, __FILE__, __LINE__, NO_COL, ERR_COL, args)

#define L_UNINDENTED(level, color, args...) LOG(false, level, color, args)
#define L_UNINDENTED_LOG(args...) L_UNINDENTED(LOG_DEBUG, LOG_COL, args)

#define L(level, color, args...) LOG(true, level, color, args)
#define L_LOG(args...) L(LOG_DEBUG, LOG_COL, args)

#define L_STACKED(args...) auto UNIQUE_NAME = L(args)
#define L_STACKED_LOG(args...) L_STACKED(LOG_DEBUG, LOG_COL, args)

#define L_COLLECT(args...) ::collect(args)

#define L_PRINT(args...) ::print(args)

#ifdef NDEBUG
#define L_DEBUG L_NOTHING
#define L_INFO_HOOK L_NOTHING
#else
#define L_DEBUG(args...) L(LOG_DEBUG, DEBUG_COL, args)
#define L_INFO_HOOK(hook, args...) if ((logger_info_hook.load() & xxh64::hash(hook)) == xxh64::hash(hook)) { L_PRINT(args); }
#endif
#define L_INFO_HOOK_LOG(hook, args...) L_INFO_HOOK(hook, args)
