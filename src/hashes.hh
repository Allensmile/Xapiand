/*
 * Copyright (C) 2015-2018 Dubalu LLC. All rights reserved.
 * Copyright (c) 2015 Daniel Kirchner
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

#ifndef HASHES_HH
#define HASHES_HH

#include <cstdint>
#include <string>
#include <string_view>        // for std::string_view

#include "lz4/xxhash.h"
#include "static_string.hh"


/*               _               _
 * __  ____  __ | |__   __ _ ___| |__
 * \ \/ /\ \/ / | '_ \ / _` / __| '_ \
 *  >  <  >  <  | | | | (_| \__ \ | | |
 * /_/\_\/_/\_\ |_| |_|\__,_|___/_| |_|
 */
class xxh64 {
	constexpr static std::uint64_t PRIME1 = 11400714785074694791ULL;
	constexpr static std::uint64_t PRIME2 = 14029467366897019727ULL;
	constexpr static std::uint64_t PRIME3 =  1609587929392839161ULL;
	constexpr static std::uint64_t PRIME4 =  9650029242287828579ULL;
	constexpr static std::uint64_t PRIME5 =  2870177450012600261ULL;

	constexpr static std::uint64_t rotl(std::uint64_t x, int r) {
		return ((x << r) | (x >> (64 - r)));
	}
	constexpr static std::uint64_t mix1(const std::uint64_t h, const std::uint64_t prime, int rshift) {
		return (h ^ (h >> rshift)) * prime;
	}
	constexpr static std::uint64_t mix2(const std::uint64_t p, const std::uint64_t v = 0) {
		return rotl (v + p * PRIME2, 31) * PRIME1;
	}
	constexpr static std::uint64_t mix3(const std::uint64_t h, const std::uint64_t v) {
		return (h ^ mix2 (v)) * PRIME1 + PRIME4;
	}
#ifdef XXH64_BIG_ENDIAN
	constexpr static std::uint32_t endian32(const char *v) {
		return std::uint32_t(std::uint8_t(v[3]))|(std::uint32_t(std::uint8_t(v[2]))<<8)
			   |(std::uint32_t(std::uint8_t(v[1]))<<16)|(std::uint32_t(std::uint8_t(v[0]))<<24);
	}
	constexpr static std::uint64_t endian64(const char *v) {
		return std::uint64_t(std::uint8_t(v[7]))|(std::uint64_t(std::uint8_t(v[6]))<<8)
			   |(std::uint64_t(std::uint8_t(v[5]))<<16)|(std::uint64_t(std::uint8_t(v[4]))<<24)
			   |(std::uint64_t(std::uint8_t(v[3]))<<32)|(std::uint64_t(std::uint8_t(v[2]))<<40)
			   |(std::uint64_t(std::uint8_t(v[1]))<<48)|(std::uint64_t(std::uint8_t(v[0]))<<56);
	}
#else
	constexpr static std::uint32_t endian32(const char *v) {
		return std::uint32_t(std::uint8_t(v[0]))|(std::uint32_t(std::uint8_t(v[1]))<<8)
			   |(std::uint32_t(std::uint8_t(v[2]))<<16)|(std::uint32_t(std::uint8_t(v[3]))<<24);
	}
	constexpr static std::uint64_t endian64 (const char *v) {
		return std::uint64_t(std::uint8_t(v[0]))|(std::uint64_t(std::uint8_t(v[1]))<<8)
			   |(std::uint64_t(std::uint8_t(v[2]))<<16)|(std::uint64_t(std::uint8_t(v[3]))<<24)
			   |(std::uint64_t(std::uint8_t(v[4]))<<32)|(std::uint64_t(std::uint8_t(v[5]))<<40)
			   |(std::uint64_t(std::uint8_t(v[6]))<<48)|(std::uint64_t(std::uint8_t(v[7]))<<56);
	}
#endif
	constexpr static std::uint64_t fetch64(const char *p, const std::uint64_t v = 0) {
		return mix2 (endian64 (p), v);
	}
	constexpr static std::uint64_t fetch32(const char *p) {
		return std::uint64_t (endian32 (p)) * PRIME1;
	}
	constexpr static std::uint64_t fetch8(const char *p) {
		return std::uint8_t (*p) * PRIME5;
	}
	constexpr static std::uint64_t finalize(const std::uint64_t h, const char *p, std::uint64_t len) {
		return (len >= 8) ? (finalize (rotl (h ^ fetch64 (p), 27) * PRIME1 + PRIME4, p + 8, len - 8)) :
			   ((len >= 4) ? (finalize (rotl (h ^ fetch32 (p), 23) * PRIME2 + PRIME3, p + 4, len - 4)) :
				((len > 0) ? (finalize (rotl (h ^ fetch8 (p), 11) * PRIME1, p + 1, len - 1)) :
				 (mix1 (mix1 (mix1 (h, PRIME2, 33), PRIME3, 29), 1, 32))));
	}
	constexpr static std::uint64_t h32bytes(const char *p, std::uint64_t len, const std::uint64_t v1,const std::uint64_t v2, const std::uint64_t v3, const std::uint64_t v4) {
		return (len >= 32) ? h32bytes (p + 32, len - 32, fetch64 (p, v1), fetch64 (p + 8, v2), fetch64 (p + 16, v3), fetch64 (p + 24, v4)) :
			   mix3 (mix3 (mix3 (mix3 (rotl (v1, 1) + rotl (v2, 7) + rotl (v3, 12) + rotl (v4, 18), v1), v2), v3), v4);
	}
	constexpr static std::uint64_t h32bytes(const char *p, std::uint64_t len, const std::uint64_t seed) {
		return h32bytes (p, len, seed + PRIME1 + PRIME2, seed + PRIME2, seed, seed - PRIME1);
	}

public:
	using key_type = std::uint64_t;

	constexpr static std::uint64_t hash(const char *p, std::uint64_t len, std::uint64_t seed = 0) {
		return finalize((len >= 32 ? h32bytes(p, len, seed) : seed + PRIME5) + len, p + (len & ~0x1F), len & 0x1F);
	}

	template <size_t N>
	constexpr static std::uint64_t hash(const char(&s)[N], std::uint64_t seed = 0) {
		return hash(s, N - 1, seed);
	}

	template <std::size_t SN, typename ST>
	constexpr static std::uint64_t hash(const static_string::static_string<SN, ST>& str, std::uint64_t seed = 0) {
		return hash(str.data(), str.size(), seed);
	}

	static std::uint64_t hash(std::string_view str, std::uint64_t seed = 0) {
		return XXH64(str.data(), str.size(), seed);
	}

	static std::uint64_t hash(const std::string& str, std::uint64_t seed = 0) {
		return XXH64(str.data(), str.size(), seed);
	}

	template <typename... Args>
	constexpr auto operator()(Args&&... args) const {
		return hash(std::forward<Args>(args)...);
	}
};


class xxh32 {
	/* constexpr xxh32::hash() not implemented! */

public:
	using key_type = std::uint32_t;

	template <std::size_t SN, typename ST>
	constexpr static std::uint32_t hash(const static_string::static_string<SN, ST>& str, std::uint32_t seed = 0) {
		return hash(str.data(), str.size(), seed);
	}

	static std::uint32_t hash(std::string_view str, std::uint32_t seed = 0) {
		return XXH32(str.data(), str.size(), seed);
	}

	static std::uint32_t hash(const std::string& str, std::uint32_t seed = 0) {
		return XXH32(str.data(), str.size(), seed);
	}

	template <typename... Args>
	constexpr auto operator()(Args&&... args) const {
		return hash(std::forward<Args>(args)...);
	}
};

constexpr std::uint64_t operator"" _xx(const char* s, size_t size) {
	return xxh64::hash(s, size);
}

constexpr static char noop(char c) {
	return c;
}

/*   __            _         _               _
 *  / _|_ ____   _/ | __ _  | |__   __ _ ___| |__
 * | |_| '_ \ \ / / |/ _` | | '_ \ / _` / __| '_ \
 * |  _| | | \ V /| | (_| | | | | | (_| \__ \ | | |
 * |_| |_| |_|\_/ |_|\__,_| |_| |_|\__,_|___/_| |_|
 */
template <typename T, T prime, T offset, typename F>
struct fnv1ah {
	using key_type = T;

	constexpr static T hash(const char *p, std::size_t len, T seed = offset, F op = noop) {
		T hash = seed;
		for (std::size_t i = 0; i < len; ++i) {
			hash = (hash ^ static_cast<unsigned char>(op(p[i]))) * prime;
		}
		return hash;
	}
	constexpr static T hash(const char *p, std::size_t len, F op) {
		return hash(p, len, offset, op);
	}

	template <size_t N>
	constexpr static T hash(const char(&s)[N], T seed = offset, F op = noop) {
		return hash(s, N - 1, seed, noop);
	}
	template <size_t N>
	constexpr static T hash(const char(&s)[N], F op) {
		return hash(s, N - 1, offset, noop);
	}

	template <std::size_t SN, typename ST>
	constexpr static T hash(const static_string::static_string<SN, ST>& str, T seed = offset, F op = noop) {
		return hash(str.data(), str.size(), seed, noop);
	}
	template <std::size_t SN, typename ST>
	constexpr static T hash(const static_string::static_string<SN, ST>& str, F op) {
		return hash(str.data(), str.size(), offset, noop);
	}

	constexpr static T hash(std::string_view str, T seed = offset, F op = noop) {
		return hash(str.data(), str.size(), seed, noop);
	}
	constexpr static T hash(std::string_view str, F op) {
		return hash(str.data(), str.size(), offset, noop);
	}

	static T hash(const std::string& str, T seed = offset, F op = noop) {
		return hash(str.data(), str.size(), seed, noop);
	}
	static T hash(const std::string& str, F op) {
		return hash(str.data(), str.size(), offset, noop);
	}

	template <typename... Args>
	constexpr auto operator()(Args&&... args) const {
		return hash(std::forward<Args>(args)...);
	}
};
using fnv1ah16 = fnv1ah<std::uint16_t, 0x21dU, 51363UL, decltype(noop)>;  // shouldn't exist, figured out the prime and offset
using fnv1ah32 = fnv1ah<std::uint32_t, 0x1000193UL, 2166136261UL, decltype(noop)>;
using fnv1ah64 = fnv1ah<std::uint64_t, 0x100000001b3ULL, 14695981039346656037ULL, decltype(noop)>;
// using fnv1ah128 = fnv1ah<__uint128_t, 0x10000000000000000000159ULLL, 275519064689413815358837431229664493455ULLL>;  // too big for compiler

constexpr std::uint32_t operator"" _fnv1a(const char* s, size_t size) {
	return fnv1ah64::hash(s, size);
}


/*      _  _ _    ____    _               _
 *   __| |(_) |__|___ \  | |__   __ _ ___| |__
 *  / _` || | '_ \ __) | | '_ \ / _` / __| '_ \
 * | (_| || | |_) / __/  | | | | (_| \__ \ | | |
 *  \__,_|/ |_.__/_____| |_| |_|\__,_|___/_| |_|
 *      |__/
 */
template <typename T, T mul, T offset, typename F>
struct djb2h {
	using key_type = T;

	constexpr static T hash(const char *p, std::size_t len, T seed = offset, F op = noop) {
		T hash = seed;
		for (std::size_t i = 0; i < len; ++i) {
			hash = (hash * mul) + static_cast<unsigned char>(op(p[i]));
		}
		return hash;
	}
	constexpr static T hash(const char *p, std::size_t len, F op) {
		return hash(p, len, offset, op);
	}

	template <size_t N>
	constexpr static T hash(const char(&s)[N], T seed = offset, F op = noop) {
		return hash(s, N - 1, seed, noop);
	}
	template <size_t N>
	constexpr static T hash(const char(&s)[N], F op) {
		return hash(s, N - 1, offset, noop);
	}

	template <std::size_t SN, typename ST>
	constexpr static T hash(const static_string::static_string<SN, ST>& str, T seed = offset, F op = noop) {
		return hash(str.data(), str.size(), seed, noop);
	}
	template <std::size_t SN, typename ST>
	constexpr static T hash(const static_string::static_string<SN, ST>& str, F op) {
		return hash(str.data(), str.size(), offset, noop);
	}

	constexpr static T hash(std::string_view str, T seed = offset, F op = noop) {
		return hash(str.data(), str.size(), seed, noop);
	}
	constexpr static T hash(std::string_view str, F op) {
		return hash(str.data(), str.size(), offset, noop);
	}

	static T hash(const std::string& str, T seed = offset, F op = noop) {
		return hash(str.data(), str.size(), seed, noop);
	}
	static T hash(const std::string& str, F op) {
		return hash(str.data(), str.size(), offset, noop);
	}

	template <typename... Args>
	constexpr auto operator()(Args&&... args) const {
		return hash(std::forward<Args>(args)...);
	}
};

// from https://stackoverflow.com/a/41849998/167522, used primeth with bits size
using djb2h8 = djb2h<std::uint8_t, 7, 5, decltype(noop)>;  // (h << 3) - h <-- mul should? be prime 7 or 11
using djb2h16 = djb2h<std::uint16_t, 13, 31, decltype(noop)>;  // (h << 2) + (h << 3) + h <-- mul should? be prime 13 or 17
using djb2h32 = djb2h<std::uint32_t, 33, 5381, decltype(noop)>;  // the one implemented everywhere: (h << 5) + h <-- mul should? be prime 31 or 37
using djb2h64 = djb2h<std::uint64_t, 63, 174440041L, decltype(noop)>;  // (h << 6) - h <-- mul should? be prime 61 or 67

#endif // HASHES_HH
