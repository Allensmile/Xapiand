/*
 * Copyright (C) 2017 deipi.com LLC and contributors. All rights reserved.
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


#if defined(XAPIAND_CHAISCRIPT) || defined(XAPIAND_V8)

#include <string>
#include <unordered_map>

#include "exception.h"
#include "schema.h"


class ScriptNotFoundError : public ClientError {
public:
	template<typename... Args>
	ScriptNotFoundError(Args&&... args) : ClientError(std::forward<Args>(args)...) { }
};


class Script {
	using dispatch_func = void (Script::*)(const MsgPack&);
	static const std::unordered_map<std::string, dispatch_func> map_dispatch_script;
	static const std::unordered_map<std::string, dispatch_func> map_dispatch_value;

	enum class Type : uint8_t {
		EMPTY,
		CHAI,
		ECMA,
	};

	void process_body(const MsgPack& _body);
	void process_name(const MsgPack& _name);
	void process_type(const MsgPack& _type);
	void process_value(const MsgPack& _value);
	void process_chai(const MsgPack& _chai);
	void process_ecma(const MsgPack& _ecma);

	std::string body;
	std::string name;
	Type type;
	bool with_value;
	bool with_data;
	std::array<FieldType, SPC_TOTAL_TYPES> sep_types;

public:
	Script(const MsgPack& _obj);

	MsgPack process_chai(bool strict);
	MsgPack process_ecma(bool strict);
	MsgPack process_script(bool strict);
};

#endif
