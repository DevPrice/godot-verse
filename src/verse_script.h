#pragma once

// _get_language returns a ScriptLanguage *, and GDCLASS's register_virtuals needs the complete
// type to encode it; script_extension.hpp only forward-declares it.
#include <godot_cpp/classes/script_extension.hpp>

#include "verse_runtime.h"

#include <vector>
#include <godot_cpp/classes/script_language.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/typed_array.hpp>

struct vh_instance;

// A .verse file as a Godot Resource: one top-level Verse class named after the file, plus the
// source text the script editor edits.
//
// The host compiles from a path, never a buffer, so compile() works off get_path() rather than
// source_code — unsaved editor text reaches the compiler through
// VerseScriptLanguage::_validate instead.
class VerseScript : public godot::ScriptExtension {
	GDCLASS(VerseScript, godot::ScriptExtension)

protected:
	static void _bind_methods();

public:
	VerseScript();
	~VerseScript() override;

	bool _editor_can_reload_from_file() override;
	void _placeholder_erased(void *p_placeholder) override;
	bool _can_instantiate() const override;
	godot::Ref<godot::Script> _get_base_script() const override;
	godot::StringName _get_global_name() const override;
	bool _inherits_script(const godot::Ref<godot::Script> &p_script) const override;
	godot::StringName _get_instance_base_type() const override;
	void *_instance_create(godot::Object *p_for_object) const override;
	void *_placeholder_instance_create(godot::Object *p_for_object) const override;
	bool _instance_has(godot::Object *p_object) const override;
	bool _has_source_code() const override;
	godot::String _get_source_code() const override;
	void _set_source_code(const godot::String &p_code) override;
	godot::Error _reload(bool p_keep_state) override;
	godot::StringName _get_doc_class_name() const override;
	godot::TypedArray<godot::Dictionary> _get_documentation() const override;
	godot::String _get_class_icon_path() const override;
	bool _has_method(const godot::StringName &p_method) const override;
	bool _has_static_method(const godot::StringName &p_method) const override;
	godot::Variant _get_script_method_argument_count(const godot::StringName &p_method) const override;
	godot::Dictionary _get_method_info(const godot::StringName &p_method) const override;
	bool _is_tool() const override;
	bool _is_valid() const override;
	bool _is_abstract() const override;
	godot::ScriptLanguage *_get_language() const override;
	bool _has_script_signal(const godot::StringName &p_signal) const override;
	godot::TypedArray<godot::Dictionary> _get_script_signal_list() const override;
	bool _has_property_default_value(const godot::StringName &p_property) const override;
	godot::Variant _get_property_default_value(const godot::StringName &p_property) const override;
	void _update_exports() override;
	godot::TypedArray<godot::Dictionary> _get_script_method_list() const override;
	godot::TypedArray<godot::Dictionary> _get_script_property_list() const override;
	int32_t _get_member_line(const godot::StringName &p_member) const override;
	godot::Dictionary _get_constants() const override;
	godot::TypedArray<godot::StringName> _get_members() const override;
	bool _is_placeholder_fallback_enabled() const override;
	godot::Variant _get_rpc_config() const override;

	// Builds the project through the host and queues an analysis of this file's source. Safe to
	// call with no host loaded; leaves the script invalid rather than failing.
	//
	// Returns what the script is known to be *now*: the analysis it queues is not waited for, so
	// a save whose result has not landed yet answers from the previous one and settles a few
	// frames later through analysis_landed().
	godot::Error compile();

	// Re-derives validity and exports if the analysis just published is the one compile() asked
	// for, and reports whether it did. Called on every live script when a result lands, since one
	// analysis covers the project.
	bool analysis_landed();

	bool is_compiled() const;
	godot::String verse_class_name() const;

	vh_instance *make_instance(int64_t p_object_id) const;
	void free_instance(vh_instance *p_instance) const;
	bool instance_has_function(vh_instance *p_instance, const char *p_decorated_name) const;
	int32_t call_instance(vh_instance *p_instance,
			const char *p_decorated_name,
			const godot::Variant **p_args,
			int32_t p_arg_count,
			godot::Variant &r_result) const;

	// Every method this script's class declares, from the last analysis.
	const godot::Vector<VerseMethodInfo> &methods() const;

	// The method Godot would call p_name: a script method answers to its Verse name verbatim, and
	// one that overrides a Godot virtual answers to Godot's name for it as well. Null for a name
	// this class declares nothing under.
	const VerseMethodInfo *find_method(const godot::StringName &p_name) const;
	godot::Variant instance_field(vh_instance *p_instance, const godot::StringName &p_name) const;
	bool set_instance_field(vh_instance *p_instance, const godot::StringName &p_name, const godot::Variant &p_value) const;
	bool set_instance_field_instance(vh_instance *p_instance, const godot::StringName &p_name, vh_instance *p_value) const;

	// Pushes the export list and its default values into every placeholder instance this script
	// has out. A non-tool script gets placeholders rather than real instances in the editor, and
	// a placeholder shows nothing at all until it is told what to show.
	void update_placeholders();

private:
	// Rebuilds exports from the last analysis, or -- when that analysis cannot be believed --
	// leaves the previous list standing and turns placeholder fallback on. GDScript::_update_exports
	// is the same shape and for the same reason: see the note on placeholder_fallback_enabled.
	void refresh_exports() const;

	// The PROPERTY_USAGE_CATEGORY entry that heads the export list, naming the registered class
	// where there is one and the file otherwise.
	godot::Dictionary class_header() const;

	// Re-reads validity and the export list out of whatever analysis the host has published.
	void refresh_from_analysis();

	// The source compile() handed the language, while it is still waiting for the answer. Until
	// that lands the script keeps the validity and exports the last analysis gave it: deriving
	// them from the superseded one would flag a mistake the author has already undone.
	godot::String awaited_source;
	bool awaiting_analysis = false;

	// Borrowed: Godot owns each placeholder and tells us through _placeholder_erased when one
	// goes away.
	mutable std::vector<void *> placeholders;

	godot::String source_code;
	// The project built and this file contributed no errors to it. True for a library file, which
	// is valid Verse that simply has nothing to attach.
	bool valid = false;

	// The compiled project defines the class this file is named after, so the file can be attached
	// to a node. False for a library file -- a `.verse` of module-level functions, which is most of
	// what makes one flat scope livable while modules wait (R-LANG-6).
	bool has_own_class = false;

	// The property list the last believable analysis produced. Kept rather than recomputed on
	// demand so that a file which currently does not analyse still has an export list to show:
	// PlaceHolderScriptInstance::update erases every value whose name the new list omits, so
	// handing it an empty list over a typo would clear the inspector *and* drop the values out
	// of the scene on the next save.
	mutable godot::TypedArray<godot::Dictionary> exports_cache;

	// The method table from the same analysis as exports_cache, and refreshed with it. Cached
	// rather than re-asked because Godot calls _has_method on paths that run per frame, and each
	// ask walks the semantic program.
	mutable godot::Vector<VerseMethodInfo> methods_cache;

	// Set while exports_cache describes a program older than the file. Godot reads this through
	// _is_placeholder_fallback_enabled and switches every placeholder to serving its own stored
	// properties and values -- which is how a node keeps what the inspector last showed, and how
	// a scene loaded against a broken script hands its values back once the script builds again.
	mutable bool placeholder_fallback_enabled = false;
};
