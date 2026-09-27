#pragma once

// Every diagnostic this bridge authors, by ID (docs/diagnostics.md), and the sentences for the ABI's
// rejection and call-failure codes.
//
// Shared by src/ and vm/, and inline C++ rather than a C table because nothing here crosses a
// binary boundary: each side compiles its own copy, so this is not ABI and need not be C the way
// verse_host_abi.h has to be. What it buys is one exhaustive switch per reject enum that both sides
// run -- a new enumerator fails the build in each (MSVC /we4062, GCC -Werror=switch) instead of
// falling into a generic sentence on one side while the other says something else. No godot-cpp:
// vm/ has none, so the currency is std::string and src/ converts at its one wrapper.
//
// The compiler's own diagnostics are not in this registry and never carry an ID; a sentence the
// bridge appends to one does.

#include "verse_host_abi.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

enum class verse_diag : uint16_t {
#define VERSE_DIAG(p_id, p_where, p_text) p_id,
#include "verse_diagnostics.def"
#undef VERSE_DIAG
};

struct verse_diag_row {
	std::string_view id;
	std::string_view where;
	std::string_view text;
};

inline constexpr verse_diag_row verse_diag_rows[] = {
#define VERSE_DIAG(p_id, p_where, p_text) { #p_id, p_where, p_text },
#include "verse_diagnostics.def"
#undef VERSE_DIAG
};

inline const verse_diag_row &verse_diag_row_of(verse_diag p_id) {
	return verse_diag_rows[static_cast<size_t>(p_id)];
}

struct verse_diag_arg {
	std::string_view name;
	std::string_view value;
};

// The row's text with every `{name}` replaced by the argument of that name. A placeholder with no
// argument is left as written, so a missing one is visible in the output rather than silently empty.
inline std::string verse_diag_text(verse_diag p_id, std::initializer_list<verse_diag_arg> p_args = {}) {
	const std::string_view text = verse_diag_row_of(p_id).text;
	std::string out;
	out.reserve(text.size() + 64);
	size_t at = 0;
	while (at < text.size()) {
		if (text[at] == '{') {
			size_t end = at + 1;
			while (end < text.size() && ((text[end] >= 'a' && text[end] <= 'z') || text[end] == '_')) {
				end++;
			}
			if (end > at + 1 && end < text.size() && text[end] == '}') {
				const std::string_view name = text.substr(at + 1, end - at - 1);
				const verse_diag_arg *found = nullptr;
				for (const verse_diag_arg &arg : p_args) {
					if (arg.name == name) {
						found = &arg;
						break;
					}
				}
				if (found != nullptr) {
					out.append(found->value);
					at = end + 1;
					continue;
				}
			}
		}
		out.push_back(text[at]);
		at++;
	}
	return out;
}

// The attribute an author wrote, named back to them. R-EXP-1's five are told apart by the hint
// alone, because vh_export_desc has no room to carry the spelling.
inline const char *verse_export_hint_attribute(vh_export_hint p_hint) {
	switch (p_hint) {
		case VH_EXPORT_HINT_FILE:
			return "`@export_file`";
		case VH_EXPORT_HINT_DIR:
			return "`@export_dir`";
		case VH_EXPORT_HINT_MULTILINE:
			return "`@export_multiline`";
		case VH_EXPORT_HINT_FLAGS:
			return "`@export_flags`";
		case VH_EXPORT_HINT_NODE_PATH:
			return "`@export_node_path`";
		// VH_EXPORT_HINT_WRONG_TYPE is refused only for the five above; these are type-driven.
		case VH_EXPORT_HINT_NONE:
		case VH_EXPORT_HINT_RANGE:
		case VH_EXPORT_HINT_ENUM:
		case VH_EXPORT_HINT_CLASS:
		case VH_EXPORT_HINT_SCRIPT_CLASS:
			return "an inspector hint";
	}
	return "an inspector hint";
}

// What a rejected export has to say for itself. p_native_class is read only for
// VH_EXPORT_BINDING_CLASS_UNSUPPORTED, and names the nearest class the inspector can draw.
inline std::string verse_export_rejection(vh_export_reject p_reject, vh_export_hint p_hint, std::string_view p_name,
		std::string_view p_class_name, std::string_view p_native_class) {
	switch (p_reject) {
		case VH_EXPORT_OBJECT_NOT_OPTIONAL:
			return verse_diag_text(verse_diag::VG1001, { { "name", p_name }, { "class", p_class_name } });
		case VH_EXPORT_OPTION_NOT_OBJECT:
			return verse_diag_text(verse_diag::VG1002, { { "name", p_name } });
		// R-EXP-1's five exist *because* the type says nothing, so the author is the only one who can
		// pair them -- and a mispairing is silent otherwise: Godot draws a plain field.
		case VH_EXPORT_HINT_WRONG_TYPE:
			return verse_diag_text(verse_diag::VG1003, { { "name", p_name }, { "attribute", verse_export_hint_attribute(p_hint) },
					{ "wants", p_hint == VH_EXPORT_HINT_FLAGS ? "an `int`" : "a `string`" } });
		// Narrow since B19: a class with no registered Godot name exports anyway, filtered by its
		// nearest mirrored ancestor. What is left is a chain that reaches `object` without one.
		case VH_EXPORT_SCRIPT_CLASS_NOT_GLOBAL:
			return verse_diag_text(verse_diag::VG1004, { { "name", p_name }, { "class", p_class_name } });
		// Its own reason rather than VG1007's, and the binding's nearest drawable class named when
		// NativeClassOf found one (docs/generated-bindings.md).
		case VH_EXPORT_BINDING_CLASS_UNSUPPORTED:
			return p_native_class.empty()
					? verse_diag_text(verse_diag::VG1006, { { "name", p_name }, { "class", p_class_name } })
					: verse_diag_text(verse_diag::VG1005, { { "name", p_name }, { "class", p_class_name }, { "native", p_native_class } });
		case VH_EXPORT_OK:
		case VH_EXPORT_UNSUPPORTED_TYPE:
			return verse_diag_text(verse_diag::VG1007, { { "name", p_name } });
	}
	return verse_diag_text(verse_diag::VG1007, { { "name", p_name } });
}

// What a refused signal has to say for itself. p_detail is read only for the two payload rejections.
inline std::string verse_signal_rejection(std::string_view p_name, int32_t p_reject, std::string_view p_detail) {
	switch ((vh_signal_reject)p_reject) {
		case VH_SIGNAL_IS_VAR:
			return verse_diag_text(verse_diag::VG2001, { { "name", p_name } });
		// Retired with the enumerator: the host does not test a member's access level. Kept for as
		// long as the value is.
		case VH_SIGNAL_NOT_PUBLIC:
			return verse_diag_text(verse_diag::VG2002, { { "name", p_name } });
		case VH_SIGNAL_NO_GODOT_OWNER:
			return verse_diag_text(verse_diag::VG2003, { { "name", p_name } });
		case VH_SIGNAL_PAYLOAD_NESTED_STRUCT:
			return verse_diag_text(verse_diag::VG2004, { { "name", p_name }, { "field", p_detail } });
		case VH_SIGNAL_PAYLOAD_UNSUPPORTED:
			return verse_diag_text(verse_diag::VG2005, { { "name", p_name }, { "field", p_detail } });
		case VH_SIGNAL_NEEDS_ATTRIBUTE:
			return verse_diag_text(verse_diag::VG2006, { { "name", p_name } });
		case VH_SIGNAL_OK:
			return verse_diag_text(verse_diag::VG2007, { { "name", p_name } });
	}
	return verse_diag_text(verse_diag::VG2007, { { "name", p_name } });
}

// What a refused `@rpc` has to say for itself.
inline std::string verse_rpc_rejection(std::string_view p_name, int32_t p_reject, std::string_view p_detail) {
	switch ((vh_rpc_reject)p_reject) {
		case VH_RPC_UNKNOWN_ARGUMENT:
			return verse_diag_text(verse_diag::VG3001, { { "name", p_name }, { "word", p_detail } });
		case VH_RPC_DUPLICATE_CATEGORY:
			return verse_diag_text(verse_diag::VG3002, { { "name", p_name }, { "category", p_detail } });
		case VH_RPC_OK:
		case VH_RPC_BAD_ARGUMENT_TYPE:
			return verse_diag_text(verse_diag::VG3003, { { "name", p_name }, { "wanted", p_detail } });
	}
	return verse_diag_text(verse_diag::VG3003, { { "name", p_name }, { "wanted", p_detail } });
}

// What a Godot call Verse made has to say when Godot answered p_status. p_verb is the call's own
// ("Called", "Read", "Wrote", "Called static"); only a non-OK status is ever passed.
inline std::string verse_call_failure(std::string_view p_verb, std::string_view p_member, int64_t p_handle, int32_t p_status) {
	const std::string handle = std::to_string(p_handle);
	const std::initializer_list<verse_diag_arg> args = { { "verb", p_verb }, { "member", p_member }, { "handle", handle } };
	switch ((vh_call_status)p_status) {
		case VH_CALL_DEAD_OBJECT:
			return verse_diag_text(verse_diag::VG4001, args);
		case VH_CALL_BAD_VALUE:
			return verse_diag_text(verse_diag::VG4002, args);
		case VH_CALL_BAD_ARITY:
			return verse_diag_text(verse_diag::VG4003, args);
		case VH_CALL_OK:
		case VH_CALL_NO_SUCH_MEMBER:
			return verse_diag_text(verse_diag::VG4004, args);
	}
	return verse_diag_text(verse_diag::VG4004, args);
}
