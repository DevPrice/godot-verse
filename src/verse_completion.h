#pragma once

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cstdint>

class VerseScriptLanguage;

// Whether the cursor -- the U+FFFF Godot splices into the buffer -- stands inside a comment or a
// string literal. Shared with verse_hover.cpp: a comment is prose and a string is data, so neither
// names a definition, and `_lookup_code` declines there for the same reason completion does.
bool completing_in_comment(const godot::String &p_code, int64_t p_marker);
bool completing_in_string(const godot::String &p_code, int64_t p_marker);

// Completion, answered by the compiler wherever it can be (`_complete_code`), and the probe seam
// over it (`probe_complete`).
//
// Owned by VerseScriptLanguage as a mutable member, the way VerseProjectState, VerseDebugger and
// VerseProfiler are. `_complete_code` stays declared on VerseScriptLanguage and delegates here for
// the reason theirs do; `probe_complete` stays a real VerseScriptLanguage member too, because
// ClassDB binds it by pointer-to-member for the GDScript-visible seam tools/probe_complete.py uses,
// and its body is the one-line delegation.
class VerseCompletion {
public:
	explicit VerseCompletion(VerseScriptLanguage &p_language) :
			language(p_language) {}

	godot::Dictionary complete_code(const godot::String &p_code, const godot::String &p_path, godot::Object *p_owner) const;

	// Every completion the editor could ask for at the positions it is handed. Whole body of
	// VerseScriptLanguage::probe_complete; tools/probe_complete.py consumes it.
	godot::TypedArray<godot::Dictionary> probe(const godot::String &p_path, const godot::PackedInt32Array &p_positions);

	// The classes a `.` at p_receiver_end reaches a member of, nearest first, or empty when the
	// buffer does not say which. Text and the analysis snapshot only: this is what completion has
	// to answer from before any analysis of the buffer in front of the author exists.
	godot::PackedStringArray receiver_classes_from_text(const godot::String &p_source, const godot::String &p_path, int64_t p_receiver_end) const;

	// Fills r_result with what a string literal at p_marker can be completed to -- a node path, a
	// res:// path, an input action or a signal name -- decided by the call the literal is an
	// argument to. Leaves it untouched when the literal is not one of those.
	void complete_in_string(const godot::String &p_code, const godot::String &p_path, int64_t p_marker, godot::Object *p_owner, godot::Dictionary &r_result) const;

	// The signals reachable on the receiver ending at p_receiver_end: the class's own Verse-spelled
	// ones, or Godot's for a mirrored class.
	void collect_signal_names(const godot::String &p_source, const godot::String &p_path, int64_t p_receiver_end, godot::Array &r_options) const;

	// Asks the editor for completion again now that the host describes the buffer the last answer
	// declined on. Called from VerseScriptLanguage::_frame when project_state.take_completion_refresh()
	// says to.
	void refresh_if_current() const;

private:
	VerseScriptLanguage &language;

	// The completion buffer the last vh_complete_symbol answered for, with its position, mode, the
	// analysis_epoch it was answered at, and the answer. Godot re-asks on every keystroke while the
	// popup is open, and each ask costs a whole-project analysis; normalizing the half-typed
	// identifier out of the buffer is what makes the whole of one prefix the same question, and the
	// epoch is what makes it safe to answer from cache rather than merely cheap.
	mutable godot::String completion_cache_source;
	mutable int32_t completion_cache_line = -1;
	mutable int32_t completion_cache_column = -1;
	mutable int32_t completion_cache_mode = -1;
	mutable uint64_t completion_cache_epoch = 0;
	mutable godot::TypedArray<godot::Dictionary> completion_cache_options;

	// The same, for the argument hint. Kept apart because the two are asked about different
	// positions in one buffer -- the callee for the hint, the cursor for the options -- and the
	// host answers both off a single analysis, so caching them together would throw one away.
	mutable godot::String signature_cache_source;
	mutable int32_t signature_cache_line = -1;
	mutable int32_t signature_cache_column = -1;
	mutable uint64_t signature_cache_epoch = 0;
	mutable godot::Dictionary signature_cache;
};
