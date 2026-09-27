#pragma once

#include "verse_host_abi.h"

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

// What a refused `@export`, signal or `@rpc` has to say for itself, at the line that declared it
// (R-EXP-1, R-SIG-1, R-EXP-9), and the formatting shared by every diagnostic the output log prints.
//
// The sentences themselves are include/verse_diagnostics.h's, which vm/ runs too; these are its
// godot::String face, so the two backends cannot word one rejection two ways.

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
