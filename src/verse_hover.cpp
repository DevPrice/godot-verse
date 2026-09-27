#include "verse_hover.h"

#include "verse_api_lookup.h"
#include "verse_class_decl.h"
#include "verse_completion.h"
#include "verse_host_abi.h"
#include "verse_lexer.h"
#include "verse_runtime.h"
#include "verse_script.h"
#include "verse_script_language.h"
#include "verse_signature.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#ifdef TOOLS_ENABLED
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#endif

#include <cctype>
#include <string>
#include <vector>

using namespace godot;

namespace {

VerseRuntime *get_runtime() {
	return Object::cast_to<VerseRuntime>(Engine::get_singleton()->get_singleton("VerseRuntime"));
}

// The two live types are the only ones that serve both features: SCRIPT_LOCATION jumps but shows
// no tooltip at all, and the CLASS_* types route into Godot's own class documentation, which has
// nothing to say about a Verse definition. LOCAL_VARIABLE and LOCAL_CONSTANT build a tooltip out
// of doc_type/description, and the click path ignores `type` entirely -- it jumps on `location`
// alone, provided `class_name` is empty. Leaving class_name unset is therefore load-bearing.
void fill_binding_result(Dictionary &r_result, const VerseScriptLanguage::BindingInfo &p_binding) {
	r_result["result"] = (int64_t)OK;
	r_result["class_name"] = p_binding.script_class.is_empty() ? p_binding.godot_class : p_binding.script_class;
	if (!p_binding.script_path.is_empty()) {
		// **Both spellings, and that is not belt and braces.** Godot renamed this field: 4.7
		// reads a `Ref<Script>` off the result and 4.8 reads `script_path`
		// (`editor/script/editor_language.h`). A result that fills one of them is, in the other
		// editor, a location with *no script beside it* -- which ScriptTextEditor reads as a line
		// in the file being edited, so a click on a binding scrolled the open file to its own top
		// instead of opening the GDScript. The jump at the end of _lookup_code sets both.
		r_result["script"] = ResourceLoader::get_singleton()->load(p_binding.script_path);
		r_result["script_path"] = p_binding.script_path;
		r_result["location"] = (int64_t)0;
	}
}

// The lexer's kind at a byte column, so a probe row can say whether the word it hovered was code
// at all. Tokens arrive in increasing column order and each runs until the next one begins.
const char *verse_token_kind_name(VerseTokenKind p_kind) {
	switch (p_kind) {
		case VerseTokenKind::Identifier:
			return "identifier";
		case VerseTokenKind::Comment:
			return "comment";
		case VerseTokenKind::String:
			return "string";
		case VerseTokenKind::Escape:
			return "escape";
		case VerseTokenKind::Interpolation:
			return "interpolation";
		case VerseTokenKind::Number:
			return "number";
		case VerseTokenKind::Keyword:
			return "keyword";
		case VerseTokenKind::ControlKeyword:
			return "control_keyword";
		case VerseTokenKind::Attribute:
			return "attribute";
		case VerseTokenKind::Symbol:
			return "symbol";
		case VerseTokenKind::Function:
			return "function";
		case VerseTokenKind::FunctionDefinition:
			return "function_definition";
		case VerseTokenKind::Member:
			return "member";
		default:
			return "text";
	}
}

} // namespace

// The two live types are the only ones that serve both features: SCRIPT_LOCATION jumps but shows
// no tooltip at all, and the CLASS_* types route into Godot's own class documentation, which has
// nothing to say about a Verse definition. LOCAL_VARIABLE and LOCAL_CONSTANT build a tooltip out
// of doc_type/description, and the click path ignores `type` entirely -- it jumps on `location`
// alone, provided `class_name` is empty. Leaving class_name unset is therefore load-bearing.
Dictionary VerseHover::lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const {
	Dictionary result;
	result["result"] = (int64_t)ERR_UNAVAILABLE;
	result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_LOCAL_VARIABLE;

	// GDScript answers a class name out of ClassDB before it parses anything; this is the same trick
	// over the mirror's static table, needing no marker, no analysis and no host call. Every refusal
	// below routes through it instead of returning `result` bare, so a hover on a mirrored class name
	// survives no host, no build yet, a stale buffer, a busy analysis and a lookup that named nothing
	// -- but never preempts an analysed answer, because it only runs where the function was about to
	// give up: a local or member that happens to share a class's spelling still resolves to itself.
	auto refuse_or_mirrored_class = [&]() -> Dictionary {
		if (const char *godot_class = p_symbol.is_empty() ? nullptr : godot_doc_class_for(p_symbol)) {
			result["result"] = (int64_t)OK;
			result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS;
			result["class_name"] = String(godot_class);
			return result;
		}
		// A binding survives the same five cases, and for the same reason: the table is the
		// consumer's own, so it answers with no host, no build, a stale buffer and a busy
		// analysis alike. It is asked after the mirror because the mirror cannot be shadowed --
		// a binding for a class the mirror carries is never generated.
		if (const VerseScriptLanguage::BindingInfo *binding = p_symbol.is_empty() ? nullptr : language.binding_for(p_symbol)) {
			result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS;
			fill_binding_result(result, *binding);
		}
		return result;
	};

	VerseRuntime *runtime = get_runtime();
	if (!language.is_built() || runtime == nullptr || !runtime->is_host_loaded()) {
		return refuse_or_mirrored_class();
	}

	// The editor marks the cursor by splicing U+FFFF into the buffer it hands over, and that is
	// the only place the position arrives: p_symbol is just the word under the pointer, which
	// cannot tell two same-named locals in different functions apart. The underline path asks
	// about the mouse rather than the caret and hands over an empty string when the pointer is
	// off the end of the text, so a missing marker is ordinary rather than a fault.
	const int64_t marker = p_code.find(String::chr(0xFFFF));
	if (marker < 0) {
		return refuse_or_mirrored_class();
	}

	// A comment is prose and a string is data. Neither is code, so nothing under the pointer
	// there names a definition, and the refusal has to be the bare one -- the fallback below
	// resolves a bare *word*, and a comment is made of words.
	//
	// This matters far more here than the same shortcut does in GDScript. The mirror spells
	// Godot's classes in lowercase, so `node`, `script`, `label`, `engine`, `window`, `panel`,
	// `animation` and `resource` are all ordinary English a comment is likely to contain, and
	// every one of them used to pop Godot's class documentation over a sentence. GDScript does
	// the same lookup ahead of its own parse -- `if (GDScriptAnalyzer::class_exists(p_symbol))`,
	// gdscript_editor.cpp -- and almost never meets it, because its class names are PascalCase
	// and nobody writes `Node` in a sentence.
	//
	// Not interpolation: `"{Score}"` is code inside a string, and verse_position_in_string says
	// so, because completion inside one is wanted for the same reason a tooltip is.
	if (completing_in_comment(p_code, marker) || completing_in_string(p_code, marker)) {
		return result;
	}

	const String before = p_code.substr(0, marker);
	const int64_t line = before.count("\n");
	const int64_t line_start = before.rfind("\n") + 1;
	// Godot counts the column in characters and the compiler counts it in utf8 bytes; one
	// non-ASCII character earlier on the line is enough to make them disagree.
	int64_t column = before.substr(line_start).utf8().length();

	// A word is hovered for one column more than it has characters. TextEdit::get_word answers
	// for `words[i] <= column && words[i + 1] >= column`, so the caret position *after* the last
	// character belongs to the word -- and that is where the pointer is whenever it is over the
	// right half of the last glyph, which is half the pixels of every identifier's last letter.
	// The compiler resolves nothing there, because the position is past the definition's source
	// range, so a hover arriving with a perfectly good symbol answered nothing at all. Stepping
	// back onto the last character is what makes a word answer alike wherever in it the pointer
	// is. The same column decides a ctrl+click, which missed for the same reason.
	//
	// Only where the character behind the cursor is part of a word and the one ahead is not:
	// mid-word is already inside the range, and a cursor with a space behind it is not hovering
	// anything. A word character is ASCII, so the step is one byte as well as one character.
	const auto word_char = [](char32_t p_char) {
		return (p_char >= 'a' && p_char <= 'z') || (p_char >= 'A' && p_char <= 'Z') ||
				(p_char >= '0' && p_char <= '9') || p_char == '_';
	};
	const char32_t ahead = marker + 1 < p_code.length() ? p_code[marker + 1] : U'\0';
	const char32_t behind = before.length() > line_start ? before[before.length() - 1] : U'\0';
	if (column > 0 && !word_char(ahead) && word_char(behind)) {
		column--;
	}

	// Answering from an analysis that predates the edit would be worse than not answering: the
	// loci below an inserted row are all shifted, so the jump lands confidently on the wrong
	// line. This is the same predicate check_buffer uses to decide a re-analysis is unnecessary.
	const String normalized = verse_newline_normalized(before + p_code.substr(marker + 1));
	if (!language.analyzed_sources().has(p_path) || String(language.analyzed_sources()[p_path]) != normalized) {
		return refuse_or_mirrored_class();
	}

	// The host blocks on an in-flight analysis before touching the semantic program, and this
	// runs on the editor's thread. An analysis of some other file is the one case where the
	// buffer can be current and the host still busy; declining costs an underline for a frame.
	if (runtime->is_check_project_busy()) {
		return refuse_or_mirrored_class();
	}

	const String globalized = ProjectSettings::get_singleton()->globalize_path(p_path);
	bool not_ready = false;
	const Dictionary found = runtime->lookup_symbol(globalized, (int32_t)line, (int32_t)column, &not_ready);
	if (not_ready) {
		// A build since the analysis that saw this buffer has taken its AST away. The text still
		// matches, so nothing else would ask for another analysis; ask for one, and the next hover
		// over the same word answers.
		language.request_check(p_path, normalized, VerseProjectState::CheckKind::ORDINARY);
	}
	if (found.is_empty()) {
		return refuse_or_mirrored_class();
	}

	// A parameter of the function that declares it, rather than a member or a local. It has no
	// documentation of its own and its source line is the line its whole function is declared on,
	// so the comment "above" it is the function's -- which is why the two places below that read
	// a comment skip it.
	const bool is_parameter = bool(found["is_parameter"]);

	result["result"] = (int64_t)OK;
	// Godot's two local results are its only ones that carry prose, so everything with no class
	// to name lands on one of them, and which one is the whole of the label: "Local Constant" or
	// "Local Variable" (editor_help.cpp). Only a `var` is mutable in Verse -- a parameter is not,
	// and neither is a plain binding -- so the constant is the right label for everything else.
	// GDScript spells a parameter the other way because there a parameter is reassignable, which
	// is why this does not simply follow gdscript_editor.cpp's SuiteNode::Local walk.
	result["type"] = (int64_t)(bool(found["is_var"])
					? ScriptLanguageExtension::LOOKUP_RESULT_LOCAL_VARIABLE
					: ScriptLanguageExtension::LOOKUP_RESULT_LOCAL_CONSTANT);
	result["doc_type"] = found["type"];

	// A parameter's documentation is the host's to give, because the host reads the comment written
	// against the parameter itself -- the inline `<# doc #> P:t`, or a `#` line above it on its own
	// line -- where the consumer's line-based reader below would read the function's comment for a
	// parameter that shares the function's declaration line. So the host's `doc` is used for a
	// parameter at its declaration (which returns early just below) and at every use, and the
	// source-reading path leaves a parameter alone (B41).
	if (is_parameter) {
		const String param_doc = found["doc"];
		if (!param_doc.is_empty()) {
			result["description"] = verse_doc_bbcode(param_doc);
		}
	}

	// A definition that came from the mirrored Godot API is described by Godot's own class
	// documentation, which is better than anything this could say and is already installed. Both
	// paths key off class_name: the click sends it to the help viewer instead of jumping, and the
	// tooltip fetches the description out of the same doc data. Naming it here is what turns a
	// Verse identifier into a Godot doc page, and there is no source in the project to jump to
	// anyway -- the generated API is compiled from the engine tree.
	const String found_name = found["name"];
	const String found_owner = found["owner"];
	const int64_t kind = found["kind"];

	// An override means something the declaration itself does not say. Only at a declaration: a
	// call site already resolves to the implementation that will run, and sending that to the
	// parent would be wrong rather than merely unhelpful.
	const String overridden_owner = found["overridden_owner"];
	const bool is_definition = bool(found["is_definition"]);
	const bool overrides_something = is_definition && !overridden_owner.is_empty();

	// A parameter where it is declared has nothing left to add: its name and its type are both on
	// the line the pointer is over, there is no comment of its own to read, and a jump has
	// nowhere to go, because the declaration is that line. The type is still worth drawing -- the
	// tooltip says "Local Variable Delta: float" and stops there.
	if (is_parameter && is_definition) {
		return result;
	}

	// The file a definition was written in. The buffer for the file being edited may be ahead of
	// what is on disk; anything else is at worst as stale as the analysis that pointed here.
	auto source_at = [&](const String &p_definition_path) -> String {
		if (p_definition_path == globalized) {
			return normalized;
		}
		const String res_path = language.res_path_by_globalized().get(p_definition_path, String());
		return verse_newline_normalized(FileAccess::get_file_as_string(
				res_path.is_empty() ? p_definition_path : res_path));
	};

	// The comment block above a definition, wherever it was written. A file the project does not
	// own -- Godot.native.verse in the engine tree -- cannot be jumped to, but its comment is
	// still the best description of what a script is overriding.
	auto comment_at = [&](const String &p_definition_path, int64_t p_line) -> String {
		if (p_line < 0 || p_definition_path.is_empty()) {
			return String();
		}
		return verse_doc_comment_above(source_at(p_definition_path), p_line);
	};

	const int64_t own_line = found["line"];
	const String own_path = found["path"];
	const int64_t overridden_line = found["overridden_line"];
	const String overridden_path = found["overridden_path"];
	// A parameter's source line is the line its whole function is declared on, so the comment
	// "above" it is the function's -- describing an argument with the method's prose. It has no
	// documentation of its own, and GDScript gives one none either.
	const String own_description = is_parameter ? String() : comment_at(own_path, own_line);

	// A type's own name has no type to spell -- LookupSymbol fills Type for a CDataDefinition or
	// a CFunction, and a class, a struct, an interface, an enum and a module are none of those --
	// so the tooltip drew the label, the name, a colon and nothing. The declaration is the only
	// place the word is: the kind cannot supply it either, since a struct and an interface both
	// arrive as VH_LOOKUP_CLASS.
	//
	// Only for a file the project owns, and that guard is what keeps this off the common path: a
	// mirrored class carries no type either, and its declaration is 2 MB of generated Verse in
	// the engine tree -- read on every hover over `node2d`, to produce a word the Godot page two
	// lines below replaces.
	//
	// The label beside it stays "Local Constant", which is wrong and has nowhere better to go:
	// Godot's results are a location, eight class members and the two locals (B31).
	const bool names_a_type = kind == VH_LOOKUP_CLASS || kind == VH_LOOKUP_ENUM || kind == VH_LOOKUP_MODULE;
	if (names_a_type && String(result["doc_type"]).is_empty() &&
			(own_path == globalized || language.res_path_by_globalized().has(own_path))) {
		result["doc_type"] = String(verse_scan_type_keyword(
				source_at(own_path).utf8().get_data(), found_name.utf8().get_data()).c_str());
	}

	if (kind == VH_LOOKUP_CLASS) {
		if (const char *godot_class = godot_doc_class_for(found_name)) {
			result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS;
			result["class_name"] = String(godot_class);
			return result;
		}
		// A class the project declares. `player` in main.verse was a "Local Constant" with an
		// empty box under it, where `area2d` beside it answered Godot's documentation -- and the
		// difference was only that one of them is in the mirror's table.
		//
		// The class a file is named after is the one VerseScript registers a doc for, under this
		// exact name (_get_doc_class_name), so this is the same arrangement that already makes a
		// hover on one of its *members* say "Property" with the comment above the declaration.
		// No early return: the doc is a script doc, so the click path skips the help viewer
		// (script_text_editor.cpp tests is_script_doc) and needs the location below to jump with.
		//
		// A second class in the same file is deliberately not included. Nothing registers a doc
		// for one, so CLASS would draw an empty box, where the local result at least carries the
		// comment above it.
		//
		// Under the module-qualified name, which is what the doc is registered as: `left/widget`
		// for a class under a `.vmodule`, and only the file-named class registers one. The name
		// comes from the file the definition was written in, because at its own declaration a
		// class's owner is the module or the snippet around it and never spells the class.
		const String own_res_path = own_path == globalized ? p_path : String(language.res_path_by_globalized().get(own_path, String()));
		const String own_qualified = own_res_path.is_empty() ? String() : language.qualified_class_name(own_res_path);
		if (own_qualified.get_file() == found_name && language.script_class_names().has(own_qualified)) {
			language.ensure_script_doc_published(own_qualified);
			result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS;
			result["class_name"] = own_qualified;
		} else if (const VerseScriptLanguage::BindingInfo *binding = language.binding_for(found_name)) {
			result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS;
			fill_binding_result(result, *binding);
			return result;
		}
	} else if (kind == VH_LOOKUP_ENUM) {
		if (const verse_api::enum_mapping *mirrored = godot_enum_for(found_name)) {
			result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS_ENUM;
			result["class_name"] = String(mirrored->godot_class);
			result["class_member"] = String(mirrored->godot_enum);
			return result;
		}
	} else if (kind == VH_LOOKUP_TYPE_ALIAS) {
		if (const char *godot_class = godot_doc_class_for_primitive(found_name)) {
			result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS;
			result["class_name"] = String(godot_class);
			return result;
		}
	} else if (kind == VH_LOOKUP_MODULE) {
		const String statics_class = godot_statics_class_for(found_name);
		if (!statics_class.is_empty()) {
			result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS;
			result["class_name"] = statics_class;
			return result;
		}
	} else if (kind == VH_LOOKUP_FUNCTION || kind == VH_LOOKUP_DATA) {
		// A global is a member of nothing, so the method table has no owner to answer it by, and
		// the file it is declared in is in the engine tree rather than in the project -- leaving
		// it, before this, described by its own comment and with nowhere to click through to.
		if (is_godot_package_global(found_owner, own_path)) {
			// An extension method first, because the two overlap by name and only one of them
			// has a receiver: `Snapped` is `(V:vector2).Snapped(Step)` *and* the scalar
			// `Snapped(X, Step)`, and Godot documents them in two different places. Asking about
			// the receiver is what separates them -- and is the only way to reach any of
			// GodotMath's 159 methods, whose owner is the file rather than the type.
			const String extension_name = verse_extension_method_name(found_name);
			if (!extension_name.is_empty()) {
				const verse_api::method_mapping *on_receiver = godot_method_for(
						verse_receiver_type(result["doc_type"]), extension_name);
				if (on_receiver != nullptr) {
					result["type"] = lookup_result_for(on_receiver->kind);
					result["class_name"] = String(on_receiver->godot_class);
					result["class_member"] = String(on_receiver->godot_method);
					return result;
				}
			}
			if (const verse_api::global_mapping *global = godot_global_for(found_name)) {
				result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS_METHOD;
				result["class_name"] = String(global->godot_class);
				result["class_member"] = String(global->godot_function);
				return result;
			}
			const String singleton_class = godot_singleton_class_for(found_name);
			if (!singleton_class.is_empty()) {
				result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS;
				result["class_name"] = singleton_class;
				return result;
			}
			// A parametric type is a function to the compiler -- `typed_array(t)` resolves to the
			// one that answers the type -- so the name of one arrives here rather than in the
			// class arm above, and is the same type either way.
			if (const char *godot_class = godot_doc_class_for(found_name)) {
				result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS;
				result["class_name"] = String(godot_class);
				return result;
			}

			// An extension method on a Verse type that no Godot page covers -- `event(t).Emit`,
			// `signal_ref.Subscribe`, `variant.AsInt`. Register a page for it and answer
			// CLASS_METHOD, so it draws as a method with its arguments and its own comment rather
			// than as a "Local Constant" whose type is the whole function type (B40). Only a
			// function, and only with a receiver to name the page after; a free function keeps the
			// local result, and so does any hover in a headless run, where publish_api_method finds
			// no script editor to register with and answers empty.
			const String api_extension = verse_extension_method_name(found_name);
			if (kind == VH_LOOKUP_FUNCTION && !api_extension.is_empty()) {
				const String function_type = result["doc_type"];
				const String receiver = verse_receiver_type(function_type);
				if (!receiver.is_empty()) {
					const String doc_class = publish_api_method(receiver, api_extension,
							function_type, verse_doc_bbcode(String(found["doc"])));
					if (!doc_class.is_empty()) {
						result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS_METHOD;
						result["class_name"] = doc_class;
						result["class_member"] = api_extension;
						return result;
					}
				}
			}
		}

		// A mirrored property is a var, so the kind alone cannot separate it from a script's own
		// @editable member; the owner does, since only a mirrored class appears in the table.
		const verse_api::method_mapping *method = godot_method_for(found_owner, found_name);

		// An override is looked up under what it overrides: a script's own class is never in the
		// table, and `Ready<override>()` means Godot's _ready however the script spells it. Only
		// when the override says nothing itself, though -- routing into Godot's documentation
		// hands the tooltip to Godot's doc data too, which would throw away prose written here.
		if (method == nullptr && overrides_something && own_description.is_empty()) {
			method = godot_method_for(overridden_owner, found_name);
		}
		if (method != nullptr) {
			// The generated table's kind rather than the Verse one. A signal accessor and a
			// static are both Verse functions and a constant and a property are both Verse data,
			// so asking by the Verse spelling looked up a *method* named `timeout` -- which Godot
			// does not have, because what it has is a signal, and the tooltip came back empty.
			result["type"] = (int64_t)lookup_result_for(method->kind);
			result["class_name"] = String(method->godot_class);
			result["class_member"] = String(method->godot_method);
			return result;
		}

		// A member of a class the project itself declares is a property or a method, and saying so
		// is the whole difference between the editor calling it that and calling it a local
		// variable. It takes naming the class it belongs to, which is only safe because that name
		// is registered as a *script* doc: the click path diverts a class_name into the help viewer
		// only when the class is one of Godot's own, so this one still falls through to the jump
		// below. The description comes from the same registered doc rather than from `description`,
		// which Godot reads for the local results alone.
		//
		// Only a class: a parameter's owner is the function that declares it, and a local's is a
		// block. Neither is a property of anything, and both keep the local results, which are the
		// only ones that can carry prose this has read out of the source itself.
		// A binding's member, under the Godot name it calls rather than the Verse one it is
		// written as: `Hit` documents nothing, `hit` is the method GDScript declared. A signal
		// is a `signal(t)` data member, so the Verse kind cannot tell the two apart and the
		// table does -- the same reason the mirror's own arm above reads its table's kind.
		if (const VerseScriptLanguage::BindingInfo *binding = language.binding_for(found_owner)) {
			const HashMap<String, String>::ConstIterator method_found = binding->methods.find(found_name);
			const HashMap<String, String>::ConstIterator signal_found = binding->signals.find(found_name);
			if (method_found != binding->methods.end() || signal_found != binding->signals.end()) {
				const bool is_signal = method_found == binding->methods.end();
				result["type"] = (int64_t)(is_signal
								? ScriptLanguageExtension::LOOKUP_RESULT_CLASS_SIGNAL
								: ScriptLanguageExtension::LOOKUP_RESULT_CLASS_METHOD);
				fill_binding_result(result, *binding);
				result["class_member"] = is_signal ? signal_found->value : method_found->value;
				return result;
			}
		}

		//
		// The owner is module-qualified for a script class -- `left/widget`, the same string
		// _get_doc_class_name registers -- because Godot looks the doc up by exactly that name.
		// And the doc is registered now if it is not current, because Godot draws the tooltip
		// from it and nothing else: the description filled below is read for a local result
		// alone (B38).
		if (language.script_class_names().has(found_owner)) {
			language.ensure_script_doc_published(found_owner);
			result["type"] = (int64_t)(kind == VH_LOOKUP_FUNCTION
							? ScriptLanguageExtension::LOOKUP_RESULT_CLASS_METHOD
							: ScriptLanguageExtension::LOOKUP_RESULT_CLASS_PROPERTY);
			result["class_name"] = found_owner;
			result["class_member"] = found_name;
		}
	}

	String description = own_description;
	if (description.is_empty() && overrides_something) {
		description = comment_at(overridden_path, overridden_line);
	}

	// The host's own reading, last, for the two families this side can say nothing about.
	//
	// Verse's library documents itself with a `@doc("...")` *attribute* rather than a comment --
	// 132 of them across /Verse.org/Verse -- so the text above the declaration is an attribute
	// line and comment_at finds nothing however well it reads. And a definition in a package the
	// project does not own has a path in the engine tree, which is not a file an editor should
	// be opening on a hover keystroke. `Sqrt`, `Concatenate` and `event` all drew a type and an
	// empty box before this.
	//
	// Last rather than first, because reading the file beats it twice over: it is current with an
	// unsaved edit where the host describes the text the last analysis saw, and the parser hangs
	// a comment off whichever node begins the construct, so for a member behind four lines of
	// `@editable` the host's answer is empty where re-reading is not.
	if (description.is_empty() && !is_parameter) {
		description = found["doc"];
		if (description.is_empty() && overrides_something) {
			description = found["overridden_doc"];
		}
	}
	// Godot reads a local result's description as its own doc BBCode and draws every `\n` as a
	// paragraph, so the prose goes over converted, not as the reader joined it.
	if (!description.is_empty()) {
		result["description"] = verse_doc_bbcode(description);
	}

	// A local result that carries no type, no description and no class is a tooltip with nothing
	// in it: Godot draws the label and the symbol and stops (editor_help.cpp). Where there is also
	// nowhere to click through to, the honest answer is no tooltip -- which is what GDScript
	// leaves for the same shape of symbol, and is the difference between a hover that says nothing
	// and a hover that does not interrupt.
	auto hide_if_empty = [&]() -> Dictionary {
		const bool draws_nothing = String(result["doc_type"]).is_empty()
				&& String(result.get("description", String())).is_empty()
				&& String(result.get("class_name", String())).is_empty();
		if (draws_nothing) {
			result["result"] = (int64_t)ERR_UNAVAILABLE;
		}
		return result;
	};

	// At a declaration that overrides, the parent is the only useful destination: this
	// definition's own line is the one the cursor is already on.
	const int64_t target_line = overrides_something ? overridden_line : own_line;
	const String target_path = overrides_something ? overridden_path : own_path;
	if (target_line < 0 || target_path.is_empty()) {
		return hide_if_empty();
	}

	// A location with no script beside it is read as a line in the file being edited, so a
	// cross-file definition we cannot name gets no location at all rather than a jump to that
	// line of the wrong file.
	const bool same_file = target_path == globalized;
	const String target_res_path = same_file ? p_path : String(language.res_path_by_globalized().get(target_path, String()));
	if (target_res_path.is_empty()) {
		return hide_if_empty();
	}

	result["location"] = target_line + 1;
	if (!same_file) {
		result["script"] = ResourceLoader::get_singleton()->load(target_res_path);
		result["script_path"] = target_res_path;
	}
	return result;
}

// Every hover the editor could produce over one file. Three things the editor does have to be
// reproduced exactly, or the rows describe a hover nobody can perform:
//
//   - the word comes from TextEdit::get_word, whose test is `start <= column && end >= column`,
//     so the column one *past* a word's last character still hovers that word. That off-by-one
//     is a real mouse position -- the right half of the last glyph -- and it is the likeliest
//     place for one column of a word to answer differently from the rest.
//   - the marker is spliced at the hovered column rather than at the word's start, which is what
//     CodeEdit's get_text_with_cursor_char does, so a hover in the middle of an identifier asks
//     about a position inside it.
//   - the symbol handed over is the whole word wherever in it the pointer was.
//
// Columns that answer alike collapse into one row, so a word every column agrees about is one
// row and a word they do not is as many rows as it has distinct answers -- which is the finding.
TypedArray<Dictionary> VerseHover::probe(const String &p_path) {
	TypedArray<Dictionary> rows;

	language.ensure_project_built();

	// Every row below is a position question, and a build leaves the host with no AST to resolve
	// one against. In the editor the analysis that puts it back starts from _frame; this runs
	// inside one call and has no frames, so it runs the analysis itself.
	language.flush_pending_check();

	const String file = FileAccess::get_file_as_string(p_path);
	if (FileAccess::get_open_error() != OK) {
		return rows;
	}
	const String source = verse_newline_normalized(file);
	const CharString source_utf8 = source.utf8();
	const std::string all(source_utf8.get_data(), (size_t)source_utf8.length());

	VerseRuntime *runtime = get_runtime();
	const bool host_can_answer = runtime != nullptr && runtime->is_host_loaded();
	const String globalized = ProjectSettings::get_singleton()->globalize_path(p_path);

	const auto is_word = [](char p_char) {
		const unsigned char c = (unsigned char)p_char;
		return std::isalnum(c) != 0 || c == '_';
	};

	VerseLexState state;
	int64_t line = 0;
	for (size_t line_start = 0; line_start <= all.size();) {
		size_t newline = all.find('\n', line_start);
		const size_t line_end = newline == std::string::npos ? all.size() : newline;
		const std::string text = all.substr(line_start, line_end - line_start);

		std::vector<VerseToken> tokens;
		verse_lex_line(text, state, tokens);
		auto kind_at = [&tokens](size_t p_column) {
			const char *name = "text";
			for (const VerseToken &token : tokens) {
				if ((size_t)token.column > p_column) {
					break;
				}
				name = verse_token_kind_name(token.kind);
			}
			return String(name);
		};

		for (size_t at = 0; at < text.size();) {
			if (!is_word(text[at])) {
				at++;
				continue;
			}
			size_t end = at;
			while (end < text.size() && is_word(text[end])) {
				end++;
			}
			const String symbol = String::utf8(text.data() + at, (int64_t)(end - at));

			Dictionary previous;
			String previous_key;
			int64_t run_start = -1;
			// `<= end` because the column one past the word is still inside get_word's range.
			for (size_t column = at; column <= end; column++) {
				const std::string buffer = all.substr(0, line_start + column) + "\xEF\xBF\xBF" +
						all.substr(line_start + column);
				const Dictionary answer = language._lookup_code(
						String::utf8(buffer.data(), (int64_t)buffer.length()), symbol, p_path, nullptr);

				Dictionary row;
				row["line"] = line;
				row["symbol"] = symbol;
				// Which occurrence of the word this is, so a consumer can tell the columns of
				// one word from two words of the same name on one line -- `Hero.Start(Start...`
				// is a method and a local, and they are meant to answer differently.
				row["word"] = (int64_t)at;
				row["token"] = kind_at(column);
				row["result"] = answer.get("result", (int64_t)ERR_UNAVAILABLE);
				row["type"] = answer.get("type", (int64_t)-1);
				row["class_name"] = answer.get("class_name", String());
				row["class_member"] = answer.get("class_member", String());
				row["doc_type"] = answer.get("doc_type", String());
				row["description"] = answer.get("description", String());
				row["location"] = answer.get("location", (int64_t)-1);
				row["script_path"] = answer.get("script_path", String());
				// The other half of where a click lands, and the half this was blind to: 4.7
				// reads the script off the result as a Ref and 4.8 reads the path, so a row
				// carrying a path alone looked like a complete answer and was one in one
				// editor only. A bool rather than the object, because a row is JSON.
				row["has_script"] = answer.get("script", Variant()).booleanize();

				// What the host resolved, which is what says whether the label above is the right
				// one for it. The consumer cannot ask separately -- only this side knows which
				// position produced the row.
				if (host_can_answer) {
					const Dictionary found = runtime->lookup_symbol(globalized, (int32_t)line, (int32_t)column);
					row["host_kind"] = found.get("kind", (int64_t)-1);
					row["host_name"] = found.get("name", String());
					row["host_owner"] = found.get("owner", String());
					row["host_type"] = found.get("type", String());
					row["host_is_var"] = found.get("is_var", false);
					row["host_is_parameter"] = found.get("is_parameter", false);
					row["host_is_definition"] = found.get("is_definition", false);
				}

				// Dictionary equality is by reference, so the run is keyed on the text of the
				// answer instead.
				const String key = UtilityFunctions::var_to_str(row);
				if (run_start < 0) {
					previous = row;
					previous_key = key;
					run_start = (int64_t)column;
					continue;
				}
				if (key == previous_key) {
					continue;
				}
				previous["column"] = run_start;
				previous["column_end"] = (int64_t)column - 1;
				rows.push_back(previous);
				previous = row;
				previous_key = key;
				run_start = (int64_t)column;
			}
			if (run_start >= 0) {
				previous["column"] = run_start;
				previous["column_end"] = (int64_t)end;
				rows.push_back(previous);
			}

			at = end;
		}

		if (newline == std::string::npos) {
			break;
		}
		line_start = line_end + 1;
		line++;
	}

	return rows;
}

String VerseHover::publish_api_method(const String &p_receiver_type, const String &p_member,
		const String &p_function_type, const String &p_description) const {
	// The page is named after the receiver, without its type parameters: `event(t)` documents its
	// methods under `event`, the name a reader sees and the compiler prints. The page is built and
	// the class name returned whether or not an editor is present -- so the lookup result is the
	// same shape in a headless run, which is the only place this can be tested -- and the
	// registration below happens only when there is a script editor to register with.
	const int64_t paren = p_receiver_type.find("(");
	const String doc_class = paren < 0 ? p_receiver_type : p_receiver_type.substr(0, paren);
	if (doc_class.is_empty()) {
		return String();
	}

	// The declared type is `type{_(:receiver, :p1, ...)<effects>:result}`. Unwrap it to the plain
	// signature the parser reads, then drop the receiver, which is the extension method's first
	// parameter and not one the author writes at the call. The parameters carry no names here --
	// the declared type does not record them -- so the tooltip shows their types alone.
	String signature_text = p_function_type;
	if (signature_text.begins_with("type{_")) {
		signature_text = signature_text.substr(6, signature_text.length() - 6);
		const int64_t last_brace = signature_text.rfind("}");
		if (last_brace >= 0) {
			signature_text = signature_text.substr(0, last_brace);
		}
	}
	const VerseSignature signature = verse_parse_signature(signature_text.utf8().get_data());

	Array arguments;
	for (size_t i = 1; i < signature.params.size(); i++) {
		Dictionary argument;
		argument["name"] = String::utf8(signature.params[i].name.c_str());
		argument["type"] = String::utf8(signature.params[i].type.c_str());
		arguments.push_back(argument);
	}

	Dictionary method;
	method["name"] = p_member;
	method["arguments"] = arguments;
	method["return_type"] = String::utf8(signature.result_type.c_str());
	method["qualifiers"] = String::utf8(signature.specifiers.c_str());
	method["description"] = p_description;

	// One page per receiver, its methods replaced by name so re-hovering one does not double it.
	Dictionary page = api_doc_pages.has(doc_class) ? api_doc_pages[doc_class] : Dictionary();
	Array methods = page.has("methods") ? (Array)page["methods"] : Array();
	bool replaced = false;
	for (int64_t i = 0; i < methods.size(); i++) {
		const Dictionary existing = methods[i];
		if (String(existing["name"]) == p_member) {
			methods[i] = method;
			replaced = true;
			break;
		}
	}
	if (!replaced) {
		methods.push_back(method);
	}
	page["name"] = doc_class;
	page["inherits"] = String();
	page["is_script_doc"] = true;
	page["methods"] = methods;
	api_doc_pages[doc_class] = page;

#ifdef TOOLS_ENABLED
	EditorInterface *editor_interface = verse_editor_interface();
	ScriptEditor *script_editor = editor_interface != nullptr ? editor_interface->get_script_editor() : nullptr;
	if (script_editor != nullptr) {
		// The carrier is created once and kept out of live_scripts: it has no source class and must
		// not be walked by a build or an analysis. Its documentation is every page gathered so far,
		// which update_docs_from_script registers by class name -- the only door onto the doc store,
		// since EditorHelp is not bound (B40).
		if (api_doc_carrier.is_null()) {
			api_doc_carrier = Ref<VerseScript>(memnew(VerseScript));
			language.unregister_script(api_doc_carrier.ptr());
		}
		TypedArray<Dictionary> pages;
		for (const KeyValue<String, Dictionary> &entry : api_doc_pages) {
			pages.push_back(entry.value);
		}
		api_doc_carrier->set_injected_documentation(pages);

		const Ref<Script> ref = api_doc_carrier;
		script_editor->update_docs_from_script(ref);
	}
#endif
	return doc_class;
}
