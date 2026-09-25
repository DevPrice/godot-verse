#include "vm_sidecar.h"

#include <charconv>

#include "vm_json.h"

namespace vm {

namespace {

// vh_type's numbering, which the file carries (include/verse_host_abi.h).
enum : int32_t {
	kTypeVoid = 0,
	kTypeLogic = 1,
	kTypeInt = 2,
	kTypeFloat = 3,
	kTypeChar = 4,
	kTypeString = 5,
	kTypeArray = 6,
	kTypeMap = 7,
	kTypeTuple = 8,
	kTypeOption = 9,
};

class SidecarReader {
public:
	SidecarReader(const std::string &p_path, std::string &r_error) :
			path(p_path), error(r_error) {}

	bool read(const JsonValue &p_root, Sidecar &r_sidecar) {
		if (!p_root.is_object()) {
			return fail("the top level is not an object");
		}
		const JsonValue *version = p_root.find("version");
		if (version == nullptr || !json_to_int64(*version, r_sidecar.version)) {
			return fail("it carries no integer `version`");
		}
		if (r_sidecar.version != kSidecarVersion) {
			error = path + " was written by sidecar version " + std::to_string(r_sidecar.version) + "; this host reads version " + std::to_string(kSidecarVersion) + ". Re-export the project.";
			return false;
		}
		if (!get_int64(p_root, "abi", r_sidecar.abi, "the top level") || !get_string(p_root, "hostId", r_sidecar.host_id, "the top level") ||
				!get_string(p_root, "engineCommit", r_sidecar.engine_commit, "the top level") ||
				!get_int64(p_root, "generation", r_sidecar.generation, "the top level")) {
			return false;
		}
		const JsonValue *packages = require(p_root, "packages", JsonValue::Kind::Array, "the top level");
		if (packages == nullptr) {
			return false;
		}
		for (const JsonValue &package : packages->items) {
			if (!package.is_string()) {
				return fail("`packages` holds something other than a string");
			}
			r_sidecar.packages.push_back(package.text);
		}
		if (!read_bindings(p_root, r_sidecar) || !read_engine_signals(p_root, r_sidecar)) {
			return false;
		}
		const JsonValue *classes = require(p_root, "classes", JsonValue::Kind::Object, "the top level");
		if (classes == nullptr) {
			return false;
		}
		r_sidecar.classes.resize(classes->items.size());
		for (size_t index = 0; index < classes->items.size(); ++index) {
			SidecarClass &class_entry = r_sidecar.classes[index];
			class_entry.name = classes->keys[index];
			if (!read_class(classes->items[index], class_entry)) {
				return false;
			}
			r_sidecar.classes_by_name.emplace(class_entry.name, index);
		}
		return true;
	}

private:
	const std::string &path;
	std::string &error;

	bool fail(const std::string &p_what) {
		if (error.empty()) {
			error = path + " is not a valid class sidecar: " + p_what + ".";
		}
		return false;
	}

	static const char *kind_name(JsonValue::Kind p_kind) {
		switch (p_kind) {
			case JsonValue::Kind::Null:
				return "null";
			case JsonValue::Kind::Bool:
				return "a bool";
			case JsonValue::Kind::Number:
				return "a number";
			case JsonValue::Kind::String:
				return "a string";
			case JsonValue::Kind::Array:
				return "an array";
			case JsonValue::Kind::Object:
				return "an object";
		}
		return "?";
	}

	const JsonValue *require(const JsonValue &p_object, const char *p_key, JsonValue::Kind p_kind, const std::string &p_where) {
		const JsonValue *value = p_object.find(p_key);
		if (value == nullptr) {
			fail(p_where + " has no `" + p_key + "`");
			return nullptr;
		}
		if (value->kind != p_kind) {
			fail("`" + std::string(p_key) + "` in " + p_where + " is not " + kind_name(p_kind));
			return nullptr;
		}
		return value;
	}

	// Absent is not an error; present and of another kind is.
	bool optional(const JsonValue &p_object, const char *p_key, JsonValue::Kind p_kind, const std::string &p_where, const JsonValue *&r_value) {
		r_value = p_object.find(p_key);
		if (r_value != nullptr && r_value->kind != p_kind) {
			return fail("`" + std::string(p_key) + "` in " + p_where + " is not " + kind_name(p_kind));
		}
		return true;
	}

	bool get_string(const JsonValue &p_object, const char *p_key, std::string &r_value, const std::string &p_where) {
		const JsonValue *value = require(p_object, p_key, JsonValue::Kind::String, p_where);
		if (value == nullptr) {
			return false;
		}
		r_value = value->text;
		return true;
	}

	bool get_bool(const JsonValue &p_object, const char *p_key, bool &r_value, const std::string &p_where) {
		const JsonValue *value = require(p_object, p_key, JsonValue::Kind::Bool, p_where);
		if (value == nullptr) {
			return false;
		}
		r_value = value->boolean;
		return true;
	}

	bool get_number(const JsonValue &p_object, const char *p_key, double &r_value, const std::string &p_where) {
		const JsonValue *value = require(p_object, p_key, JsonValue::Kind::Number, p_where);
		if (value == nullptr) {
			return false;
		}
		r_value = value->number;
		return true;
	}

	bool get_int64(const JsonValue &p_object, const char *p_key, int64_t &r_value, const std::string &p_where) {
		const JsonValue *value = require(p_object, p_key, JsonValue::Kind::Number, p_where);
		if (value == nullptr) {
			return false;
		}
		if (!json_to_int64(*value, r_value)) {
			return fail("`" + std::string(p_key) + "` in " + p_where + " is not an integer");
		}
		return true;
	}

	bool get_int(const JsonValue &p_object, const char *p_key, int32_t &r_value, const std::string &p_where) {
		int64_t value;
		if (!get_int64(p_object, p_key, value, p_where)) {
			return false;
		}
		if (value < INT32_MIN || value > INT32_MAX) {
			return fail("`" + std::string(p_key) + "` in " + p_where + " is out of range");
		}
		r_value = int32_t(value);
		return true;
	}

	bool read_param(const JsonValue &p_value, SidecarParam &r_param, const std::string &p_where) {
		if (!p_value.is_object()) {
			return fail(p_where + " holds a parameter that is not an object");
		}
		return get_string(p_value, "name", r_param.name, p_where) && get_int(p_value, "type", r_param.type, p_where) &&
				get_int(p_value, "tag", r_param.tag, p_where) && get_bool(p_value, "default", r_param.has_default, p_where) &&
				get_string(p_value, "class", r_param.class_name, p_where) && get_int(p_value, "classKind", r_param.class_kind, p_where);
	}

	bool read_params(const JsonValue &p_object, const char *p_key, std::vector<SidecarParam> &r_params, const std::string &p_where) {
		const JsonValue *params = require(p_object, p_key, JsonValue::Kind::Array, p_where);
		if (params == nullptr) {
			return false;
		}
		r_params.resize(params->items.size());
		for (size_t index = 0; index < params->items.size(); ++index) {
			if (!read_param(params->items[index], r_params[index], p_where)) {
				return false;
			}
		}
		return true;
	}

	bool read_method(const JsonValue &p_value, SidecarMethod &r_method, const std::string &p_where) {
		if (!p_value.is_object()) {
			return fail(p_where + " lists a method that is not an object");
		}
		const std::string where = "method `" + (p_value.find("name") != nullptr ? p_value.find("name")->text : std::string()) + "` of " + p_where;
		return get_string(p_value, "name", r_method.name, where) && get_string(p_value, "decorated", r_method.decorated, where) &&
				read_params(p_value, "params", r_method.params, where) && get_int(p_value, "required", r_method.required, where) &&
				get_int(p_value, "result", r_method.result, where) && get_int(p_value, "resultTag", r_method.result_tag, where) &&
				get_string(p_value, "resultClass", r_method.result_class, where) &&
				get_int(p_value, "resultClassKind", r_method.result_class_kind, where) &&
				get_bool(p_value, "canFail", r_method.can_fail, where) && get_bool(p_value, "suspends", r_method.suspends, where) &&
				get_string(p_value, "virtual", r_method.godot_virtual, where) && get_int(p_value, "line", r_method.line, where) &&
				get_int(p_value, "column", r_method.column, where);
	}

	bool read_signal(const JsonValue &p_value, SidecarSignal &r_signal, const std::string &p_where) {
		if (!p_value.is_object()) {
			return fail(p_where + " lists a signal that is not an object");
		}
		const std::string where = "a signal of " + p_where;
		return get_string(p_value, "name", r_signal.name, where) && read_params(p_value, "args", r_signal.args, where) &&
				get_int(p_value, "line", r_signal.line, where) && get_int(p_value, "column", r_signal.column, where) &&
				get_int(p_value, "reject", r_signal.reject, where) && get_string(p_value, "rejectDetail", r_signal.reject_detail, where);
	}

	bool read_rpc(const JsonValue &p_value, SidecarRpc &r_rpc, const std::string &p_where) {
		if (!p_value.is_object()) {
			return fail(p_where + " lists an rpc that is not an object");
		}
		const std::string where = "an rpc of " + p_where;
		return get_string(p_value, "name", r_rpc.name, where) && get_int(p_value, "mode", r_rpc.mode, where) &&
				get_bool(p_value, "callLocal", r_rpc.call_local, where) && get_int(p_value, "transfer", r_rpc.transfer, where) &&
				get_int(p_value, "channel", r_rpc.channel, where) && get_int(p_value, "line", r_rpc.line, where) &&
				get_int(p_value, "column", r_rpc.column, where) && get_int(p_value, "reject", r_rpc.reject, where) &&
				get_string(p_value, "rejectDetail", r_rpc.reject_detail, where);
	}

	bool read_export(const JsonValue &p_value, SidecarExport &r_export, const std::string &p_where) {
		if (!p_value.is_object()) {
			return fail(p_where + " holds an export description that is not an object");
		}
		return get_string(p_value, "name", r_export.name, p_where) && get_int(p_value, "type", r_export.type, p_where) &&
				get_int(p_value, "tag", r_export.tag, p_where) && get_int(p_value, "elementTag", r_export.element_tag, p_where) &&
				get_bool(p_value, "isVar", r_export.is_var, p_where) && get_int(p_value, "hint", r_export.hint, p_where) &&
				get_string(p_value, "hintString", r_export.hint_string, p_where) &&
				get_string(p_value, "nativeClass", r_export.native_class, p_where) &&
				get_number(p_value, "rangeMin", r_export.range_min, p_where) && get_number(p_value, "rangeMax", r_export.range_max, p_where) &&
				get_bool(p_value, "hasRangeMin", r_export.has_range_min, p_where) &&
				get_bool(p_value, "hasRangeMax", r_export.has_range_max, p_where) &&
				get_int(p_value, "groupKind", r_export.group_kind, p_where) && get_string(p_value, "groupName", r_export.group_name, p_where) &&
				get_int(p_value, "line", r_export.line, p_where) && get_int(p_value, "column", r_export.column, p_where) &&
				get_int(p_value, "reject", r_export.reject, p_where);
	}

	bool read_value(const JsonValue &p_value, SidecarValue &r_value, const std::string &p_where) {
		if (!p_value.is_object()) {
			return fail(p_where + " holds a value that is not an object");
		}
		if (!get_int(p_value, "type", r_value.type, p_where) || !get_int(p_value, "tag", r_value.tag, p_where)) {
			return false;
		}
		const JsonValue *payload = p_value.find("v");
		const auto expect = [&](JsonValue::Kind p_kind) {
			if (payload == nullptr || payload->kind != p_kind) {
				return fail("a value of type " + std::to_string(r_value.type) + " in " + p_where + " carries no " + kind_name(p_kind) + " `v`");
			}
			return true;
		};
		switch (r_value.type) {
			case kTypeLogic:
				if (!expect(JsonValue::Kind::Bool)) {
					return false;
				}
				r_value.logic = payload->boolean;
				return true;
			case kTypeInt: {
				if (!expect(JsonValue::Kind::String)) {
					return false;
				}
				const std::string &digits = payload->text;
				const std::from_chars_result result = std::from_chars(digits.data(), digits.data() + digits.size(), r_value.integer);
				if (digits.empty() || result.ec != std::errc() || result.ptr != digits.data() + digits.size()) {
					return fail("an int value in " + p_where + " is not a decimal int64");
				}
				return true;
			}
			case kTypeFloat:
				if (!expect(JsonValue::Kind::Number)) {
					return false;
				}
				r_value.number = payload->number;
				return true;
			case kTypeChar: {
				int64_t code_point;
				if (!expect(JsonValue::Kind::Number) || !json_to_int64(*payload, code_point) || code_point < 0 || code_point > UINT32_MAX) {
					return fail("a char value in " + p_where + " is not a code point");
				}
				r_value.code_point = uint32_t(code_point);
				return true;
			}
			case kTypeString:
				if (!expect(JsonValue::Kind::String)) {
					return false;
				}
				r_value.text = payload->text;
				return true;
			case kTypeArray:
			case kTypeTuple:
				if (!expect(JsonValue::Kind::Array)) {
					return false;
				}
				r_value.elements.resize(payload->items.size());
				for (size_t index = 0; index < payload->items.size(); ++index) {
					if (!read_value(payload->items[index], r_value.elements[index], p_where)) {
						return false;
					}
				}
				return true;
			case kTypeOption:
				if (payload != nullptr) {
					r_value.elements.resize(1);
					return read_value(*payload, r_value.elements[0], p_where);
				}
				return true;
			default:
				return true;
		}
	}

	bool read_member_type(const JsonValue &p_value, SidecarMemberType &r_type, const std::string &p_where) {
		if (!p_value.is_object()) {
			return fail(p_where + " holds a member type that is not an object");
		}
		const JsonValue *described = require(p_value, "described", JsonValue::Kind::Object, p_where);
		if (described == nullptr || !read_export(*described, r_type.described, p_where)) {
			return false;
		}
		const JsonValue *field;
		if (!optional(p_value, "var", JsonValue::Kind::Bool, p_where, field)) {
			return false;
		}
		r_type.is_var = field != nullptr && field->boolean;
		if (!optional(p_value, "ref", JsonValue::Kind::String, p_where, field)) {
			return false;
		}
		if (field != nullptr) {
			r_type.has_ref = true;
			r_type.ref = field->text;
			if (!get_string(p_value, "refPath", r_type.ref_path, p_where) || !get_int(p_value, "refOrigin", r_type.ref_origin, p_where) ||
					!get_bool(p_value, "refOption", r_type.ref_option, p_where)) {
				return false;
			}
		}
		if (!optional(p_value, "struct", JsonValue::Kind::String, p_where, field)) {
			return false;
		}
		if (field != nullptr) {
			r_type.struct_name = field->text;
		}
		if (!optional(p_value, "enum", JsonValue::Kind::String, p_where, field)) {
			return false;
		}
		if (field != nullptr) {
			r_type.has_enum = true;
			r_type.enum_name = field->text;
			if (!get_int(p_value, "enumerators", r_type.enumerators, p_where)) {
				return false;
			}
		}
		if (!optional(p_value, "userStruct", JsonValue::Kind::Object, p_where, field)) {
			return false;
		}
		if (field != nullptr) {
			r_type.has_user_struct = true;
			const std::string where = "the userStruct of " + p_where;
			const JsonValue *names = require(*field, "fieldNames", JsonValue::Kind::Array, where);
			const JsonValue *keys = names == nullptr ? nullptr : require(*field, "fieldKeys", JsonValue::Kind::Array, where);
			const JsonValue *types = keys == nullptr ? nullptr : require(*field, "fieldTypes", JsonValue::Kind::Array, where);
			if (types == nullptr || !get_string(*field, "name", r_type.user_struct_name, where)) {
				return false;
			}
			if (names->items.size() != keys->items.size() || names->items.size() != types->items.size()) {
				return fail(where + " has field lists of different lengths");
			}
			const size_t count = names->items.size();
			r_type.field_names.resize(count);
			r_type.field_keys.resize(count);
			r_type.field_types.resize(count);
			for (size_t index = 0; index < count; ++index) {
				if (!names->items[index].is_string() || !keys->items[index].is_string()) {
					return fail(where + " has a field name or key that is not a string");
				}
				r_type.field_names[index] = names->items[index].text;
				r_type.field_keys[index] = keys->items[index].text;
				if (!read_member_type(types->items[index], r_type.field_types[index], where)) {
					return false;
				}
			}
		}
		return true;
	}

	bool read_payload_shape(const JsonValue &p_value, SidecarPayloadShape &r_shape, const std::string &p_where) {
		if (!p_value.is_object()) {
			return fail(p_where + " holds a payload shape that is not an object");
		}
		if (!get_int(p_value, "kind", r_shape.kind, p_where) || !get_int(p_value, "reject", r_shape.reject, p_where) ||
				!get_string(p_value, "rejectDetail", r_shape.reject_detail, p_where)) {
			return false;
		}
		const JsonValue *whole;
		if (!optional(p_value, "whole", JsonValue::Kind::Object, p_where, whole)) {
			return false;
		}
		if (whole != nullptr) {
			r_shape.has_whole = true;
			if (!read_member_type(*whole, r_shape.whole, p_where)) {
				return false;
			}
		}
		const JsonValue *args = require(p_value, "args", JsonValue::Kind::Array, p_where);
		if (args == nullptr) {
			return false;
		}
		r_shape.args.resize(args->items.size());
		for (size_t index = 0; index < args->items.size(); ++index) {
			const JsonValue &arg = args->items[index];
			if (!arg.is_object()) {
				return fail(p_where + " holds a payload argument that is not an object");
			}
			const JsonValue *type = require(arg, "type", JsonValue::Kind::Object, p_where);
			if (type == nullptr || !get_string(arg, "name", r_shape.args[index].name, p_where) ||
					!get_string(arg, "key", r_shape.args[index].key, p_where) || !read_member_type(*type, r_shape.args[index].type, p_where)) {
				return false;
			}
		}
		return true;
	}

	template <typename Row, typename ReadRow>
	bool read_rows(const JsonValue &p_object, const char *p_key, std::vector<Row> &r_rows, const std::string &p_where, ReadRow p_read_row) {
		const JsonValue *rows = require(p_object, p_key, JsonValue::Kind::Array, p_where);
		if (rows == nullptr) {
			return false;
		}
		r_rows.resize(rows->items.size());
		for (size_t index = 0; index < rows->items.size(); ++index) {
			if (!(this->*p_read_row)(rows->items[index], r_rows[index], p_where)) {
				return false;
			}
		}
		return true;
	}

	bool read_types(const JsonValue &p_types, SidecarClass &r_class, const std::string &p_where) {
		const std::string where = "the declared types of " + p_where;
		const JsonValue *members;
		const JsonValue *methods;
		const JsonValue *signals;
		if (!optional(p_types, "members", JsonValue::Kind::Object, where, members) || !optional(p_types, "methods", JsonValue::Kind::Object, where, methods) ||
				!optional(p_types, "signals", JsonValue::Kind::Object, where, signals)) {
			return false;
		}
		if (members != nullptr) {
			r_class.member_types.resize(members->items.size());
			for (size_t index = 0; index < members->items.size(); ++index) {
				r_class.member_types[index].first = members->keys[index];
				if (!read_member_type(members->items[index], r_class.member_types[index].second, "member `" + members->keys[index] + "` of " + where)) {
					return false;
				}
			}
		}
		if (methods != nullptr) {
			r_class.method_types.resize(methods->items.size());
			for (size_t index = 0; index < methods->items.size(); ++index) {
				const std::string method_where = "method `" + methods->keys[index] + "` of " + where;
				const JsonValue &method = methods->items[index];
				SidecarMethodTypes &types = r_class.method_types[index].second;
				r_class.method_types[index].first = methods->keys[index];
				if (!method.is_object()) {
					return fail(method_where + " is not an object");
				}
				const JsonValue *params = require(method, "params", JsonValue::Kind::Array, method_where);
				const JsonValue *result = params == nullptr ? nullptr : require(method, "result", JsonValue::Kind::Object, method_where);
				if (result == nullptr || !read_member_type(*result, types.result, method_where)) {
					return false;
				}
				types.params.resize(params->items.size());
				for (size_t param = 0; param < params->items.size(); ++param) {
					if (!read_member_type(params->items[param], types.params[param], method_where)) {
						return false;
					}
				}
			}
		}
		if (signals != nullptr) {
			r_class.signal_types.resize(signals->items.size());
			for (size_t index = 0; index < signals->items.size(); ++index) {
				r_class.signal_types[index].first = signals->keys[index];
				if (!read_payload_shape(signals->items[index], r_class.signal_types[index].second, "signal `" + signals->keys[index] + "` of " + where)) {
					return false;
				}
			}
		}
		return true;
	}

	bool read_class(const JsonValue &p_value, SidecarClass &r_class) {
		const std::string where = "class `" + r_class.name + "`";
		if (!p_value.is_object()) {
			return fail(where + " is not an object");
		}
		if (!get_bool(p_value, "abstract", r_class.abstract, where) || !get_bool(p_value, "published", r_class.published, where) ||
				!get_bool(p_value, "exportsHarvested", r_class.exports_harvested, where)) {
			return false;
		}
		const JsonValue *to_string;
		if (!optional(p_value, "toString", JsonValue::Kind::String, where, to_string)) {
			return false;
		}
		if (to_string != nullptr) {
			r_class.to_string = to_string->text;
		}
		if (!read_rows(p_value, "methods", r_class.methods, where, &SidecarReader::read_method) ||
				!read_rows(p_value, "signals", r_class.signals, where, &SidecarReader::read_signal) ||
				!read_rows(p_value, "rpcs", r_class.rpcs, where, &SidecarReader::read_rpc) ||
				!read_rows(p_value, "exports", r_class.exports, where, &SidecarReader::read_export)) {
			return false;
		}
		const JsonValue *statics;
		if (!optional(p_value, "statics", JsonValue::Kind::Object, where, statics)) {
			return false;
		}
		if (statics != nullptr) {
			const std::string statics_where = "the statics of " + where;
			const JsonValue *members = require(*statics, "members", JsonValue::Kind::Array, statics_where);
			if (members == nullptr) {
				return false;
			}
			r_class.statics.resize(members->items.size());
			for (size_t index = 0; index < members->items.size(); ++index) {
				const JsonValue &member = members->items[index];
				SidecarStatic &entry = r_class.statics[index];
				if (!member.is_object()) {
					return fail(statics_where + " lists a member that is not an object");
				}
				if (!get_string(member, "name", entry.name, statics_where) || !get_bool(member, "isFunction", entry.is_function, statics_where) ||
						!get_int(member, "line", entry.line, statics_where) || !get_int(member, "column", entry.column, statics_where)) {
					return false;
				}
				if (!entry.is_function) {
					const JsonValue *value = require(member, "value", JsonValue::Kind::Object, statics_where);
					if (value == nullptr || !read_value(*value, entry.value, "static `" + entry.name + "` of " + where)) {
						return false;
					}
				}
			}
		}
		const JsonValue *types;
		if (!optional(p_value, "types", JsonValue::Kind::Object, where, types)) {
			return false;
		}
		return types == nullptr || read_types(*types, r_class, where);
	}

	bool read_bindings(const JsonValue &p_root, Sidecar &r_sidecar) {
		const JsonValue *bindings = require(p_root, "bindings", JsonValue::Kind::Array, "the top level");
		if (bindings == nullptr) {
			return false;
		}
		r_sidecar.bindings.resize(bindings->items.size());
		for (size_t index = 0; index < bindings->items.size(); ++index) {
			const JsonValue &row = bindings->items[index];
			SidecarBinding &binding = r_sidecar.bindings[index];
			if (!row.is_object() || !get_string(row, "verse", binding.verse, "a binding row")) {
				return fail("a binding row is not an object with a `verse` class");
			}
			const JsonValue *godot;
			const JsonValue *script;
			if (!optional(row, "godot", JsonValue::Kind::String, "a binding row", godot) || !optional(row, "script", JsonValue::Kind::String, "a binding row", script)) {
				return false;
			}
			if ((godot == nullptr) == (script == nullptr)) {
				return fail("the binding row for `" + binding.verse + "` names neither or both of `godot` and `script`");
			}
			(godot != nullptr ? binding.godot : binding.script) = (godot != nullptr ? godot : script)->text;
		}
		return true;
	}

	bool read_engine_signals(const JsonValue &p_root, Sidecar &r_sidecar) {
		const JsonValue *signals;
		if (!optional(p_root, "engineSignals", JsonValue::Kind::Object, "the top level", signals)) {
			return false;
		}
		if (signals == nullptr) {
			return true;
		}
		const JsonValue *shapes = require(*signals, "shapes", JsonValue::Kind::Array, "`engineSignals`");
		const JsonValue *keys = shapes == nullptr ? nullptr : require(*signals, "keys", JsonValue::Kind::Object, "`engineSignals`");
		if (keys == nullptr) {
			return false;
		}
		r_sidecar.engine_signal_shapes.resize(shapes->items.size());
		for (size_t index = 0; index < shapes->items.size(); ++index) {
			if (!read_payload_shape(shapes->items[index], r_sidecar.engine_signal_shapes[index], "`engineSignals`")) {
				return false;
			}
		}
		for (size_t index = 0; index < keys->items.size(); ++index) {
			int64_t shape;
			if (!json_to_int64(keys->items[index], shape) || shape < 0 || uint64_t(shape) >= shapes->items.size()) {
				return fail("`engineSignals` key `" + keys->keys[index] + "` names no shape");
			}
			r_sidecar.engine_signal_keys.emplace(keys->keys[index], size_t(shape));
		}
		return true;
	}
};

} // namespace

const SidecarClass *Sidecar::find_class(std::string_view p_name) const {
	const auto found = classes_by_name.find(std::string(p_name));
	return found == classes_by_name.end() ? nullptr : &classes[found->second];
}

const SidecarBinding *Sidecar::find_binding(std::string_view p_verse_name) const {
	for (const SidecarBinding &binding : bindings) {
		if (binding.verse == p_verse_name) {
			return &binding;
		}
	}
	return nullptr;
}

bool sidecar_parse(std::string_view p_text, const std::string &p_path, Sidecar &r_sidecar, std::string &r_error) {
	r_error.clear();
	r_sidecar = Sidecar();
	JsonValue root;
	std::string json_error;
	if (!json_parse(p_text, root, json_error)) {
		r_error = p_path + " is not valid JSON";
		return false;
	}
	SidecarReader reader(p_path, r_error);
	return reader.read(root, r_sidecar);
}

} // namespace vm
