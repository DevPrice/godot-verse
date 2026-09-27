#pragma once

#include "verse_diagnostic_prose.h"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class VerseScriptLanguage;

// True on Godot's main/game thread, the one the host pins every Verse call to and the one every
// mutation of VerseProjectState's pump and of VerseScriptLanguage's _frame-owned state must come
// from. A caller with a legitimate reason to run elsewhere -- VerseScript::_get_documentation
// (B20) -- checks this itself and declines before touching either, rather than asserting.
bool verse_on_main_thread();

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

// The editor side's build-and-analysis pump: publishing a generation, queueing and reaping the
// background analyses, and the diagnostics and analysed text each of those leaves behind.
//
// One instance, owned by VerseScriptLanguage, which keeps the Godot virtuals and reaches the pump
// through it. The language is borrowed back for what a landing *does* -- the roster, the module
// map, the script warnings, the per-script notifications -- none of which is the pump's own state.
class VerseProjectState {
public:
	// Where the session is in publishing generations.
	//
	// A build against a held-back binding roster is *provisional*: those bindings carry types and
	// no members, so a Verse file calling one of their methods fails against members the next
	// frame's generation will have. Such a build's verdict is withheld rather than logged, and one
	// corrective build on the next frame produces the real one. Once per session -- a roster that
	// never completes would otherwise withhold every verdict it ever produced, which is a silent
	// session rather than a noisy one (B30, B36).
	//
	//   from                trigger                                                   to
	//   UNBUILT             build_project: no runtime, or the host will not load      UNBUILT
	//   UNBUILT, BUILT      build_project fails with the roster incomplete: withheld  CORRECTION_PENDING
	//   UNBUILT             build_project finishes any other way, including the       BUILT
	//                       two that compile nothing (no compiler, no sources)
	//   BUILT               build_project finishes, not withheld                      BUILT
	//   CORRECTION_PENDING  build_project finishes (Play before _frame took it)       CORRECTION_PENDING
	//   CORRECTION_PENDING  take_corrective_build, from _frame, which then builds     CORRECTION_SPENT
	//   CORRECTION_SPENT    build_project finishes                                    CORRECTION_SPENT
	//
	// Every "finishes" records the build's status, which ensure_project_built answers from then on.
	// Nothing leaves a built state for UNBUILT: a failed build still counts as built, so
	// ensure_project_built answers the failure rather than rebuilding -- refresh_from_analysis asks
	// it from inside compile(), and a build re-entered there would report the diagnostics being
	// withheld.
	enum class BuildState {
		UNBUILT,
		BUILT,
		CORRECTION_PENDING,
		CORRECTION_SPENT,
	};

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

	// The analysis pump, beside the build. At most one analysis runs, which is the host's rule, and
	// at most one buffer waits per kind, the newest. A COMPLETION buffer is the author's text with
	// the half-typed identifier replaced by the placeholder; an ORDINARY one is the text itself.
	//
	//   state      trigger                                          effect
	//   any        request_check(COMPLETION)                        completion_refresh := the buffer, and
	//                                                               then the next two rows
	//   any        request_check, the buffer is the one in flight   nothing queued
	//   any        request_check otherwise                          that kind's slot := the buffer
	//   IDLE       start_pending_check: a slot filled, host up and  ANALYZING the completion slot if it is
	//              not busy, vh_check_project_begin accepts         filled, else the ordinary; slot emptied
	//                                                               -- but the owed restore first, as an
	//                                                               ORDINARY, when it is for another file
	//   ANALYZING  start_pending_check                              nothing: the host refuses a begin until
	//                                                               the last one is reaped
	//   ANALYZING  poll_check, the host reports it finished         IDLE, after the landing below
	//   any        flush_pending_check, host with a compiler        one slot analysed synchronously,
	//                                                               completion first: the buffer and its
	//                                                               diagnostics recorded, analysis_epoch
	//                                                               alone advances, nothing is told
	//   any        flush_pending_check, no host or no compiler      both slots emptied
	//   any        build_project publishes a generation             both epochs advance; one ORDINARY
	//                                                               request queued for the text just read
	//
	// A COMPLETION landing also owes the host that file's own text back (pending_restore), and an
	// ORDINARY landing for the file, or a build, pays it.
	//
	// A landing records the buffer as analysed and advances analysis_epoch. ORDINARY then advances
	// description_epoch, records the diagnostics, refreshes the file's script warnings, re-arms the
	// documentation republish and tells every live script; COMPLETION draws none of that, because it
	// describes a line nobody has finished writing, and arms the completion refresh when it was
	// for completion_refresh's buffer.
	//
	// Two slots rather than one, and the second is not a luxury: confirming a completion changes
	// the text, so _validate runs on the editor's idle timer a moment after _complete_code queued
	// the buffer the argument hint is waiting on. Sharing a slot let that validate displace it --
	// and nothing re-asks, because only a COMPLETION landing arms the refresh, so the hint stayed
	// blank until the next keystroke. Which of the two won was a race against how busy the host
	// was, which is what made it intermittent (B27).
	enum class CheckKind {
		ORDINARY,
		COMPLETION,
	};
	struct CheckRequest {
		godot::String path;
		godot::String source;
	};

	void request_check(const godot::String &p_path, const godot::String &p_normalized_source, CheckKind p_kind = CheckKind::ORDINARY);
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

	// Whether any build has finished, which is what every reader that needs a program asks first.
	bool is_built() const { return build_state != BuildState::UNBUILT; }

	// CORRECTION_PENDING -> CORRECTION_SPENT, answering whether that transition happened. _frame's
	// one caller builds when it did.
	bool take_corrective_build();

	// The classes the last binding generation emitted as bare types -- because a script would not
	// load, or because it was held back to avoid a cyclic one (B30). Non-empty re-arms the refresh
	// until one describes everything, and is what makes a failed build's verdict provisional.
	//
	// Kept by name rather than as a flag beside the list, because the withheld build has to say
	// which: a verdict nobody prints is the right answer to a diagnostic that is already false,
	// and it was the *whole* answer -- so a node whose script failed to attach reached GDScript as
	// "on a base object of type 'Nil'" and named neither the script nor the reason (B36).
	void set_incomplete_binding_classes(const std::vector<std::string> &p_classes) { incomplete_bindings = p_classes; }
	bool binding_roster_incomplete() const { return !incomplete_bindings.empty(); }
	const std::vector<std::string> &incomplete_binding_classes() const { return incomplete_bindings; }

	// Every error the last analysis or build filed, by res:// path.
	const godot::Dictionary &all_diagnostics() const { return diagnostics_by_path; }

	// The text the host currently holds for each script, keyed by res:// path. A validate whose
	// buffer already matches it needs no re-analysis: the host's last analysis answered for
	// exactly these sources. Godot validates on open, on every tab switch, on an idle timer and
	// on save, while a whole-project semantic analysis costs ~750 ms whether anything changed or
	// not. Nothing on the editor's thread waits for one any more, but it blocks the VM for its
	// whole length, so without this every tab switch costs a `@tool` script that long not running.
	const godot::Dictionary &analyzed_sources() const { return analyzed_source_by_path; }

	// res:// path for each absolute path the host reports diagnostics against, as of the last build.
	const godot::Dictionary &res_path_by_globalized() const { return path_by_globalized; }

	// Whether a completion buffer is waiting for an analysis. probe_complete asks, because a caret
	// whose first answer queued one is the caret worth asking twice.
	bool completion_check_pending() const { return pending_completion.has_value(); }

	// Whether a completion buffer is still waiting or being analysed. probe_complete_code's caller
	// lets frames go by until this is false, which is when the editor's own re-ask would come.
	bool completion_check_outstanding() const {
		return pending_completion.has_value() || (in_flight.has_value() && in_flight->kind == CheckKind::COMPLETION);
	}

	// Armed by a completion landing for the buffer completion last asked about, and taken by the
	// _frame that asks the editor to complete again. Answers whether it was armed.
	bool take_completion_refresh();

	// The completion buffer last asked about and the file it belongs to, empty until one is. Never
	// cleared: refresh_completion_if_current compares the editor's caret against it.
	const CheckRequest &completion_refresh_request() const { return completion_refresh; }

private:
	VerseScriptLanguage &language;

	BuildState build_state = BuildState::UNBUILT;
	// What the last finished build came back with. Unread in UNBUILT.
	godot::Error build_status = godot::OK;
	std::vector<std::string> incomplete_bindings;

	// The table's every "finishes" row.
	void finish_build(godot::Error p_status, bool p_withheld);

	struct InFlightCheck {
		CheckKind kind = CheckKind::ORDINARY;
		CheckRequest request;
	};
	std::optional<CheckRequest> pending_ordinary;
	std::optional<CheckRequest> pending_completion;
	// A file's own text, owed to the host after a completion analysis of it. The host keeps the
	// last buffer it was handed for every file, so the half-typed line a completion analysed stays
	// the host's copy of that file after the author moves on -- and a line that does not parse
	// costs every other file its analysis, completion included. Sent ahead of the next analysis of
	// any *other* file (next_request); an analysis of the same file supersedes it.
	std::optional<CheckRequest> pending_restore;
	// The text each file had in the last analysis that was not a completion's: the build's read of
	// the disk, or an ordinary buffer. What pending_restore sends.
	godot::Dictionary real_source_by_path;

	// The slot start_pending_check or flush_pending_check runs next, or null when all are empty:
	// the completion slot, then the ordinary one, with pending_restore ahead of either when it is
	// for another file. A restore for the same file is dropped here, since that analysis replaces
	// the host's copy anyway. r_kind is what the landing is treated as; a restore is ORDINARY,
	// because it is the file's own text.
	std::optional<CheckRequest> *next_slot(CheckKind &r_kind);

	// What a landing of p_kind leaves owed: a completion owes the file's own text back, and
	// anything else is that text, which settles the debt.
	void note_landed(CheckKind p_kind, const CheckRequest &p_request);
	// Empty is IDLE.
	std::optional<InFlightCheck> in_flight;

	CheckRequest completion_refresh;
	bool completion_refresh_armed = false;

	// The analysis in flight, or a default one -- ORDINARY, empty path and text -- when IDLE. What
	// request_check compares a buffer against, and what poll_check would land if the host reported
	// a finish nothing began, which vh_check_project_poll's contract rules out.
	InFlightCheck in_flight_or_default() const { return in_flight.value_or(InFlightCheck()); }

	godot::Dictionary diagnostics_by_path;
	// The compiler's warnings, keyed and shaped the same way and recorded by the same analysis.
	// Kept apart from the errors because diagnostics_for is what decides a script's validity.
	godot::Dictionary compiler_warnings_by_path;
	godot::Dictionary analyzed_source_by_path;
	godot::Dictionary path_by_globalized;

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
