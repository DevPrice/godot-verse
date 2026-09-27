#pragma once

#include "verse_host_abi.h"

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

// What a refused `@export`, signal or `@rpc` has to say for itself, at the line that declared it
// (R-EXP-1, R-SIG-1, R-EXP-9), and the formatting shared by every diagnostic the output log prints.
//
// Free functions of plain inputs -- a reject code, a name, a detail string -- rather than of the
// Dictionary or VerseSignalInfo/VerseRpcInfo a caller happens to be holding, which is what lets
// `verse_bindings_test`-shaped unit tests reach them with no Godot at all. Only the return type
// keeps this godot-cpp-dependent: a later task moving the signal-rejection sentences into a header
// both src/ and vm/ include would need std::string here instead, since vm/ builds with no
// godot::String, and would drop this header's dependency on godot_cpp/variant entirely -- the ABI
// reject enums it switches over already come from the plain-C verse_host_abi.h.

// The attribute an author wrote, named back to them. R-EXP-1's five are told apart by the hint
// alone, because vh_export_desc has no room to carry the spelling and adding one would be a layout
// change -- so this is the one place the mapping is written down on the consumer's side.
godot::String export_hint_attribute_name(vh_export_hint p_hint);

// What a rejected export has to say for itself. p_native_class is only ever read for
// VH_EXPORT_BINDING_CLASS_UNSUPPORTED, and empty otherwise.
godot::String export_rejection_message(vh_export_reject p_reject, vh_export_hint p_hint,
		const godot::String &p_name, const godot::String &p_class_name, const godot::String &p_native_class);
godot::String export_rejection_code(int64_t p_reject);

// What a refused signal has to say for itself. p_reject_detail is only read for the two payload
// rejections, and empty otherwise.
godot::String signal_rejection_message(const godot::String &p_name, int32_t p_reject, const godot::String &p_reject_detail);
godot::String signal_rejection_code(int32_t p_reject);

// What a refused `@rpc` has to say for itself.
godot::String rpc_rejection_message(const godot::String &p_name, int32_t p_reject, const godot::String &p_reject_detail);
godot::String rpc_rejection_code(int32_t p_reject);

// One diagnostic as the output log prints it, which is also what tells one analysis' results from
// the next: Dictionary's own == is reference equality.
godot::String verse_formatted_diagnostic(const godot::Dictionary &p_error);

// Every diagnostic in p_errors_by_path (a res:// path -> its array of diagnostics), formatted and
// flattened into one array -- the shape two analyses' diagnostics are compared as equal or not in.
godot::PackedStringArray verse_flattened_diagnostics(const godot::Dictionary &p_errors_by_path);
