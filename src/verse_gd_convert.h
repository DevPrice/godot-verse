#pragma once

// "Convert to Verse": one GDScript file in, one Verse file out, deterministically
// (docs/gdscript-conversion.md).
//
// Deterministic means the output is a function of the inputs below and nothing else -- no clock, no
// map ordered by pointer, no guess that depends on what else happened to be loaded. The editor half
// (`verse_convert_menu.cpp`) gathers those inputs from the project; everything here is pure and has
// no godot-cpp dependency, so the units layer tests it against fixtures without Godot.
//
// What cannot be translated faithfully is **marked, not refused**: the statement becomes a
// `# TODO(convert): <why>` comment carrying the GDScript it replaced, the file is still written, and
// the note is returned so the editor can list it. The one refusal is a file that does not parse.

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

struct VerseGdNote {
	// 1-based GDScript line; 0 for the file as a whole.
	int line = 0;
	// True when the output carries a `# TODO(convert):` for this note, false for something the
	// author should know but that the output handles (a renamed member, an initialiser moved).
	bool todo = false;
	std::string message;
};

// A member whose name changed. The scene files store exported values and signal connections by
// name, so every one of these is also an edit to every scene that uses the script.
struct VerseGdRename {
	enum class Kind {
		Property,
		Method,
		Signal,
	};
	Kind kind = Kind::Property;
	std::string gdscript;
	std::string verse;
};

// One method of a script class, as Verse will see it.
struct VerseGdMethodInfo {
	std::string verse_name;
	std::vector<std::string> param_names; // Verse
	std::vector<std::string> param_types; // Verse types, `variant` where unknown
	std::vector<bool> param_optional; // passed by name, `?Name := value`
	std::string result_type; // Verse type, "void" for none
	bool suspends = false; // called without `await` it has to be `spawn`ed
};

// Another script class the file may name -- by `class_name` or by a `preload` of its path.
//
// The members are optional: they are what a conversion of *that* script said about itself
// (`VerseGdConvertResult::self`), which the editor has only when both are converted in one batch.
// Without them a member is assumed to be PascalCase, which is what both a converted script and a
// generated binding (R-INT-7) spell it, and its type is unknown.
struct VerseGdScriptClass {
	std::string verse_name; // `player`
	std::string godot_base; // the nearest Godot class it extends, `Area2D`
	std::string path; // `res://player.gd`, so `preload("res://player.gd")` resolves too
	// A GDScript class this conversion leaves as GDScript: Verse reaches it through its generated
	// binding (R-INT-7), so a file that names one needs `using { /Godot.org/Bindings }`.
	bool is_binding = false;
	std::map<std::string, VerseGdMethodInfo> methods; // by GDScript name
	std::map<std::string, std::pair<std::string, std::string>> properties; // GDScript -> (Verse, type)
	std::map<std::string, std::pair<std::string, std::string>> signals; // GDScript -> (Verse, payload)
};

struct VerseGdConvertInput {
	std::string source;
	// The .gd file's name without extension, `player`.
	std::string file_stem;
	// What `$Path` and `%Unique` name, from the scenes that use the script: the node path as it is
	// written after the `$` (or `%Name`) -> the Godot class, or a script's `class_name`. A path
	// that is absent is a plain `node`.
	std::map<std::string, std::string> node_types;
	// Every other script class the project has, by `class_name`.
	std::map<std::string, VerseGdScriptClass> script_classes;
	// What other files pass to this one's untyped parameters, by GDScript method name and index,
	// as a Verse type: only hints every caller agreed on (`VerseGdConvertResult::observed_arguments`).
	std::map<std::string, std::map<int, std::string>> param_hints;
};

struct VerseGdConvertResult {
	// False when the GDScript did not parse; `error` and `error_line` say where, and nothing else
	// in the result is meaningful.
	bool ok = false;
	std::string error;
	int error_line = 0;

	std::string verse;
	// The name the .verse file must have, without extension. It is also the class's name, because
	// only the class named after its file can go on a node (CLAUDE.md, "Modules and names").
	std::string verse_stem;
	// What Godot will register the class as -- `verse_pascal_case(verse_stem)` -- or empty when the
	// GDScript had no `class_name` and so the Verse class carries no `@global_class`.
	std::string global_name;
	// The GDScript `class_name`, or empty.
	std::string gd_class_name;

	std::vector<VerseGdNote> notes;
	std::vector<VerseGdRename> renames;

	// This class as another script's conversion should see it, for a batch: converting each file a
	// second time with every other's `self` in its `script_classes` is what lets `main` know that
	// `hud.show_game_over()` suspends and has to be spawned.
	VerseGdScriptClass self;
	// The type of every argument this file passes to another script class's method, keyed by that
	// class's Verse name, then the GDScript method name and the argument's index. A batch merges
	// these across files and hands the ones that agree back as the callee's `param_hints`.
	std::map<std::string, std::map<std::string, std::map<int, std::set<std::string>>>> observed_arguments;

	int todo_count() const;
};

VerseGdConvertResult verse_gd_convert(const VerseGdConvertInput &p_input);

// Several files converted together, which is what the editor does with a multi-selection. Each is
// converted, then converted again knowing what the others said about themselves (`self`) and what
// they pass to each other's untyped parameters (`observed_arguments`), three rounds in all, so the
// result depends on the set of files and never on the order they were selected in. An input's
// `script_classes` entry whose `verse_name` is another input's stem is how the two are matched.
std::vector<VerseGdConvertResult> verse_gd_convert_batch(const std::vector<VerseGdConvertInput> &p_inputs);

// `speed` -> `Speed`, `_on_body_entered` -> `OnBodyEntered`, `MAX_SPEED` -> `MaxSpeed`. The rule
// for a member that is not one of Godot's virtuals, which keep Godot's spelling (`_Ready`).
std::string verse_gd_member_name(const std::string &p_gdscript_name);

// The Verse file stem for a GDScript file: its `class_name` in snake case when it has one, so the
// global name Godot registers survives the conversion (`class_name PlayerController` ->
// `player_controller.verse`, registered as `PlayerController`), and the file's own stem otherwise.
std::string verse_gd_stem(const std::string &p_file_stem, const std::string &p_class_name);

// The `class_name` a GDScript declares, read without parsing the whole file, or "".
std::string verse_gd_scan_class_name(const std::string &p_source);
// The class a GDScript `extends`, as written -- a class name or a quoted path -- or "".
std::string verse_gd_scan_extends(const std::string &p_source);

// --- Scenes and resources ---------------------------------------------------------------------
//
// A `.tscn`/`.tres` names a script by path in an `[ext_resource]`, stores each exported value under
// the property's name, and each connection under its signal's and method's names. Converting a
// script changes all three, so every text resource in the project is rewritten with it. A binary
// `.scn`/`.res` cannot be, and the editor says so.

// Reads another resource's text by `res://` path, or answers "" when there is none -- the way an
// instanced scene's root script is found, which is where a connection *from* an instance to the
// scene that holds it gets the name of the signal it is connecting.
using VerseGdResourceReader = std::function<std::string(const std::string &p_path)>;

// The node types a script sees through `$`, from one scene that uses it: every node below each node
// carrying `p_script_path`, keyed the way `$` writes it relative to that node. `%Unique` names are
// keyed with their `%`. An instanced child is its instanced scene's root type; a scripted node with
// a `class_name` is that name (read from the script through `p_read`).
std::map<std::string, std::string> verse_gd_scene_node_types(const std::string &p_scene_text,
		const std::string &p_script_path, const VerseGdResourceReader &p_read);

struct VerseGdScriptMove {
	std::string old_path; // res://player.gd
	std::string new_path; // res://player.verse
	std::string old_class_name; // `Player`, or "" when the script had none
	std::string new_class_name; // what Godot registers the Verse class as, or ""
	std::vector<VerseGdRename> renames;
};

// The nearest class two node types share, for a path two scenes disagree about: `Button` and
// `Label` meet at `Control`. A script class is only ever its own nearest match.
std::string verse_gd_common_class(const std::string &p_a, const std::string &p_b);

// Every resource path a text resource references directly -- `[ext_resource path=...]`.
std::vector<std::string> verse_gd_resource_dependencies(const std::string &p_text);

// Rewrites one text resource for a batch of converted scripts: the `ext_resource` paths, the
// property keys in every section whose script is one of them (directly, or as the root of an
// instanced scene), and the connections' `signal=`/`method=` where the node at either end carries
// one. Answers the text unchanged when nothing in it refers to any of them.
std::string verse_gd_rewrite_resource(const std::string &p_text, const std::vector<VerseGdScriptMove> &p_moves,
		const VerseGdResourceReader &p_read);

// --- Other GDScript files -----------------------------------------------------------------------
//
// A GDScript that calls into a converted script by a member name that changed. Rewriting it is only
// sound where the receiver's type is *known* to be the converted class -- a variable, parameter or
// cast written with its `class_name` -- so that is all this rewrites; every other use of a renamed
// name is reported as a site for the author to look at, and never touched.

struct VerseGdCallerSite {
	int line = 0;
	std::string source; // the whole line, trimmed
	bool rewritten = false;
	std::string reason; // why it was not rewritten, when it was not
};

struct VerseGdCallerRewrite {
	std::string text;
	bool changed = false;
	std::vector<VerseGdCallerSite> sites;
};

// `p_node_types` is what `$` names in the caller, from its own scenes, the way the converter's input
// carries it -- which is what makes `$Player.start(...)` a known receiver.
VerseGdCallerRewrite verse_gd_rewrite_callers(const std::string &p_source, const std::vector<VerseGdScriptMove> &p_moves,
		const std::map<std::string, std::string> &p_node_types);
