#pragma once

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

class VerseScriptLanguage;
class VerseScript;

// Ctrl+click, the ctrl-hover underline and the documentation tooltip: `_lookup_code`'s whole body,
// and the probe seam over it (`probe_hover`).
//
// Owned by VerseScriptLanguage as a mutable member, the way VerseProjectState, VerseDebugger,
// VerseProfiler and VerseCompletion are. `_lookup_code` stays declared on VerseScriptLanguage and
// delegates here for the reason theirs do; `probe_hover` stays a real VerseScriptLanguage member
// too, because ClassDB binds it by pointer-to-member for the GDScript-visible seam
// tools/probe_hover.py uses, and its body is the one-line delegation.
class VerseHover {
public:
	explicit VerseHover(VerseScriptLanguage &p_language) :
			language(p_language) {}

	godot::Dictionary lookup_code(const godot::String &p_code, const godot::String &p_symbol, const godot::String &p_path, godot::Object *p_owner) const;

	// Every hover the editor could produce over one file. Whole body of
	// VerseScriptLanguage::probe_hover; tools/probe_hover.py consumes it.
	godot::TypedArray<godot::Dictionary> probe(const godot::String &p_path);

private:
	VerseScriptLanguage &language;

	// Registers a documentation page for a Godot-package function that no Godot class documents --
	// an extension method on a Verse type like `event(t)`, or a free function of GodotApi -- so a
	// hover draws a method tooltip rather than a constant whose type is the whole function type. No
	// Godot page exists for one, and `EditorHelp` is not exposed to a GDExtension, so the page is
	// carried by `api_doc_carrier`: a script with no file whose only job is to feed
	// `ScriptEditor::update_docs_from_script`, the one door onto the doc store. Returns the class
	// name to put in the lookup result, or empty when there is no script editor to register with
	// (a headless run), so the caller falls back to the local result (B40).
	godot::String publish_api_method(const godot::String &p_receiver_type, const godot::String &p_member,
			const godot::String &p_function_type, const godot::String &p_description) const;

	// The doc carrier and the pages it holds, for publish_api_method. The carrier is a VerseScript
	// with no file, kept out of live_scripts so the build and analysis walks never reach it; its
	// documentation is whatever api_doc_pages currently holds, one ClassDoc per Godot-package
	// receiver a hover has asked about. Mutable because a hover is const and is where they fill.
	mutable godot::Ref<VerseScript> api_doc_carrier;
	mutable godot::HashMap<godot::String, godot::Dictionary> api_doc_pages;
};
