#include "verse_diagnostic_prose.h"

#include "verse_diagnostics.h"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

namespace {

std::string utf8_of(const String &p_text) {
	const CharString utf8 = p_text.utf8();
	return std::string(utf8.get_data(), utf8.length());
}

String string_of(const std::string &p_text) {
	return String::utf8(p_text.data(), (int)p_text.size());
}

} // namespace

String export_rejection_message(vh_export_reject p_reject, vh_export_hint p_hint,
		const String &p_name, const String &p_class_name, const String &p_native_class) {
	return string_of(verse_export_rejection(p_reject, p_hint, utf8_of(p_name), utf8_of(p_class_name), utf8_of(p_native_class)));
}

String export_rejection_code(int64_t p_reject) {
	switch ((vh_export_reject)p_reject) {
		case VH_EXPORT_OBJECT_NOT_OPTIONAL:
			return String("OBJECT_EXPORT_NOT_OPTIONAL");
		case VH_EXPORT_OPTION_NOT_OBJECT:
			return String("OPTION_EXPORT_NOT_OBJECT");
		case VH_EXPORT_SCRIPT_CLASS_NOT_GLOBAL:
			return String("SCRIPT_CLASS_EXPORT_NOT_GLOBAL");
		case VH_EXPORT_HINT_WRONG_TYPE:
			return String("EXPORT_HINT_WRONG_TYPE");
		case VH_EXPORT_BINDING_CLASS_UNSUPPORTED:
			return String("EXPORT_BINDING_CLASS_UNSUPPORTED");
		case VH_EXPORT_OK:
		case VH_EXPORT_UNSUPPORTED_TYPE:
			return String("EXPORT_TYPE_UNSUPPORTED");
	}
	return String("EXPORT_TYPE_UNSUPPORTED");
}

String signal_rejection_message(const String &p_name, int32_t p_reject, const String &p_reject_detail) {
	return string_of(verse_signal_rejection(utf8_of(p_name), p_reject, utf8_of(p_reject_detail)));
}

String rpc_rejection_message(const String &p_name, int32_t p_reject, const String &p_reject_detail) {
	return string_of(verse_rpc_rejection(utf8_of(p_name), p_reject, utf8_of(p_reject_detail)));
}

String rpc_rejection_code(int32_t p_reject) {
	switch ((vh_rpc_reject)p_reject) {
		case VH_RPC_UNKNOWN_ARGUMENT:
			return String("RPC_UNKNOWN_ARGUMENT");
		case VH_RPC_DUPLICATE_CATEGORY:
			return String("RPC_DUPLICATE_CATEGORY");
		case VH_RPC_OK:
		case VH_RPC_BAD_ARGUMENT_TYPE:
			return String("RPC_BAD_ARGUMENT_TYPE");
	}
	return String("RPC_BAD_ARGUMENT_TYPE");
}

String signal_rejection_code(int32_t p_reject) {
	switch ((vh_signal_reject)p_reject) {
		case VH_SIGNAL_IS_VAR:
			return String("SIGNAL_IS_VAR");
		case VH_SIGNAL_NOT_PUBLIC:
			return String("SIGNAL_NOT_PUBLIC");
		case VH_SIGNAL_NO_GODOT_OWNER:
			return String("SIGNAL_NO_GODOT_OWNER");
		case VH_SIGNAL_PAYLOAD_NESTED_STRUCT:
			return String("SIGNAL_PAYLOAD_NESTED_STRUCT");
		case VH_SIGNAL_NEEDS_ATTRIBUTE:
			return String("SIGNAL_NEEDS_ATTRIBUTE");
		case VH_SIGNAL_OK:
		case VH_SIGNAL_PAYLOAD_UNSUPPORTED:
			return String("SIGNAL_PAYLOAD_UNSUPPORTED");
	}
	return String("SIGNAL_PAYLOAD_UNSUPPORTED");
}

String verse_formatted_diagnostic(const Dictionary &p_error) {
	return String(p_error["path"]) + ":" + String::num_int64((int64_t)p_error["line"]) + ":"
			+ String::num_int64((int64_t)p_error["column"]) + ": " + String(p_error["message"]);
}

PackedStringArray verse_flattened_diagnostics(const Dictionary &p_errors_by_path) {
	PackedStringArray flattened;
	const Array paths = p_errors_by_path.keys();
	for (int64_t i = 0; i < paths.size(); i++) {
		const TypedArray<Dictionary> errors = p_errors_by_path[paths[i]];
		for (int64_t e = 0; e < errors.size(); e++) {
			flattened.push_back(verse_formatted_diagnostic(errors[e]));
		}
	}
	return flattened;
}
