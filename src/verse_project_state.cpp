#include "verse_project_state.h"

#include "verse_runtime.h"
#include "verse_script.h"
#include "verse_script_language.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <string>
#include <vector>

using namespace godot;

namespace {

VerseRuntime *get_runtime() {
	return Object::cast_to<VerseRuntime>(Engine::get_singleton()->get_singleton("VerseRuntime"));
}

PackedStringArray flattened_diagnostics(const Dictionary &p_errors_by_path) {
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

} // namespace

String verse_formatted_diagnostic(const Dictionary &p_error) {
	return String(p_error["path"]) + ":" + String::num_int64((int64_t)p_error["line"]) + ":"
			+ String::num_int64((int64_t)p_error["column"]) + ": " + String(p_error["message"]);
}

Error VerseProjectState::ensure_project_built() {
	if (is_built()) {
		return build_status;
	}
	return build_project();
}

bool VerseProjectState::take_corrective_build() {
	if (build_state != BuildState::CORRECTION_PENDING) {
		return false;
	}
	build_state = BuildState::CORRECTION_SPENT;
	return true;
}

void VerseProjectState::finish_build(Error p_status, bool p_withheld) {
	build_status = p_status;
	if (p_withheld) {
		build_state = BuildState::CORRECTION_PENDING;
	} else if (build_state == BuildState::UNBUILT) {
		build_state = BuildState::BUILT;
	}
}

Error VerseProjectState::build_project() {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr) {
		return ERR_UNAVAILABLE;
	}
	if (!runtime->is_host_loaded()) {
		const Error host_status = runtime->load_host();
		if (host_status != OK) {
			return host_status;
		}
	}

	// Re-derived per build rather than trusted: a .vmodule added or removed since the last one
	// moves files between modules, and a build is the moment that is allowed to take effect. The
	// class-name list is re-enumerated with it, since a build walks res:// anyway and is the one
	// hook a game -- where there is no EditorFileSystem to signal -- still has.
	language.invalidate_script_class_names();
	language.refresh_module_map();

	// And the bindings, for the same reason and one more: a build is the one moment a *script*
	// the roster names is guaranteed to be on disk and loadable, and a Verse file naming a binding
	// does not compile until the package holding it is in the project. `_frame` refreshes these
	// too, which is what makes an addon completable without a build -- but a headless run reaches
	// this before its first frame.
	if (language.bindings_refresh_pending && language.refresh_bindings()) {
		language.bindings_refresh_pending = false;
	}

	// An exported game has nothing to build. vh_init loaded the generation the cooker published,
	// and every .verse under res:// is a one-byte stub (D10) -- compiling those would replace a
	// working project with an empty one, if there were a compiler to do it with. The two lines
	// above are the part that still has to happen, because which module a script is in and which
	// class names are declared are read off res:// rather than out of the host.
	if (!runtime->host_has_compiler()) {
		finish_build(OK, false);
		return OK;
	}

	const PackedStringArray sources = VerseScriptLanguage::find_verse_sources("res://");
	// A project with the addon installed and no Verse yet has nothing to publish, and the host
	// refuses an empty list as VH_ERR_ABI -- which failed every Play with no diagnostic to say why.
	if (sources.is_empty()) {
		finish_build(OK, false);
		return OK;
	}
	PackedStringArray globalized;
	PackedStringArray modules;
	ProjectSettings *settings = ProjectSettings::get_singleton();
	for (int64_t i = 0; i < sources.size(); i++) {
		globalized.push_back(settings->globalize_path(sources[i]));
		modules.push_back(language.module_for_script(sources[i]));
	}

	// The host reports against the absolute path it was handed; scripts are keyed by res:// path.
	path_by_globalized.clear();
	for (int64_t i = 0; i < sources.size(); i++) {
		path_by_globalized[globalized[i]] = sources[i];
	}

	Dictionary errors_by_globalized;
	const Error status = runtime->compile_project(globalized, modules, &errors_by_globalized);

	// **A build against a held-back roster is provisional, and a failed one says nothing.** Those
	// bindings carry types and no members, so a Verse file calling `MainScript.Greet()` fails here
	// against a member that lands on the next frame's generation -- a diagnostic that is already
	// false by the time anyone reads it, and the log has no way to retract a line (which is the
	// same reason check_buffer keeps analysis diagnostics out of it). The script editor's own list
	// is replaced wholesale on the next validate, so record_diagnostics below still runs.
	//
	// Decided here and entered at finish_build below, and nothing between the two reads the state.
	const bool provisional_allowed = build_state == BuildState::UNBUILT || build_state == BuildState::BUILT;
	const bool withhold = status != OK && binding_roster_incomplete() && provisional_allowed;
	if (withhold) {
		// Withholding the diagnostics is not withholding the fact. The corrective build repairs
		// the *project*, and it cannot repair a node the scene already tried and failed to give a
		// script to -- so on a cold run this is the only sentence anyone gets, and without it the
		// first thing said is GDScript's, about a value that is null for a reason named nowhere.
		language.warn_incomplete_roster();
	}

	// The host loaded each of these from disk just now, so this is the text it holds. Seeding it
	// here is what makes the *first* validate of a file free rather than only the repeats -- and
	// record_diagnostics below measures a diagnostic's span against it, so it has to be filled
	// first.
	analyzed_source_by_path.clear();
	std::vector<std::string> texts;
	texts.reserve(sources.size());
	for (int64_t i = 0; i < sources.size(); i++) {
		const String text = FileAccess::get_file_as_string(sources[i]);
		const bool read = FileAccess::get_open_error() == OK;
		if (read) {
			analyzed_source_by_path[sources[i]] = verse_newline_normalized(text);
		}
		texts.push_back(read ? std::string(text.utf8().get_data()) : std::string());
	}

	record_diagnostics(errors_by_globalized);

	// Every source is in hand exactly once per build, which is the only affordable moment to ask
	// the three questions that are about the project rather than about a file.
	language.report_name_collisions(sources, texts);
	language.log_script_warnings(sources);

	if (!withhold) {
		const Array reported = errors_by_globalized.keys();
		for (int64_t i = 0; i < reported.size(); i++) {
			language.log_build_diagnostics(TypedArray<Dictionary>(errors_by_globalized[reported[i]]));
		}
	}

	finish_build(status, withhold);

	if (status != OK) {
		if (withhold) {
			return status;
		}
		// Nothing was published, so whatever ran before this still runs (R-ITER-5). The
		// diagnostics above say what is wrong; this says what that costs.
		UtilityFunctions::push_warning(
				"Verse: the project did not build, so no new code was published. The editor's analysis -- "
				"diagnostics, completion and the shape of the exported properties -- is live either way; "
				"fix the errors and build again to replace what is running.");
		return status;
	}

	// A new generation means new classes, new method tables and new declared defaults -- exactly
	// what a byte-identical buffer's cached completion or signature answer could have been
	// describing before this build, and what every script's own description caches now read
	// stale against. Both epochs move together here: a build is a moment both kinds of cache
	// have to give way. The inspector has to be told too, which is R-ITER-3. Snapshotted because
	// refreshing a script republishes its export list, and Godot is free to drop a script while
	// that runs.
	analysis_epoch.advance();
	description_epoch.advance();
	const std::vector<VerseScript *> scripts = language.live_scripts;
	for (VerseScript *script : scripts) {
		script->generation_published();
	}

	// Same reason as the poll's: a class that could not describe itself against the retiring
	// program may be able to now.
	language.docs_refresh_attempted = false;

	// A build generates code, and generating code puts the AST out of reach -- so a hover, a
	// completion or an argument hint has nothing to resolve a position against until an analysis
	// has run. This is the ask for one. Queued rather than run: request_check leaves it for
	// _frame, so it costs the author nothing between pressing Play and the game starting, and a
	// keystroke that arrives first supersedes it.
	//
	// The host used to do this itself, inside vh_compile_project, and it cost ~770 ms of every
	// build to have the answer ready for a question nobody had asked yet.
	//
	// Any file will do -- an analysis is of the whole project, and the path only says which file's
	// buffer overrides what is on disk. The text is the one just read for it.
	for (int64_t i = 0; i < sources.size(); i++) {
		if (analyzed_source_by_path.has(sources[i])) {
			request_check(sources[i], String(analyzed_source_by_path[sources[i]]));
			break;
		}
	}

	return status;
}

TypedArray<Dictionary> VerseProjectState::check_buffer(const String &p_path, const String &p_source) {
	VerseRuntime *runtime = get_runtime();
	if (!is_built() || runtime == nullptr || !runtime->is_host_loaded()) {
		return diagnostics_for(p_path);
	}

	// Anything the host does not already hold needs a fresh analysis, which takes ~750 ms -- some
	// forty-five frames. Start it on the host's thread and answer from the last one: returning stale
	// diagnostics for a moment is a far smaller cost than freezing the editor on every keystroke.
	// _frame picks the result up, and Godot re-validates often enough that the fresh answer lands
	// on its own.
	//
	// Nothing an analysis finds is written to the output log, neither here nor when the result
	// lands. It describes what the file said a moment ago, which may be a mistake the author has
	// already undone, and the log has no way to retract a line; the script editor's own error list
	// can show it because Godot replaces that wholesale on the next validate. The log is the
	// build's (build_project): a build is something the author asked for, and a failed one
	// refuses the run.
	if (!analysis_is_current(p_path, p_source)) {
		queue_check(p_path, p_source);
	}

	// Analysis covers the whole project, so a broken file elsewhere reports against its own path;
	// the editor asked about this one.
	return diagnostics_for(p_path);
}

bool VerseProjectState::analysis_is_current(const String &p_path, const String &p_source) const {
	VerseRuntime *runtime = get_runtime();
	if (!is_built() || runtime == nullptr || !runtime->is_host_loaded()) {
		return true;
	}

	// An exported game has no analysis and never will: the snapshot came out of the sidecar and is
	// the only one there is. Answering false here left every script waiting for a check that
	// nothing could run, so `valid` stayed false and not one scene came up with its script
	// attached -- with no error anywhere, because waiting is not failing.
	if (!runtime->host_has_compiler()) {
		return true;
	}

	// The common case by far: opening a file, switching to its tab and saving it all ask about a
	// buffer nothing has touched since the last analysis.
	return analyzed_source_by_path.has(p_path)
			&& String(analyzed_source_by_path[p_path]) == verse_newline_normalized(p_source);
}

void VerseProjectState::queue_check(const String &p_path, const String &p_source) {
	request_check(p_path, verse_newline_normalized(p_source));
}

void VerseProjectState::request_check(const String &p_path, const String &p_normalized_source, bool p_is_completion) {
	// Recorded ahead of the in-flight test below, because the analysis already running may be the
	// very one this is asking for -- the editor asks for the options and the argument hint about
	// one keystroke, and the second ask must not lose the first's claim on the result.
	if (p_is_completion) {
		completion_refresh_path = p_path;
		completion_refresh_source = p_normalized_source;
	}

	// The analysis in flight is already for this exact text. Godot validates the same unchanged
	// buffer several times over while one runs, and queueing behind it would buy the same answer
	// a second time -- putting a whole extra analysis between a save and the result it settles on.
	if (p_path == in_flight_path && p_normalized_source == in_flight_source) {
		return;
	}

	// Newest buffer wins *within its kind*: while an analysis runs the editor keeps typing, and
	// every intermediate state is worth less than the one the author is looking at now. Across the
	// two kinds nothing displaces anything, because a completion buffer and the author's own text
	// are different questions with different consumers -- which is what the second slot is for.
	if (p_is_completion) {
		pending_completion_path = p_path;
		pending_completion_source = p_normalized_source;
		has_pending_completion_check = true;
	} else {
		pending_check_path = p_path;
		pending_check_source = p_normalized_source;
		has_pending_check = true;
	}

	// Queued, not started. Nothing that describes a class joins the analysis thread any more --
	// since ABI v7 they answer from the snapshot the last one left -- but an analysis still blocks
	// the VM for its whole length, so one begun in the middle of the editor's work is the pump and
	// every `@tool` instance stopped for ~750 ms of it. _frame starts it once the frame's own work
	// is done, and _frame is also the only thing that polls, so a buffer superseded before the next
	// one costs nothing at all.
}

void VerseProjectState::start_pending_check() {
	if (!has_pending_check && !has_pending_completion_check) {
		return;
	}

	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr || !runtime->is_host_loaded() || runtime->is_check_project_busy()) {
		return;
	}

	// The completion buffer first when both are waiting: a popup and an argument hint are blocked
	// on it and are drawing nothing meanwhile, where the author's own buffer feeds a gutter that is
	// still showing the last analysis' diagnostics. Each kind holds only its newest buffer, so
	// preferring one delays the other by a single analysis and can never queue a third.
	const bool completion = has_pending_completion_check;
	const String path = completion ? pending_completion_path : pending_check_path;
	const String source = completion ? pending_completion_source : pending_check_source;

	const String globalized = ProjectSettings::get_singleton()->globalize_path(path);
	if (runtime->begin_check_project(globalized, source) != OK) {
		return;
	}

	// The host has taken this text, so it is what the next result answers for.
	in_flight_path = path;
	in_flight_source = source;
	in_flight_is_completion = completion;
	if (completion) {
		has_pending_completion_check = false;
	} else {
		has_pending_check = false;
	}
}

void VerseProjectState::flush_pending_check() {
	if (!has_pending_check && !has_pending_completion_check) {
		return;
	}

	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr || !runtime->is_host_loaded() || !runtime->host_has_compiler()) {
		has_pending_check = false;
		has_pending_completion_check = false;
		return;
	}

	// The completion slot first, in the order start_pending_check prefers them and for the same
	// reason. One flush runs one analysis and leaves the other slot for _frame; probe_complete is
	// what makes that enough, because it flushes once per caret and each caret queues one buffer.
	const bool completion = has_pending_completion_check;
	const String path = completion ? pending_completion_path : pending_check_path;
	const String source = completion ? pending_completion_source : pending_check_source;
	if (completion) {
		has_pending_completion_check = false;
	} else {
		has_pending_check = false;
	}

	// The synchronous entry point, which is what makes this a flush rather than a second queue: it
	// blocks on whatever the background thread is doing and then analyses.
	Dictionary errors_by_globalized;
	runtime->check_project(ProjectSettings::get_singleton()->globalize_path(path), source, &errors_by_globalized);

	analyzed_source_by_path[path] = source;
	// The snapshot the host now holds changed, whether or not this buffer was a completion one --
	// see analysis_epoch. This never touches description_epoch: a probe_hover/probe_complete flush
	// has no live scripts of its own to describe, which matches poll_check never doing so here
	// either (only its non-completion branch does).
	analysis_epoch.advance();
	record_diagnostics(errors_by_globalized);
}

void VerseProjectState::poll_check() {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr || !runtime->is_host_loaded()) {
		return;
	}

	Dictionary errors_by_globalized;
	if (runtime->poll_check_project(&errors_by_globalized)) {
		// Only now does the host hold this text, so only now may a validate answer from cache.
		// Recorded for a completion buffer too, and that is the point: the entry says which text
		// the host is describing, so `_validate` comparing the author's real buffer against it
		// queues the ordinary analysis that puts the diagnostics back, and a hover declines in the
		// meantime rather than trusting loci measured against a spliced-in placeholder.
		analyzed_source_by_path[in_flight_path] = in_flight_source;
		// The whole-project snapshot the host holds moved, whichever buffer produced it -- see
		// analysis_epoch.
		analysis_epoch.advance();

		if (in_flight_is_completion) {
			// Everything below describes the author's file to the author. This analysis was of a
			// line they are halfway through typing -- the placeholder resolves to nothing, so its
			// diagnostics are an unknown identifier they did not write -- and drawing that would
			// be worse than drawing nothing. The answer it was asked for is the program it left
			// behind, which vh_complete_symbol reads on the way back through. description_epoch does
			// not move here either, for the same reason: a script's exports and documentation must
			// not be re-derived from a line nobody finished writing.
			completion_refresh_pending = completion_refresh_path == in_flight_path
					&& completion_refresh_source == in_flight_source;
		} else {
			// The program every script may honestly describe itself against moved, ahead of the
			// per-script loop below so each one's own comparison against description_epoch_value()
			// already reads stale -- which is what lets analysis_landed() skip a script this landing
			// was not for and still have that script's exports and documentation catch up next time
			// they are asked for, without this loop having to reach it directly (B13, B26, B39).
			description_epoch.advance();

			language.editor_refresh_pending = record_diagnostics(errors_by_globalized) || language.editor_refresh_pending;
			language.refresh_script_warnings(in_flight_path);

			// The program the last attempt was made against is gone, so the answer may have
			// changed. _frame is what acts on it, for the reason the refresh below is deferred.
			language.docs_refresh_attempted = false;

			// Every script whose compile() declined to wait for this. Told one at a time rather
			// than only the analysed file's script, because a save can be waiting on a result its
			// own buffer did not start. Snapshotted: telling a script republishes its export list,
			// and Godot is free to drop a script while that runs.
			const std::vector<VerseScript *> scripts = language.live_scripts;
			for (VerseScript *script : scripts) {
				language.editor_refresh_pending = script->analysis_landed() || language.editor_refresh_pending;
			}
		}

		in_flight_path = String();
		in_flight_source = String();
		in_flight_is_completion = false;
	}
}

bool VerseProjectState::record_diagnostics(const Dictionary &p_diagnostics_by_globalized) {
	PackedStringArray previous = flattened_diagnostics(diagnostics_by_path);
	previous.append_array(flattened_diagnostics(compiler_warnings_by_path));

	diagnostics_by_path.clear();
	compiler_warnings_by_path.clear();

	const Array reported = p_diagnostics_by_globalized.keys();
	for (int64_t i = 0; i < reported.size(); i++) {
		const String globalized = reported[i];
		const String path = path_by_globalized.has(globalized) ? String(path_by_globalized[globalized]) : globalized;
		const TypedArray<Dictionary> filed = p_diagnostics_by_globalized[globalized];

		// The host reports the absolute path it was handed, but the script editor compares an
		// error's path against the *script's* -- `res://scripts/mover.verse` -- and moves every
		// error that does not match into its depended-errors list. Those are listed but never
		// marked: the line highlight and the error bar both read the list this filters.
		//
		// The same dictionaries, not copies: the build logs what it filed after this has run,
		// which is how the log carries the path and the explanations added below.
		TypedArray<Dictionary> errors;
		TypedArray<Dictionary> warnings;
		for (int64_t e = 0; e < filed.size(); e++) {
			Dictionary entry = filed[e];
			entry["path"] = path;
			const int64_t severity = entry["severity"];
			if (severity == VH_SEVERITY_ERROR) {
				errors.push_back(entry);
			} else if (severity == VH_SEVERITY_WARNING) {
				warnings.push_back(entry);
			}
			// An info has no row in the editor; the build's log is where it is read.
		}
		language.explain_skipped_members(path, errors);
		language.note_missing_imports(path, errors);
		diagnostics_by_path[path] = errors;
		if (!warnings.is_empty()) {
			compiler_warnings_by_path[path] = warnings;
		}
	}

	PackedStringArray current = flattened_diagnostics(diagnostics_by_path);
	current.append_array(flattened_diagnostics(compiler_warnings_by_path));
	return current != previous;
}

TypedArray<Dictionary> VerseProjectState::diagnostics_for(const String &p_path) const {
	if (!diagnostics_by_path.has(p_path)) {
		return TypedArray<Dictionary>();
	}
	return TypedArray<Dictionary>(diagnostics_by_path[p_path]);
}

TypedArray<Dictionary> VerseProjectState::compiler_warnings_for(const String &p_path) const {
	if (!compiler_warnings_by_path.has(p_path)) {
		return TypedArray<Dictionary>();
	}
	return TypedArray<Dictionary>(compiler_warnings_by_path[p_path]);
}
