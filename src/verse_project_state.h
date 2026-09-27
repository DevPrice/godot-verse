#pragma once

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <atomic>
#include <cstdint>

class VerseScriptLanguage;

// A monotonically increasing counter, advanced whenever something a cache might describe changes.
// A cache stores the value this returned when it was filled, and a read compares against current()
// instead of being cleared by hand at every place that could invalidate it.
//
// VerseProjectState holds two instances, for two different questions -- see analysis_epoch and
// description_epoch there, and poll_check for where each actually advances.
//
// Atomic: VerseScript::_get_documentation can run off the editor's thread (B20), and it still has
// to read the current epoch to know whether its own answer is fresh. Relaxed ordering is enough --
// this is a bare "did anything change" counter, not a fence for any other memory.
class VerseEpochCounter {
public:
	uint64_t current() const { return value.load(std::memory_order_relaxed); }
	void advance() { value.fetch_add(1, std::memory_order_relaxed); }

private:
	// Starts at 1, not 0, so a default-constructed cache field reads as already stale against the
	// first real epoch with no separate "never filled" sentinel to carry alongside it.
	std::atomic<uint64_t> value{ 1 };
};

// One diagnostic as the output log prints it, which is also what tells one analysis' results from
// the next: Dictionary's own == is reference equality.
godot::String verse_formatted_diagnostic(const godot::Dictionary &p_error);

// The editor side's build-and-analysis pump: publishing a generation, queueing and reaping the
// background analyses, and the diagnostics and analysed text each of those leaves behind.
//
// One instance, owned by VerseScriptLanguage, which keeps the Godot virtuals and reaches the pump
// through it. The language is borrowed back for what a landing *does* -- the roster, the module
// map, the script warnings, the per-script notifications -- none of which is the pump's own state.
class VerseProjectState {
public:
	explicit VerseProjectState(VerseScriptLanguage &p_language) :
			language(p_language) {}

	// Builds every .verse file under res:// as one Verse program and publishes it as a new
	// generation. Verse's compilation unit is the package rather than the file, so one script
	// cannot be built alone and every build re-enumerates res:// -- which is what makes a file
	// added, renamed or deleted since the last build land without a restart (R-ITER-2).
	//
	// Called on Play and from the Build action, not on save: the whole project is ~200ms, and
	// GDScript only gets away with building on save because its unit is one file. What a save
	// does instead is refresh analysis, which keeps diagnostics, completion and the export
	// *shape* live per keystroke.
	//
	// A failed build publishes nothing, so the last generation that did keeps running (R-ITER-5).
	godot::Error build_project();

	// build_project the first time and the remembered result after, for the callers that need a
	// program to exist but have no business deciding when a new one is published.
	godot::Error ensure_project_built();

	// Errors the project build reported against one script, in the shape _validate returns.
	// The build is the only thing that ever produces them: asking the compiler again for a
	// single file would report every other script's definitions as duplicates.
	// Re-analyses the project with p_path's on-disk text replaced by the editor's buffer and
	// returns just that file's diagnostics. Falls back to the diagnostics recorded at startup
	// when there is no host to ask, and skips the re-analysis entirely when the host already
	// holds this exact text.
	godot::TypedArray<godot::Dictionary> check_buffer(const godot::String &p_path, const godot::String &p_source);

	// Whether the host's last analysis answered for exactly p_source as p_path's text, so
	// diagnostics_for describes that text and not the one before it. True as well when there is
	// no host to ask: the build's diagnostics are then the only answer there will ever be, and a
	// caller that waits for a better one waits forever.
	bool analysis_is_current(const godot::String &p_path, const godot::String &p_source) const;

	// Queues p_source as p_path's text for analysis and returns. _frame is what hands it to the
	// host, and a later one is where the result lands and every script awaiting it is told.
	// Nothing starts on this thread: an analysis blocks the VM for its whole length, so one begun
	// in the middle of the editor's work stops the pump and every `@tool` instance until it lands.
	void queue_check(const godot::String &p_path, const godot::String &p_source);

	// Queues p_path's buffer for analysis in the slot p_is_completion picks: see
	// has_pending_completion_check.
	void request_check(const godot::String &p_path, const godot::String &p_normalized_source, bool p_is_completion = false);
	void start_pending_check();

	// Runs the queued analysis here and now instead of leaving it for _frame.
	//
	// For a caller with no frames to wait for. A build generates code, which leaves the host's
	// program with no AST, so a position -- a hover, a jump, an argument hint -- resolves against
	// nothing until the analysis queued after that build has run. The editor never notices: frames
	// go by between pressing Play and the author's next hover. `probe_hover` has none.
	void flush_pending_check();

	// Reaps a finished analysis and starts whatever came in while it ran. Called once per frame.
	void poll_check();

	godot::TypedArray<godot::Dictionary> diagnostics_for(const godot::String &p_path) const;

	// The compiler's warnings against one script, in the same shape as its errors; _validate
	// reshapes them into the warnings array, which Godot reads with different keys.
	godot::TypedArray<godot::Dictionary> compiler_warnings_for(const godot::String &p_path) const;

	// The epoch a VerseScript compares its own description caches (exports, methods, signals,
	// rpcs, documentation) against, instead of being told to invalidate them by hand. It advances
	// at exactly two points: a published generation, and an *ordinary* analysis landing -- not a
	// completion buffer's, which describes a line the author has not finished typing and must not
	// be read as the file's own shape (see poll_check).
	uint64_t description_epoch_value() const { return description_epoch.current(); }
	uint64_t analysis_epoch_value() const { return analysis_epoch.current(); }

	bool project_built = false;
	// What the last build came back with, so ensure_project_built can answer without publishing
	// a generation of its own.
	godot::Error project_build_status = godot::OK;

	// True when the last generation emitted a class as a bare type -- because its script would not
	// load, or because it was held back to avoid a cyclic one (B30) -- which re-arms the refresh
	// until one describes everything.
	bool bindings_incomplete = false;
	// A build against such a roster cannot describe every binding, so a Verse file *calling* one of
	// their methods fails against members that land on the next frame's generation. That build's
	// verdict is withheld rather than logged and these two carry the correction: one build, on the
	// next frame, with the roster as complete as it is going to get.
	//
	// Once per session. A roster that never completes would otherwise withhold every verdict it
	// ever produced, which is a silent session rather than a noisy one.
	bool provisional_build_allowed = true;
	bool corrective_build_pending = false;

	godot::Dictionary diagnostics_by_path;
	// The compiler's warnings, keyed and shaped the same way and recorded by the same analysis.
	// Kept apart from the errors because diagnostics_for is what decides a script's validity.
	godot::Dictionary compiler_warnings_by_path;

	// The text the host currently holds for each script, keyed by res:// path. A validate whose
	// buffer already matches it needs no re-analysis: the host's last analysis answered for
	// exactly these sources. Godot validates on open, on every tab switch, on an idle timer and
	// on save, while a whole-project semantic analysis costs ~750 ms whether anything changed or
	// not. Nothing on the editor's thread waits for one any more, but it blocks the VM for its
	// whole length, so without this every tab switch costs a `@tool` script that long not running.
	godot::Dictionary analyzed_source_by_path;

	// res:// path for each absolute path the host reports diagnostics against.
	godot::Dictionary path_by_globalized;

	// The buffers waiting for an analysis, and the one an analysis is running for. Only one runs
	// at a time, and a newer buffer replaces a waiting one of its own kind rather than queueing
	// behind it.
	//
	// Two slots rather than one, and the second is not a luxury: confirming a completion changes
	// the text, so _validate runs on the editor's idle timer a moment after _complete_code queued
	// the buffer the argument hint is waiting on. Sharing a slot let that validate displace it --
	// and nothing re-asks, because poll_check refreshes the popup only when the analysis that
	// landed was the completion one, so the hint stayed blank until the next keystroke. Which of
	// the two won was a race against how busy the host was, which is what made it intermittent.
	bool has_pending_check = false;
	godot::String pending_check_path;
	godot::String pending_check_source;
	bool has_pending_completion_check = false;
	godot::String pending_completion_path;
	godot::String pending_completion_source;
	godot::String in_flight_path;
	godot::String in_flight_source;

	// Whether the buffer in flight is a completion buffer rather than the author's own text -- the
	// half-typed identifier replaced by the placeholder. Its diagnostics describe a line nobody has
	// finished writing, so poll_check drops them instead of drawing them; what it keeps is the
	// record that the host now holds this text, which is the whole reason the analysis was asked
	// for.
	bool in_flight_is_completion = false;

	// The completion buffer whose analysis is worth re-asking completion for once it lands, and
	// the file it belongs to. Empty when nothing is waiting on one.
	godot::String completion_refresh_path;
	godot::String completion_refresh_source;
	bool completion_refresh_pending = false;

private:
	VerseScriptLanguage &language;

	// Two counters that answer "has the analyzed program changed since this cache was filled".
	// They are not one counter because they answer different questions. analysis_epoch tracks
	// whatever whole-project snapshot the host currently holds and moves on *every* landed
	// analysis, completion buffer included -- because vh_complete_symbol and vh_signature_at read
	// that snapshot regardless of which file's buffer produced it, so a byte-identical completion
	// query for file A can be stale because of an edit to file B. description_epoch
	// tracks what a *script* may honestly describe itself as, and skips a completion buffer's
	// landing on purpose: that landing describes a line the author has not finished typing, and
	// letting it move description_epoch would flicker a script's own export list and documentation
	// mid-keystroke, the same reason poll_check already runs the per-script analysis_landed() loop
	// only in the non-completion branch. See poll_check and build_project for where each advances.
	VerseEpochCounter analysis_epoch;
	VerseEpochCounter description_epoch;

	// Replaces diagnostics_by_path and compiler_warnings_by_path with one analysis' results,
	// sorted by severity. Analysis covers the whole project, so a file absent from the result
	// has no errors and must lose any it had. Returns whether anything the editor draws
	// actually moved.
	bool record_diagnostics(const godot::Dictionary &p_diagnostics_by_globalized);
};
