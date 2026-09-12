#pragma once

#include "verse_host.h"
#include "verse_host_abi.h"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/typed_array.hpp>

// One method a Verse script declares, in Godot's vocabulary.
//
// A copy rather than a view: vh_class_method_list's descriptors live until the next call to it,
// and a script's method table outlives many of those.
struct VerseMethodInfo {
	// The Verse name, which is the name Godot calls it by -- a script method is not transformed on
	// its way out.
	godot::StringName name;
	// What call_instance takes. A CharString because the ABI wants utf8 bytes and a StringName
	// would have to be converted at every call.
	godot::CharString decorated;
	// Godot's own name for the virtual this overrides -- `_ready` -- or empty for a plain method.
	// A virtual answers to this name as well as to its Verse one, which is how Godot's own calls
	// reach it.
	godot::StringName godot_virtual;

	struct Param {
		godot::StringName name;
		godot::Variant::Type type = godot::Variant::NIL;
	};
	godot::Vector<Param> params;
	int32_t required_params = 0;

	godot::Variant::Type return_type = godot::Variant::NIL;
	bool returns_value = false;

	bool can_fail = false;
	bool suspends = false;
};

// The "VerseRuntime" engine singleton. Owns the verse_host.dll loader and the vh_init_desc handed
// to it. Every method degrades to ERR_UNAVAILABLE plus a warning when no host is loaded; nothing
// here may crash for that reason.
class VerseRuntime : public godot::Object {
	GDCLASS(VerseRuntime, godot::Object)

protected:
	static void _bind_methods();

public:
	VerseRuntime() = default;
	~VerseRuntime() override;

	// Reads verse/host/dll_path, verse/host/engine_dir and verse/host/enable_debugger from
	// ProjectSettings (creating them with defaults if absent) and loads the host from there.
	godot::Error load_host();
	godot::Error load_host(const godot::String &p_dll_path);
	void unload_host();
	bool is_host_loaded() const;

	void tick(double p_budget_seconds);

	// Verse's compilation unit is the package, not the file, so every .verse file in the project
	// is built together -- a build is always of the whole project. Each successful call publishes
	// a new generation; instances made against an earlier one keep running against it.
	//
	// p_module_paths runs parallel to p_globalized_paths and says which module each file's
	// definitions go into, "" being the project's root module. Godot answers that question because
	// it is a question about res://, which the host knows nothing about.
	//
	// While r_diagnostics_by_path is non-null every diagnostic the host reports is filed under
	// its own source path as { line, column, message, path } instead of reaching the output log.
	godot::Error compile_project(const godot::PackedStringArray &p_globalized_paths, const godot::PackedStringArray &p_module_paths, godot::Dictionary *r_diagnostics_by_path);

	// Which generation the last successful compile_project published, counting from 1; 0 before
	// the first. A failed build does not advance it.
	int32_t script_generation() const { return generation; }
	// Re-runs semantic analysis with one file's text replaced, filing diagnostics the same way
	// compile_project does. Generates nothing, so it is safe to call as often as the editor asks.
	godot::Error check_project(const godot::String &p_globalized_path, const godot::String &p_source, godot::Dictionary *r_diagnostics_by_path);

	// The same analysis on the host's own thread, so the editor keeps drawing. ERR_BUSY when one
	// is already running -- only one at a time.
	godot::Error begin_check_project(const godot::String &p_globalized_path, const godot::String &p_source);

	// Reaps a begin_check_project. True only on the call that reaps one, which is when
	// r_diagnostics_by_path has been filled in. Cheap enough to call every frame.
	bool poll_check_project(godot::Dictionary *r_diagnostics_by_path);

	bool is_check_project_busy() const;
	// A script is a top-level class named after its own file, driven through one instance of
	// that class per node. False when the compiled project defines no such class, which is what
	// makes the .verse file unusable as a script.
	bool has_class(const godot::String &p_class_name) const;
	// The `@editable` data members of a top-level class, as { name, type, is_var } entries
	// where type is a vh_type. Read out of the last analysis pass rather than the running
	// program, so it follows the editor's buffer and refreshes without restarting -- unlike
	// anything routed through the compiled bytecode, which may only be generated once.
	//
	// r_found separates a class that exports nothing from one the analysis never saw; both come
	// back as an empty array, and only the second means the answer is not to be believed.
	godot::TypedArray<godot::Dictionary> class_exports(const godot::String &p_class_name, bool *r_found = nullptr) const;

	// The definition the identifier at p_line/p_column resolves to, as
	// { name, path, line, column, type, kind, is_var }, or an empty dictionary when nothing
	// there resolves. Positions in and out are the compiler's: zero-based rows, and columns
	// that are byte offsets into the line rather than character counts.
	//
	// Answered from the last analysis, which cannot tell that the buffer has moved since --
	// so the caller must have established that the text at p_globalized_path is the text that
	// analysis saw, or every locus below an edit is off by the rows it added.
	godot::Dictionary lookup_symbol(const godot::String &p_globalized_path, int32_t p_line, int32_t p_column) const;

	// What could be written at p_line/p_column of p_source, as an array of
	// { name, type, owner, kind, is_var } -- the members of the expression there when
	// p_mode is VH_COMPLETE_MEMBERS, everything the scope admits when it is VH_COMPLETE_SCOPE,
	// and the attributes among those when it is VH_COMPLETE_ATTRIBUTES. Positions are the
	// compiler's, as above, and for members they are the *receiver's* last byte rather than the
	// cursor.
	//
	// Takes the buffer because it analyses it: unlike lookup_symbol there is never an existing
	// analysis of half-typed text to answer from. That analysis costs about as much as a
	// validate and blocks for it, and it spends whatever analysis lookup_symbol was relying on
	// -- see note_host_program_spent.
	godot::TypedArray<godot::Dictionary> complete_symbol(const godot::String &p_globalized_path, const godot::String &p_source, int32_t p_line, int32_t p_column, int32_t p_mode) const;

	// Every member p_class_name declares itself, as { name, type, owner, path, line, kind,
	// is_var } -- broader than class_exports, which answers only for the inspector. Read off the
	// last analysis, so it follows the editor's buffer.
	godot::TypedArray<godot::Dictionary> class_members(const godot::String &p_class_name) const;

	// The function called at p_line/p_column of p_source, as { name, result, params }. The
	// position names the callee's last byte rather than the cursor, for the same reason
	// complete_symbol's does. Analyses the buffer, and spends the analysis the same way.
	godot::Dictionary signature_at(const godot::String &p_globalized_path, const godot::String &p_source, int32_t p_line, int32_t p_column) const;

	vh_instance *instantiate(const godot::String &p_class_name, int64_t p_object_id);
	void release_instance(vh_instance *p_instance);
	bool instance_has_function(vh_instance *p_instance, const char *p_decorated_name) const;

	// Calls any method the script declares. Answers a vh_status rather than a godot::Error because
	// the caller has to tell the four outcomes apart: VH_ERR_NOT_FOUND is INVALID_METHOD to Godot,
	// VH_ERR_ARGUMENT is an argument-count or type error, VH_ERR_FAILED is a <decides> method that
	// declined -- which is a nil return, not an error -- and VH_ERR_RUNTIME has already been
	// reported with its stack.
	int32_t call_instance(vh_instance *p_instance,
			const char *p_decorated_name,
			const godot::Variant **p_args,
			int32_t p_arg_count,
			godot::Variant &r_result);

	// Every method a script's class declares, copied out of the host's storage -- which the ABI
	// only promises until the next call, so nothing here may hold a pointer into it.
	godot::Vector<VerseMethodInfo> class_methods(const godot::String &p_class_name) const;

	// One data member read off a live instance, and off the class default object respectively.
	// Unlike class_exports these go through the VM, because a value exists nowhere else. A nil
	// Variant means the field is absent or holds a Verse type with no Variant counterpart.
	godot::Variant instance_field(vh_instance *p_instance, const godot::String &p_name) const;
	godot::Variant class_default_field(const godot::String &p_class_name, const godot::String &p_name) const;
	// Writes one data member on a live instance. Covers bool, int, float, String and an Object --
	// the same types the reader covers -- and refuses anything else rather than truncating it.
	// An Object crosses as its instance id, and a null one clears the member.
	bool set_instance_field(vh_instance *p_instance, const godot::String &p_name, const godot::Variant &p_value);
	// Writes a reference member with another script's instance rather than with a handle, which is
	// what a member typed as one of the project's own classes holds. A null p_value clears it.
	bool set_instance_field_instance(vh_instance *p_instance, const godot::String &p_name, vh_instance *p_value);

private:
	VerseHostLibrary host;
	vh_init_desc init_desc = {};
	vh_godot_api godot_api = {};
	godot::Dictionary *diagnostic_sink = nullptr;
	int32_t generation = 0;

	godot::Error load_host_internal(const godot::String &p_dll_path, const godot::String &p_engine_dir, bool p_enable_debugger);

	static void api_print(void *p_ctx, const char *p_utf8, int32_t p_len);
	static vh_bool api_is_valid(void *p_ctx, vh_handle p_handle);
	static int32_t api_get_property(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, vh_arena *p_arena, vh_value *r_value);
	static int32_t api_set_property(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_value);
	static int32_t api_call_method(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_args, int32_t p_arg_count, vh_arena *p_arena, vh_value *r_value);
	static vh_handle api_get_singleton(void *p_ctx, const char *p_name_utf8, int32_t p_name_len);

	// The reference table (R-TYPE-1). See src/verse_ref_table.h for what is in it and why, and the
	// ABI header's "reference values" for the ownership rule these implement.
	static void api_release_ref(void *p_ctx, int64_t p_ref);
	static int64_t api_retain_ref(void *p_ctx, int64_t p_ref);
	static int64_t api_new_ref(void *p_ctx, int32_t p_variant_tag);
	static int32_t api_ref_get(void *p_ctx, int64_t p_ref, const vh_value *p_key, vh_arena *p_arena, vh_value *r_value);
	static int32_t api_ref_set(void *p_ctx, int64_t p_ref, const vh_value *p_key, const vh_value *p_value);
	static int32_t api_ref_size(void *p_ctx, int64_t p_ref, int64_t *r_size);
	static int32_t api_ref_contents(void *p_ctx, int64_t p_ref, vh_arena *p_arena, vh_value *r_value);
	static int32_t api_invoke_callable(void *p_ctx, int64_t p_ref, const vh_value *p_args, int32_t p_arg_count, vh_arena *p_arena, vh_value *r_value);

	// R-DIAG-2: a Verse runtime error, with the file, line and Verse call stack it was raised at.
	// Separate from on_diagnostic because the two go to different places -- a compile error
	// annotates the script editor's gutter, this goes to the output and errors panel.
	static void on_runtime_error(void *p_ctx, const vh_runtime_error *p_error);

	static void on_diagnostic(void *p_ctx, const vh_diagnostic *p_diagnostic);
};
