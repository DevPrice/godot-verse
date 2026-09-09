#pragma once

// _get_language returns a ScriptLanguage *, and GDCLASS's register_virtuals needs the complete
// type to encode it; script_extension.hpp only forward-declares it.
#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/classes/script_language.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/typed_array.hpp>

struct vh_script;

// A .verse file as a Godot Resource. Owns one vh_script handle from the host plus the source
// text the script editor edits.
//
// The host compiles from a path, never a buffer, so compile() works off get_path() rather than
// source_code — unsaved editor text reaches the compiler through
// VerseScriptLanguage::_validate instead.
class VerseScript : public godot::ScriptExtension {
	GDCLASS(VerseScript, godot::ScriptExtension)

protected:
	static void _bind_methods();

public:
	VerseScript() = default;
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

	// Compiles get_path() through the host and caches which lifecycle functions the file
	// defines. Safe to call with no host loaded; leaves the script invalid rather than failing.
	godot::Error compile();

	bool is_compiled() const;
	bool verse_has_function(const char *p_decorated_name) const;
	godot::Error call_verse_void(const char *p_decorated_name);
	godot::Error call_verse_void_float(const char *p_decorated_name, double p_arg);

private:
	godot::String source_code;
	vh_script *handle = nullptr;
	bool valid = false;

	void release_handle();
};
