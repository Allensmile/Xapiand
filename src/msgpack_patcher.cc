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

#include "msgpack_patcher.h"

#include "exception.h"
#include "log.h"
#include "utils.h"


#define PATCH_ADD "add"
#define PATCH_REM "remove"
#define PATCH_REP "replace"
#define PATCH_MOV "move"
#define PATCH_COP "copy"
#define PATCH_TES "test"
#define PATCH_INC "incr"
#define PATCH_DEC "decr"

#define PATCH_PATH "path"
#define PATCH_FROM "from"


void apply_patch(const MsgPack& patch, MsgPack& object) {
	if (patch.obj->type == msgpack::type::ARRAY) {
		for (auto elem : patch) {
			try {
				MsgPack op = elem.at("op");
				std::string op_str = op.get_str();

				if      (op_str.compare(PATCH_ADD) == 0) { patch_add(elem, object);          }
				else if (op_str.compare(PATCH_REM) == 0) { patch_remove(elem, object);       }
				else if (op_str.compare(PATCH_REP) == 0) { patch_replace(elem, object);      }
				else if (op_str.compare(PATCH_MOV) == 0) { patch_move(elem, object);         }
				else if (op_str.compare(PATCH_COP) == 0) { patch_copy(elem, object);         }
				else if (op_str.compare(PATCH_TES) == 0) { patch_test(elem, object);         }
				else if (op_str.compare(PATCH_INC) == 0) { patch_incr_decr(elem, object);    }
				else if (op_str.compare(PATCH_DEC) == 0) { patch_incr_decr(elem, object, 1); }
			} catch (const std::out_of_range&) {
				throw MSG_ClientError("Objects MUST have exactly one \"op\" member");
			}
		}
	} else {
		throw MSG_ClientError("A JSON Patch document MUST be an array of objects");
	}
}


void patch_add(const MsgPack& obj_patch, MsgPack& object) {
	try {
		std::vector<std::string> path_split;
		_tokenizer(obj_patch, path_split, PATCH_PATH);
		std::string target = path_split.back();
		path_split.pop_back();
		MsgPack o = object.path(path_split);
		MsgPack val = get_patch_value(obj_patch);
		_add(o, val, target);
	} catch (const ClientError& e) {
		throw MSG_ClientError("In patch add: %s", e.what());
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("In patch add: Inconsistent data");
	} catch (const std::exception& e) {
		throw MSG_Error("In patch add: %s", e.what());
	}
}


void patch_remove(const MsgPack& obj_patch, MsgPack& object) {
	try {
		std::vector<std::string> path_split;
		_tokenizer(obj_patch, path_split, PATCH_PATH);
		MsgPack o = object.path(path_split);
		_erase(o.parent(), path_split.back());
	} catch (const ClientError& e) {
		throw MSG_ClientError("In patch remove: %s", e.what());
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("In patch remove: Inconsistent data");
	} catch (const std::exception& e) {
		throw MSG_Error("In patch remove: %s", e.what());
	}
}


void patch_replace(const MsgPack& obj_patch, MsgPack& object) {
	try {
		std::vector<std::string> path_split;
		_tokenizer(obj_patch, path_split, PATCH_PATH);
		MsgPack o = object.path(path_split);
		MsgPack val = get_patch_value(obj_patch);
		o = val;
	} catch (const ClientError& e) {
		throw MSG_ClientError("In patch replace: %s", e.what());
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("In patch replace: Inconsistent data");
	} catch (const std::exception& e) {
		throw MSG_Error("In patch replace: %s", e.what());
	}
}


void patch_move(const MsgPack& obj_patch, MsgPack& object) {
	try {
		std::vector<std::string> path_split;
		std::vector<std::string> from_split;
		_tokenizer(obj_patch, path_split, PATCH_PATH);
		_tokenizer(obj_patch, from_split, PATCH_FROM);
		std::string target = path_split.back();
		path_split.pop_back();
		MsgPack to = object.path(path_split);
		MsgPack from = object.path(from_split);
		_add(to, from, target);
		_erase(from.parent(), from_split.back());
	} catch (const ClientError& e) {
		throw MSG_ClientError("In patch move: %s", e.what());
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("In patch move: Inconsistent data");
	} catch (const std::exception& e) {
		throw MSG_Error("In patch move: %s", e.what());
	}
}


void patch_copy(const MsgPack& obj_patch, MsgPack& object) {
	try {
		std::vector<std::string> path_split;
		std::vector<std::string> from_split;
		_tokenizer(obj_patch, path_split, PATCH_PATH);
		_tokenizer(obj_patch, from_split, PATCH_FROM);
		std::string target = path_split.back();
		path_split.pop_back();
		MsgPack to = object.path(path_split);
		MsgPack from = object.path(from_split);
		_add(to, from, target);
	} catch (const ClientError& e) {
		throw MSG_ClientError("In patch copy: %s", e.what());
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("In patch copy: Inconsistent data");
	} catch (const std::exception& e) {
		throw MSG_Error("In patch copy: %s", e.what());
	}
}


void patch_test(const MsgPack& obj_patch, MsgPack& object) {
	try {
		std::vector<std::string> path_split;
		_tokenizer(obj_patch, path_split, PATCH_PATH);
		MsgPack o = object.path(path_split);
		MsgPack val = get_patch_value(obj_patch);
		if (val != o) {
			throw MSG_ClientError("In patch test: Objects are not equals");
		}
	} catch (const ClientError& e) {
		throw MSG_ClientError("In patch test: %s", e.what());
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("In patch test: Inconsistent data");
	} catch (const std::exception& e) {
		throw MSG_Error("In patch test: %s", e.what());
	}
}


void patch_incr_decr(const MsgPack& obj_patch, MsgPack& object, bool decr) {
	try {
		int limit;
		std::vector<std::string> path_split;
		_tokenizer(obj_patch, path_split, PATCH_PATH);
		MsgPack o = object.path(path_split);
		MsgPack val = get_patch_value(obj_patch);
		int val_num = strict_stoi(std::string(val.obj->via.str.ptr, val.obj->via.str.size));
		if(get_patch_custom_limit(limit, obj_patch)) {
			_incr_decr(o, decr ? -val_num : val_num, limit);
		} else {
			_incr_decr(o, decr ? -val_num : val_num, val_num);
		}
	} catch (const LimitError& e){
		throw MSG_ClientError("In patch increment: %s\n", e.what());
	} catch (const ClientError& e) {
		throw MSG_ClientError("In patch increment: %s", e.what());
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("In patch increment: Inconsistent data");
	} catch (const std::exception& e) {
		throw MSG_Error("In patch increment: %s", e.what());
	}
}


MsgPack get_patch_value(const MsgPack& obj_patch) {
	try {
		return obj_patch.at("value");
	} catch (const std::out_of_range&) {
		throw MSG_ClientError("Object MUST have exactly one \"value\" member for this operation");
	}
}


bool get_patch_custom_limit(int& limit, const MsgPack& obj_patch) {
	try {
		MsgPack o = obj_patch.at("limit");
		if (o.obj->type == msgpack::type::STR) {
			limit = strict_stoi(std::string(o.obj->via.str.ptr, o.obj->via.str.size));
			return true;
		} else {
			throw MSG_ClientError("\"limit\" must be string");
		}
	} catch (const std::out_of_range&) {
		return false;
	}
}
