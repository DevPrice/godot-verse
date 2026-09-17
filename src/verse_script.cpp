#include "verse_script.h"

#include "verse_class_decl.h"
#include "verse_runtime.h"
#include "verse_script_instance.h"
#include "verse_script_language.h"
#include "verse_value.h"

#include <gdextension_interface.h>

#include <godot_cpp/classes/engine.hpp>
#ifdef TOOLS_ENABLED
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#endif
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

using namespace godot;

namespace {

VerseRuntime *get_runtime() {
	return Object::cast_to<VerseRuntime>(Engine::get_singleton()->get_singleton("VerseRuntime"));
}

// The two argument slots of the Dictionary shape ScriptExtension::_get_method_info wants for a
// MethodInfo. A declared type gives the typed one; PROPERTY_USAGE_NIL_IS_VARIANT on the untyped one
// is what tells Godot the absence is deliberate rather than a property nobody set a type on.
Dictionary typed_argument(const String &p_name, Variant::Type p_type) {
	Dictionary arg;
	arg["name"] = p_name;
	arg["type"] = (int64_t)p_type;
	arg["class_name"] = StringName();
	arg["hint"] = (int64_t)PROPERTY_HINT_NONE;
	arg["hint_string"] = String();
	// A NIL type here means "any", not "must be nil" -- which is what NIL_IS_VARIANT tells Godot,
	// and what a Verse array parameter needs, since it accepts any of Godot's packed arrays.
	arg["usage"] = (int64_t)(p_type == Variant::NIL
					? (PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT)
					: PROPERTY_USAGE_DEFAULT);
	return arg;
}

Dictionary untyped_argument(const String &p_name) {
	Dictionary arg;
	arg["name"] = p_name;
	arg["type"] = (int64_t)Variant::NIL;
	arg["class_name"] = StringName();
	arg["hint"] = (int64_t)PROPERTY_HINT_NONE;
	arg["hint_string"] = String();
	arg["usage"] = (int64_t)(PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT);
	return arg;
}

// One method as the MethodInfo Dictionary _get_method_info and _get_script_method_list share.
//
// The name crosses verbatim: a Verse `Fire` is `Fire` to GDScript, which is what a property
// already does (`mover.Greeting`) and is the C# convention in Godot rather than the GDScript one.
// A method that overrides a Godot virtual is listed under Godot's name for it instead, because
// that is the name the engine will call.
Dictionary method_info_dict(const VerseMethodInfo &p_method) {
	Dictionary info;
	info["name"] = p_method.godot_virtual == StringName() ? p_method.name : p_method.godot_virtual;
	info["flags"] = (int64_t)METHOD_FLAG_NORMAL;
	info["id"] = -1;
	info["default_args"] = Array();
	info["return"] = p_method.returns_value ? typed_argument(String(), p_method.return_type)
										   : untyped_argument(String());

	TypedArray<Dictionary> args;
	for (int64_t i = 0; i < p_method.params.size(); i++) {
		const VerseMethodInfo::Param &param = p_method.params[i];
		args.push_back(typed_argument(String(param.name), param.type));
	}
	info["args"] = args;
	return info;
}

GDExtensionInterfacePlaceholderScriptInstanceUpdate get_placeholder_instance_update_fn() {
	static GDExtensionInterfacePlaceholderScriptInstanceUpdate fn = (GDExtensionInterfacePlaceholderScriptInstanceUpdate)
			gdextension_interface::get_proc_address("placeholder_script_instance_update");
	return fn;
}

GDExtensionInterfacePlaceholderScriptInstanceCreate get_placeholder_instance_create_fn() {
	static GDExtensionInterfacePlaceholderScriptInstanceCreate fn = (GDExtensionInterfacePlaceholderScriptInstanceCreate)
			gdextension_interface::get_proc_address("placeholder_script_instance_create");
	return fn;
}

} // namespace

void VerseScript::_bind_methods() {
}

VerseScript::VerseScript() {
	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	if (language != nullptr) {
		language->register_script(this);
	}
}

VerseScript::~VerseScript() {
	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	if (language != nullptr) {
		language->unregister_script(this);
	}
}

Error VerseScript::compile() {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr) {
		// Loading many .verse resources with no runtime registered (e.g. during an editor
		// filesystem scan) would otherwise spam the log once per resource.
		static bool warned = false;
		if (!warned) {
			UtilityFunctions::push_warning("VerseScript: VerseRuntime singleton is not available");
			warned = true;
		}
		return ERR_UNAVAILABLE;
	}

	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	if (language == nullptr) {
		return ERR_UNAVAILABLE;
	}

	const String path = get_path();
	if (path.is_empty()) {
		valid = false;
		has_own_class = false;
		return ERR_UNCONFIGURED;
	}

	// One file cannot be compiled on its own: the whole project is built together. Ensure rather
	// than build, because this is reached on save and on reload, and neither publishes a
	// generation -- Play does, and the Build action does. The script is only usable if a build
	// succeeded outright: the linker requires a complete program, so one bad file leaves nothing
	// assembled.
	language->ensure_project_built();

	// Reached on save and on reload, where the answer has to be about the text being saved. When
	// the host is not already holding it, that costs a whole-project analysis -- ~750 ms, which
	// blocking here would spend with the editor frozen on every Ctrl+S. Queue it instead and keep
	// the previous answer until poll_check publishes this one.
	if (language->analysis_is_current(path, source_code)) {
		awaiting_analysis = false;
		refresh_from_analysis();
	} else {
		awaited_source = source_code;
		awaiting_analysis = true;
		language->queue_check(path, source_code);
	}

	return valid ? OK : ERR_COMPILATION_FAILED;
}

bool VerseScript::analysis_landed() {
	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	if (language == nullptr) {
		return false;
	}

	// Every landed analysis, not only the one this script asked for. refresh_exports believes the
	// project's diagnostics rather than this file's, so an edit to a file this script never heard
	// of can be the reason its list has to give way to the placeholder's. The rebuild itself waits
	// for the next ask, which is what stops the inspector paying for it on every redraw.
	exports_current = false;

	if (!awaiting_analysis) {
		return false;
	}

	// One analysis covers the project, but it is published against a single buffer: a result that
	// landed for another file's buffer says nothing about the text this script asked about.
	if (!language->analysis_is_current(get_path(), awaited_source)) {
		return false;
	}

	awaiting_analysis = false;
	awaited_source = String();
	refresh_from_analysis();
	return true;
}

void VerseScript::generation_published() {
	refresh_from_analysis();

	// The shape of the property list has come from analysis since Phase 1 and refreshes per
	// keystroke; what only a build can produce is a *value*, because a declared default is
	// evaluated by generated code. So this is where a changed default reaches the inspector, and
	// it is why a new member appears long before its default does.
	notify_property_list_changed();
}

void VerseScript::refresh_from_analysis() {
	VerseRuntime *runtime = get_runtime();
	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	if (runtime == nullptr || language == nullptr) {
		return;
	}

	// What lets update_placeholders below actually rebuild the list: a new generation changes the
	// declared defaults even when the analysis behind the shape has not moved.
	exports_current = false;

	// A file is valid when the project built and this file is not one of the reasons it might not
	// have. Attachability is the separate question below: a `.verse` holding only module-level
	// functions compiles, is usable from every other file in the one flat scope, and is not a
	// mistake -- so reporting it broken was wrong, and this is R-LANG-6's third clause.
	valid = language->ensure_project_built() == OK
			&& language->diagnostics_for(get_path()).is_empty();
	has_own_class = valid && runtime->has_class(verse_class_name());

	// The method table comes from the same analysis as the exports and is cached for the same
	// reason: Godot asks _has_method on per-frame paths, and every ask walks the semantic program.
	methods_cache = has_own_class ? runtime->class_methods(verse_class_name()) : Vector<VerseMethodInfo>();
	signals_cache = has_own_class ? runtime->class_signals(verse_class_name()) : Vector<VerseSignalInfo>();
	rpcs_cache = has_own_class ? runtime->class_rpcs(verse_class_name()) : Vector<VerseRpcInfo>();

	// The export list only exists once the project has been analysed, and a placeholder created
	// before that got an empty one.
	update_placeholders();
}

String VerseScript::verse_class_name() const {
	// The module prefix is what the host ABI wants: `player` at the project root,
	// `gameplay/player` for a file under a directory carrying a `.vmodule`. The *stem* is still
	// what names the class, and still the only class in the file that can go on a node -- Verse
	// stopped requiring that and the bridge did not.
	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	const String path = get_path();
	return language != nullptr ? language->qualified_class_name(path) : path.get_file().get_basename();
}

vh_instance *VerseScript::make_instance(int64_t p_object_id) const {
	VerseRuntime *runtime = get_runtime();
	return runtime != nullptr ? runtime->instantiate(verse_class_name(), p_object_id) : nullptr;
}

Variant VerseScript::instance_field(vh_instance *p_instance, const StringName &p_name) const {
	VerseRuntime *runtime = get_runtime();
	return runtime != nullptr ? runtime->instance_field(p_instance, String(p_name)) : Variant();
}

bool VerseScript::set_instance_field(vh_instance *p_instance, const StringName &p_name, const Variant &p_value) const {
	VerseRuntime *runtime = get_runtime();
	return runtime != nullptr && runtime->set_instance_field(p_instance, String(p_name), p_value);
}

bool VerseScript::set_instance_field_instance(vh_instance *p_instance, const StringName &p_name, vh_instance *p_value) const {
	VerseRuntime *runtime = get_runtime();
	return runtime != nullptr && runtime->set_instance_field_instance(p_instance, String(p_name), p_value);
}

bool VerseScript::instance_to_string(vh_instance *p_instance, String &r_text) const {
	VerseRuntime *runtime = get_runtime();
	return runtime != nullptr && runtime->instance_to_string(p_instance, r_text);
}

void VerseScript::free_instance(vh_instance *p_instance) const {
	VerseRuntime *runtime = get_runtime();
	if (runtime != nullptr) {
		runtime->release_instance(p_instance);
	}
}

bool VerseScript::instance_has_function(vh_instance *p_instance, const char *p_decorated_name) const {
	VerseRuntime *runtime = get_runtime();
	return runtime != nullptr && runtime->instance_has_function(p_instance, p_decorated_name);
}

int32_t VerseScript::call_instance(vh_instance *p_instance,
		const char *p_decorated_name,
		const Variant **p_args,
		int32_t p_arg_count,
		Variant &r_result) const {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr) {
		r_result = Variant();
		return VH_ERR_STATE;
	}
	return runtime->call_instance(p_instance, p_decorated_name, p_args, p_arg_count, r_result);
}

const Vector<VerseMethodInfo> &VerseScript::methods() const {
	return methods_cache;
}

const Vector<VerseSignalInfo> &VerseScript::signals() const {
	return signals_cache;
}

const VerseMethodInfo *VerseScript::find_method(const StringName &p_name) const {
	for (int64_t i = 0; i < methods_cache.size(); i++) {
		const VerseMethodInfo &method = methods_cache[i];
		if (method.name == p_name || (method.godot_virtual != StringName() && method.godot_virtual == p_name)) {
			return &method;
		}
	}
	return nullptr;
}

bool VerseScript::is_compiled() const {
	return has_own_class;
}

bool VerseScript::_editor_can_reload_from_file() {
	return true;
}

void VerseScript::_placeholder_erased(void *p_placeholder) {
	for (size_t i = 0; i < placeholders.size(); i++) {
		if (placeholders[i].placeholder == p_placeholder) {
			const int64_t owner_id = placeholders[i].owner_id;
			placeholders.erase(placeholders.begin() + i);
			forget_owner(owner_id);
			return;
		}
	}
}

StringName VerseScript::_get_doc_class_name() const {
	// The Verse name rather than the PascalCase one @global_class registers. This is the name the
	// lookup reports as a member's owner and the name the class is written under, and the two have
	// to be the same string for a hover on a member to find the documentation below.
	return StringName(verse_class_name());
}

// A script's own documentation, which is the only way the editor can describe a member of it.
//
// Registering it is what makes a hover on `Speed` say "Property" with the comment above it rather
// than "Local Variable": Godot's tooltip reads a description straight off the lookup result only
// for its two *local* results, and for a property or a method it goes to the documentation instead.
// `is_script_doc` is the other half -- it is what keeps ctrl+click jumping to the declaration
// rather than diverting into the help viewer the way a name from Godot's own API does.
//
// Godot builds a project's script documentation once per session and does it on a **loader thread**
// (EditorHelp::_regen_script_doc_thread), where every ABI entry point is refused: the host pins
// Verse to the game thread. So the ask that matters most arrives where it cannot be answered, and
// answering it with an empty member list registered a class whose every hover drew an empty box.
//
// Declining and asking to be asked again is the answer. republish_script_docs is what asks, on the
// editor's own thread. A `.verse` that declares no class of its own is the other way to have
// nothing to say and is never worth re-asking about, so it is separated out first (R-LANG-6).
TypedArray<Dictionary> VerseScript::_get_documentation() const {
	TypedArray<Dictionary> docs;

	const String source = verse_newline_normalized(source_code);
	const VerseClassDecl decl =
			verse_scan_class_decl(source.utf8().get_data(), get_path().get_file().get_basename().utf8().get_data());
	if (decl.name.empty()) {
		return docs;
	}

	VerseRuntime *runtime = get_runtime();
	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	OS *os = OS::get_singleton();
	const bool on_verse_thread = os != nullptr && os->get_thread_caller_id() == os->get_main_thread_id();
	const String class_name = verse_class_name();

	// has_class separates "not described yet" from "described, and has no members": both answer
	// an empty list, and only the first is worth asking again about.
	if (!on_verse_thread || class_name.is_empty() || runtime == nullptr || !runtime->is_host_loaded()
			|| !runtime->has_class(class_name)) {
		if (language != nullptr) {
			language->note_script_docs_deferred();
		}
		return docs;
	}

	const TypedArray<Dictionary> members = runtime->class_members(class_name);

	Array properties;
	Array methods;
	for (int64_t i = 0; i < members.size(); i++) {
		const Dictionary member = members[i];
		const String name = member["name"];
		const int64_t kind = member["kind"];
		const int64_t line = member["line"];

		// Every member of this class is declared in this file, so the comment is in the buffer
		// being edited rather than on disk -- which is what keeps documentation current with an
		// unsaved edit, the same way the analysis behind it is.
		const String description = verse_doc_comment_above(source, line);

		Dictionary entry;
		entry["name"] = name;
		entry["description"] = description;
		if (kind == VH_LOOKUP_FUNCTION) {
			// The whole signature as Verse spells it. Godot's own doc renders `return_type` beside
			// the name, and a Verse function type reads better there than a decomposition into
			// Godot's argument shape would -- the parameter names live in the argument hint.
			entry["return_type"] = member["type"];
			methods.push_back(entry);
		} else {
			entry["type"] = member["type"];
			properties.push_back(entry);
		}
	}

	Dictionary doc;
	doc["name"] = class_name;
	doc["inherits"] = String(decl.base.c_str());
	doc["brief_description"] = verse_doc_comment_above(source, decl.line);
	doc["description"] = verse_doc_comment_above(source, decl.line);
	doc["properties"] = properties;
	doc["methods"] = methods;
	doc["script_path"] = get_path();
	doc["is_script_doc"] = true;
	docs.push_back(doc);
	return docs;
}

// R-EXP-8's `@icon`, read out of the source text rather than from the host.
//
// Godot asks this of scripts it has only *scanned* -- the same filesystem-thread question
// `_get_global_class_name` answers, and before anything has been built -- so there is no analysis to
// ask and the host ABI could not answer it. verse_scan_class_decl reads both for that reason.
String VerseScript::_get_class_icon_path() const {
	return String::utf8(verse_scan_class_decl(source_code.utf8().get_data(),
			get_path().get_file().get_basename().utf8().get_data())
								.icon_path.c_str());
}

Variant VerseScript::_get_script_method_argument_count(const StringName &p_method) const {
	const VerseMethodInfo *method = is_compiled() ? find_method(p_method) : nullptr;
	// A nil Variant means "cannot say", which is not the same as zero arguments.
	return method != nullptr ? Variant((int64_t)method->params.size()) : Variant();
}

bool VerseScript::_can_instantiate() const {
	// Without the editor check a non-tool script gets a real instance in the editor and its
	// Ready() runs while the scene is merely open, which is what Script::can_instantiate guards
	// against for GDScript. Godot falls back to a placeholder instance when this is false.
	return is_compiled() && (_is_tool() || !Engine::get_singleton()->is_editor_hint());
}

bool VerseScript::_is_valid() const {
	return valid;
}

bool VerseScript::_is_tool() const {
	// From the text, not the host, and for the same reason _get_global_class_name is: Godot asks
	// this of scripts it has merely scanned, long before anything is built. Nothing else is
	// needed to make a tool script run -- _can_instantiate above already hands the editor a real
	// instance when this is true and a placeholder when it is not.
	return verse_scan_class_decl(source_code.utf8().get_data(),
			get_path().get_file().get_basename().utf8().get_data())
			.is_tool;
}

// R-NODE-5. Verse has `class<abstract>`, so this is the semantic program's answer rather than an
// unconditional no -- which is what had Godot offering to instantiate a base script that was never
// meant to be attached.
bool VerseScript::_is_abstract() const {
	VerseRuntime *runtime = get_runtime();
	return is_compiled() && runtime != nullptr && runtime->class_is_abstract(verse_class_name());
}

StringName VerseScript::_get_instance_base_type() const {
	// From the declared Verse superclass, so a `class(node2d)` script attaches at Node2D rather
	// than at the Node floor. Text again, not the host: this is asked of scripts the editor has
	// merely scanned, well before anything is built.
	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	if (language == nullptr) {
		return StringName("Node");
	}
	const VerseClassDecl decl =
			verse_scan_class_decl(source_code.utf8().get_data(), get_path().get_file().get_basename().utf8().get_data());

	if (!decl.name.empty()) {
		return language->base_types_for(decl).instance_base;
	}

	// No class in the text, which is two different things. In the editor it is a library file -- no
	// top-level class of its own, nothing to attach to -- and saying so is how Godot refuses and
	// explains: the attach dialog and the drag-a-script-onto-a-node path both test the base type
	// against the node's own class, and answering "Node" would let it be attached and then do
	// nothing at all.
	//
	// In an exported game it means the text is *gone*: every `.verse` ships as a one-byte stub, so
	// there has never been a superclass to read and this answered nothing for every script in the
	// game. Nothing in a running game had noticed, because the callers that matter there are narrow
	// -- a typed array of a script class in a serialised resource, and a custom ResourceFormatLoader
	// or Saver written in Verse -- but nothing had asked either. The host has the compiled class and
	// is the only side that does.
	//
	// Asked here and nowhere else, so the editor's per-keystroke path never reaches it: resolving a
	// class is a VM entry, and this is called from EditorFileSystem's scan thread.
	VerseRuntime *runtime = get_runtime();
	if (runtime != nullptr && !runtime->host_has_compiler()) {
		return StringName(runtime->class_base_type(verse_class_name()));
	}
	return StringName();
}

void *VerseScript::_instance_create(Object *p_for_object) const {
	return VerseScriptInstance::create(const_cast<VerseScript *>(this), p_for_object);
}

void *VerseScript::_placeholder_instance_create(Object *p_for_object) const {
	GDExtensionInterfacePlaceholderScriptInstanceCreate create_placeholder = get_placeholder_instance_create_fn();
	if (create_placeholder == nullptr || p_for_object == nullptr) {
		return nullptr;
	}

	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	if (language == nullptr) {
		return nullptr;
	}

	void *placeholder = create_placeholder(language->_owner, _owner, p_for_object->_owner);
	if (placeholder != nullptr) {
		const int64_t owner_id = (int64_t)p_for_object->get_instance_id();
		placeholders.push_back(PlaceholderRef{ placeholder, owner_id });
		note_owner(owner_id);
		const_cast<VerseScript *>(this)->update_placeholders();
	}
	return placeholder;
}

void VerseScript::note_owner(int64_t p_object_id) const {
	for (int64_t id : owner_ids) {
		if (id == p_object_id) {
			return;
		}
	}
	owner_ids.push_back(p_object_id);
}

void VerseScript::forget_owner(int64_t p_object_id) const {
	for (size_t i = 0; i < owner_ids.size(); i++) {
		if (owner_ids[i] == p_object_id) {
			owner_ids.erase(owner_ids.begin() + i);
			return;
		}
	}
}

void VerseScript::reload_instances() {
	// Snapshotted: set_script below destroys an instance and creates another, and both ends of
	// that run through note_owner/forget_owner and mutate the vector being walked.
	const std::vector<int64_t> ids = owner_ids;
	const Ref<Script> self(this);

	for (int64_t id : ids) {
		Object *owner = UtilityFunctions::instance_from_id(id);
		if (owner == nullptr || Object::cast_to<Script>(owner->get_script()) != this) {
			continue;
		}

		// The values live in the instance this is about to destroy, so they are read off the
		// object first and written back onto whatever kind of instance replaces it. Only the
		// script's own: a Node's `position` belongs to Godot and survives untouched.
		Dictionary saved;
		const TypedArray<Dictionary> properties = owner->get_property_list();
		for (int64_t i = 0; i < properties.size(); i++) {
			const Dictionary property = properties[i];
			if (((int64_t)property["usage"] & PROPERTY_USAGE_SCRIPT_VARIABLE) != 0) {
				const StringName name = property["name"];
				saved[name] = owner->get(name);
			}
		}

		// Object::set_script returns early when handed the script the object already has, so the
		// swap has to go through nothing and back. Nothing else destroys the old instance: it is
		// what holds the vh_instance made against the retiring generation.
		owner->set_script(Variant());
		owner->set_script(self);

		const Array names = saved.keys();
		for (int64_t i = 0; i < names.size(); i++) {
			owner->set(names[i], saved[names[i]]);
		}
	}
}

bool VerseScript::_instance_has(Object *p_object) const {
	return false;
}

bool VerseScript::_has_source_code() const {
	return !source_code.is_empty();
}

String VerseScript::_get_source_code() const {
	return source_code;
}

void VerseScript::_set_source_code(const String &p_code) {
	source_code = p_code;
}

// Called when the source changed and not when it was merely loaded -- the resource loader calls
// compile() directly, and this is the saver's path and Script.reload()'s.
//
// Re-attaching is the second half and is not optional. An instance holds a vh_instance made
// against one generation and adopts nothing, so without this a saved edit is compiled, analysed,
// reported on, and still not running (by-hand-findings.md B8).
//
// Only on a compile that succeeded. Re-attaching destroys every instance and every placeholder, and
// a placeholder's `values` map is the *only* copy of an exported value a non-tool script has in the
// editor -- so a swap that hands back an instance of the same retiring class is loss with no gain.
// A failed compile publishes no generation, which is exactly when there is nothing to adopt.
Error VerseScript::_reload(bool p_keep_state) {
	const Error status = compile();
	if (status == OK) {
		reload_instances();
	}
	return status;
}

bool VerseScript::_has_method(const StringName &p_method) const {
	return is_compiled() && find_method(p_method) != nullptr;
}

// R-NODE-4, the half Godot has to be told about: a Verse script's statics are ordinary members of
// an inline module -- `PlayerStatics.MaxSpeed` needs nothing from the bridge -- and what the
// `@statics("player")` attribute buys is the link, so these two questions have an answer.
bool VerseScript::_has_static_method(const StringName &p_method) const {
	if (!is_compiled()) {
		return false;
	}
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr) {
		return false;
	}
	const PackedStringArray names = runtime->class_static_methods(verse_class_name());
	for (int64_t i = 0; i < names.size(); i++) {
		if (StringName(names[i]) == p_method) {
			return true;
		}
	}
	return false;
}

Dictionary VerseScript::_get_method_info(const StringName &p_method) const {
	if (!is_compiled()) {
		return Dictionary();
	}
	const VerseMethodInfo *method = find_method(p_method);
	return method != nullptr ? method_info_dict(*method) : Dictionary();
}

ScriptLanguage *VerseScript::_get_language() const {
	return VerseScriptLanguage::singleton();
}

Ref<Script> VerseScript::_get_base_script() const {
	return Ref<Script>();
}

StringName VerseScript::_get_global_name() const {
	// Answered from the source text, like VerseScriptLanguage::_get_global_class_name and for the
	// same reasons -- and it has to agree with it, because Godot compares the two when it decides
	// whether the class cache is stale.
	const VerseClassDecl decl =
			verse_scan_class_decl(source_code.utf8().get_data(), get_path().get_file().get_basename().utf8().get_data());
	return decl.is_global ? StringName(String(verse_pascal_case(decl.name).c_str())) : StringName();
}

bool VerseScript::_inherits_script(const Ref<Script> &p_script) const {
	return false;
}

// R-SIG-1's Godot half, and what makes `connect` and `emit_signal` legal on a node carrying this
// script: Object::connect validates against has_script_signal, and emit_signalp refuses a name
// neither ClassDB nor the script knows.
bool VerseScript::_has_script_signal(const StringName &p_signal) const {
	for (int64_t i = 0; i < signals_cache.size(); i++) {
		if (signals_cache[i].name == p_signal && signals_cache[i].reject == VH_SIGNAL_OK) {
			return true;
		}
	}
	return false;
}

TypedArray<Dictionary> VerseScript::_get_script_signal_list() const {
	TypedArray<Dictionary> out;
	for (int64_t i = 0; i < signals_cache.size(); i++) {
		const VerseSignalInfo &signal = signals_cache[i];
		// A rejected signal is listed by the host and dropped here -- the same bargain the export
		// list makes a few hundred lines down, for the same reason: the editor needs the row so it
		// can say why, and Godot must not be told about a signal nothing can emit.
		if (signal.reject != VH_SIGNAL_OK) {
			continue;
		}
		Array args;
		for (int64_t j = 0; j < signal.args.size(); j++) {
			args.push_back(typed_argument(String(signal.args[j].name), signal.args[j].type));
		}
		Dictionary info;
		info["name"] = String(signal.name);
		info["args"] = args;
		info["flags"] = METHOD_FLAG_NORMAL;
		out.push_back(info);
	}
	return out;
}

// Whether this is one of the script's own exported members -- which is Godot's question, and not
// the one this used to answer.
//
// PlaceHolderScriptInstance::set refuses a value outright for a name this says no to
// (engine: core/object/script_language.cpp:597), and a placeholder's values are the only copy a
// non-tool script's exports have in the editor. Answering it with "is the default non-nil" tied two
// unrelated things to it: a nil default is what an object-, node- or resource-typed export always
// has, and what *every* export has while the last build failed, because _get_property_default_value
// below short-circuits on has_own_class. So a save during a failed compile -- or any save at all,
// for an object-typed export -- had reload_instances hand the values back to a placeholder that
// declined every one of them, and the node read null from then on.
//
// GDScript answers the same question the same way: its member_default_values_cache holds an entry
// for `@export var target: Node2D` whose value is null, so the name is known and the default is not.
bool VerseScript::_has_property_default_value(const StringName &p_property) const {
	refresh_exports();
	for (int64_t i = 0; i < exports_cache.size(); i++) {
		const Dictionary property = exports_cache[i];
		if (((int64_t)property["usage"] & PROPERTY_USAGE_SCRIPT_VARIABLE) != 0
				&& StringName(property["name"]) == p_property) {
			return true;
		}
	}
	return false;
}

Variant VerseScript::_get_property_default_value(const StringName &p_property) const {
	VerseRuntime *runtime = get_runtime();
	if (!has_own_class || runtime == nullptr) {
		return Variant();
	}
	return runtime->class_default_field(verse_class_name(), String(p_property));
}

void VerseScript::_update_exports() {
	exports_current = false;
	update_placeholders();
}

void VerseScript::update_placeholders() {
	refresh_exports();

	GDExtensionInterfacePlaceholderScriptInstanceUpdate update = get_placeholder_instance_update_fn();
	if (update == nullptr || placeholders.empty()) {
		return;
	}

	// Fallback means there is no *fresh* list to hand over, not that there is nothing to hand over.
	// A placeholder keeps whatever it was last given, which is what lets the author see the
	// properties they had while they fix the file -- but a placeholder created *during* the failure
	// was given nothing, and returning here left it with no properties at all: no inspector rows,
	// and nothing stored for the node the next time the scene was saved. reload_instances creates
	// exactly such a placeholder, which is how a save during a broken compile emptied a node.
	//
	// The last good list with no defaults is what covers both. PlaceHolderScriptInstance::update
	// erases only the values whose names are *absent* from the list it is given
	// (engine: core/object/script_language.cpp:723), and an empty values dictionary overwrites
	// none of them, so every placeholder that already holds this list is left exactly as it was.
	const bool have_defaults = !placeholder_fallback_enabled;
	const TypedArray<Dictionary> properties = exports_cache;
	if (!have_defaults && properties.is_empty()) {
		return;
	}

	Dictionary values;
	if (have_defaults) {
		for (int64_t i = 0; i < properties.size(); i++) {
			const Dictionary property = properties[i];
			// A section header is a layout marker, not a property, and has no value to report. The
			// three depths are three separate bits, so all three have to be tested: a category or a
			// subgroup tested against PROPERTY_USAGE_GROUP alone reads as a property.
			const int64_t header_usage = PROPERTY_USAGE_CATEGORY | PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP;
			if (((int64_t)property["usage"] & header_usage) != 0) {
				continue;
			}
			const StringName name = property["name"];
			// A default only exists once code generation has run, and the export list outlives that:
			// a project whose build failed can describe its members but cannot instantiate one to
			// read them off. Omitting the name leaves the placeholder's own value alone, where a nil
			// would overwrite it and then be written to the scene as the property's value.
			const Variant default_value = _get_property_default_value(name);
			if (default_value.get_type() != Variant::NIL) {
				values[name] = default_value;
			}
		}
	}

	for (const PlaceholderRef &entry : placeholders) {
		update(entry.placeholder, (GDExtensionConstTypePtr)&properties, (GDExtensionConstTypePtr)&values);
	}
}

TypedArray<Dictionary> VerseScript::_get_script_method_list() const {
	TypedArray<Dictionary> methods;
	if (!is_compiled()) {
		return methods;
	}
	for (int64_t i = 0; i < methods_cache.size(); i++) {
		methods.push_back(method_info_dict(methods_cache[i]));
	}
	return methods;
}

namespace {
// The smallest change the inspector will make to a value of this type. Godot's own default for a
// float field, so a bound rounded to it lands where the spinbox was going to land anyway; an
// integer field steps by one.
static double inspector_step_for(Variant::Type p_type) {
	if (p_type == Variant::INT) {
		return 1.0;
	}
#ifdef TOOLS_ENABLED
	// Reached at game runtime too -- a range hint is built wherever the property list is -- and there
	// is no editor to ask then. See verse_editor_interface.
	EditorInterface *editor = verse_editor_interface();
	Ref<EditorSettings> settings = editor != nullptr ? editor->get_editor_settings() : Ref<EditorSettings>();
	if (settings.is_valid()) {
		const Variant step = settings->get_setting("interface/inspector/default_float_step");
		if (step.get_type() == Variant::FLOAT && (double)step > 0.0) {
			return (double)step;
		}
	}
#endif
	return 0.001;
}

// A bound moved onto the inspector's step grid, inward, so that it is a value the inspector can
// actually produce.
//
// Every value a spinbox hands back is a multiple of its step, so a bound that falls between two of
// them is a bound the editor can never quite reach -- and rounding it the wrong way would make the
// field offer a value the type rejects. Inward is therefore the only safe direction, and it is
// also what makes a strict inequality work without being told about one: `_X < 500.0` reaches here
// as the double immediately below 500.0 and rounds to 499.999, while `_X <= 500.0` is already on
// the grid and does not move. A bound finer than one step -- `_X <= 0.0005` against a step of
// 0.001 -- rounds down to 0, which reads as harsh and is right: every other value the field could
// produce violates the constraint.
//
// The arithmetic is Godot's own Math::snapped, minus the half-step that rounds to nearest.
static double rounded_inward(double p_value, double p_step, bool p_is_min) {
	if (p_step <= 0.0) {
		return p_value;
	}
	const double steps = p_value / p_step;
	return (p_is_min ? Math::ceil(steps) : Math::floor(steps)) * p_step;
}

// Godot's range hint, which wants two numbers and has no spelling for a bound that is not there.
//
// A type constrained on one side only -- `type{_X:int where 0 <= _X}` -- is therefore spelled with
// the bound it does have at both ends, plus `or_greater`/`or_less` to say which way it runs on:
// Godot clamps at the end that is real and lets the value past the other, which is the constraint
// the compiler is enforcing. `hide_control` goes with that because a slider across a range of zero
// width says nothing. An older build spells that slice `hide_slider` and ignores this one, which
// costs a cosmetic slider and nothing else -- Range::get_as_ratio guards the division itself.
static String range_hint_for(const Dictionary &p_entry, Variant::Type p_type) {
	const bool has_min = p_entry["has_range_min"];
	const bool has_max = p_entry["has_range_max"];
	const double step = inspector_step_for(p_type);
	const double min = rounded_inward(p_entry["range_min"], step, true);
	const double max = rounded_inward(p_entry["range_max"], step, false);

	// An integer bound is spelled as one: Godot reads the hint with to_float() either way, but the
	// inspector shows the text, and "0,10" is what an author writing that type would have typed.
	auto spell = [p_type](double p_value) {
		return p_type == Variant::INT ? String::num_int64((int64_t)p_value) : String::num(p_value);
	};

	if (has_min && has_max) {
		return spell(min) + String(",") + spell(max);
	}
	if (has_min) {
		return spell(min) + String(",") + spell(min) + String(",or_greater,hide_control");
	}
	if (has_max) {
		return spell(max) + String(",") + spell(max) + String(",or_less,hide_control");
	}
	return String();
}

// Godot's three nesting depths, which are three usage flags rather than a depth number.
PropertyUsageFlags usage_for_group(int64_t p_kind) {
	switch (p_kind) {
		case VH_EXPORT_GROUP_CATEGORY:
			return PROPERTY_USAGE_CATEGORY;
		case VH_EXPORT_GROUP_SUBGROUP:
			return PROPERTY_USAGE_SUBGROUP;
		default:
			return PROPERTY_USAGE_GROUP;
	}
}

// The name an export's reference slot is filtered by: the Godot class for one of the mirrors, and the
// registered class name for one of the project's own.
//
// A mirrored name is resolved through the generated API's table, the only place the two spellings are
// written down together. A script's class has no entry there -- nothing generated it -- and the name
// Godot knows it by is the PascalCase form of the same name it registered with, derived here through
// verse_pascal_case rather than sent across the ABI: the host would have to reimplement the transform
// to send it, and two implementations of one naming rule are one too many.
//
// **The leaf of the qualified name, not the whole of it.** Everything the host is asked about is
// module-qualified -- `left/palette` for a class under a `.vmodule` -- and ClassDB is one flat
// namespace a module is deliberately not part of: `@global_class` registers the file stem
// PascalCased and nothing else. Passing the module through gave `Left/palette`, a name nothing had
// ever registered, and the inspector answered *"Cannot get class"* the moment a slot of that type
// was drawn. Found by hand; `by-hand-findings.md` B18.
String filter_class_from_hint(const Dictionary &p_entry) {
	const String verse_class = p_entry["hint_string"];
	if ((int64_t)p_entry["hint"] == VH_EXPORT_HINT_SCRIPT_CLASS) {
		const String leaf = verse_class.substr(verse_class.rfind("/") + 1);
		return String(verse_pascal_case(std::string(leaf.utf8().get_data())).c_str());
	}
	const char *godot_class = verse_godot_class_for(verse_class);
	return godot_class != nullptr ? String(godot_class) : String();
}

Dictionary property_for(const Dictionary &p_entry, Variant::Type p_type) {
	Dictionary property;
	property["name"] = p_entry["name"];
	property["type"] = (int64_t)p_type;

	switch ((int64_t)p_entry["hint"]) {
		case VH_EXPORT_HINT_RANGE:
			property["hint"] = (int64_t)PROPERTY_HINT_RANGE;
			property["hint_string"] = range_hint_for(p_entry, p_type);
			break;
		case VH_EXPORT_HINT_CLASS:
		case VH_EXPORT_HINT_SCRIPT_CLASS: {
			// Two different questions, and answering them with one name is the mistake worth avoiding.
			// What the slot is *filtered* by is the name Godot knows the class as, registered ones
			// included. Which *kind* of slot it is comes from ClassDB -- a node is picked out of the
			// scene and a resource off disk -- and ClassDB can only be asked about a class it has, so
			// it is asked about the mirrored class the host named instead. A script's registered name
			// is not one of those: asking whether `Mover` descends from Node gets "no", and an object
			// property with no hint is drawn as a resource picker.
			//
			// This is the same split GDScript makes, with `native_type` deciding the hint and
			// `_find_narrowest_native_or_global_class` the hint string
			// (engine: modules/gdscript/gdscript_parser.cpp:4796-4811).
			const char *native_class = verse_godot_class_for(p_entry["native_class"]);
			property["hint_string"] = filter_class_from_hint(p_entry);
			if (native_class == nullptr) {
				// `object` and nothing else: the only mirrored class the generated table leaves out,
				// and a reference to it says no more than "some Godot object".
				property["hint"] = (int64_t)PROPERTY_HINT_NONE;
			} else if (ClassDB::is_parent_class(native_class, "Node")) {
				property["hint"] = (int64_t)PROPERTY_HINT_NODE_TYPE;
			} else if (ClassDB::is_parent_class(native_class, "Resource")) {
				property["hint"] = (int64_t)PROPERTY_HINT_RESOURCE_TYPE;
			} else {
				// Neither, which is most of Godot's singletons -- an Engine or a MainLoop. There is
				// nothing to pick one of, so the slot is an object field with no picker behind it.
				property["hint"] = (int64_t)PROPERTY_HINT_NONE;
			}
			break;
		}
		// R-EXP-1's five, which say what a `string` or an `int` is *for*. Each maps to one of
		// Godot's own hints and each hint string is already in Godot's own spelling, so nothing is
		// translated here -- the flags names are comma separated because that is what
		// PROPERTY_HINT_FLAGS wants, and the file filter is `*.png` because that is what
		// PROPERTY_HINT_FILE wants.
		case VH_EXPORT_HINT_FILE:
			property["hint"] = (int64_t)PROPERTY_HINT_FILE;
			property["hint_string"] = p_entry["hint_string"];
			break;
		case VH_EXPORT_HINT_DIR:
			property["hint"] = (int64_t)PROPERTY_HINT_DIR;
			property["hint_string"] = String();
			break;
		case VH_EXPORT_HINT_MULTILINE:
			property["hint"] = (int64_t)PROPERTY_HINT_MULTILINE_TEXT;
			property["hint_string"] = String();
			break;
		case VH_EXPORT_HINT_FLAGS:
			property["hint"] = (int64_t)PROPERTY_HINT_FLAGS;
			property["hint_string"] = p_entry["hint_string"];
			break;
		case VH_EXPORT_HINT_NODE_PATH:
			property["hint"] = (int64_t)PROPERTY_HINT_NODE_PATH_VALID_TYPES;
			property["hint_string"] = p_entry["hint_string"];
			break;
		case VH_EXPORT_HINT_ENUM:
			// The enumerators, comma separated in declaration order, which is what Godot's enum hint
			// wants and what the stored ordinal indexes into. Spelled as the author wrote them: the
			// names are what the dropdown shows and nothing resolves them back.
			property["hint"] = (int64_t)PROPERTY_HINT_ENUM;
			property["hint_string"] = p_entry["hint_string"];
			break;
		case VH_EXPORT_HINT_NONE:
		default:
			// An untyped Array is the one property that needs a hint nothing asked for. Godot's
			// packed arrays say what they hold in their own type, but a plain Array does not, and an
			// editor that was not told offers the author a row of whatever they like -- which the
			// host then refuses the whole member for. The element type is spelled the way
			// Variant::get_type_name's callers spell it: the element's Variant type, then a colon.
			if ((int64_t)p_entry["element_variant_tag"] != VH_VARIANT_NIL) {
				property["hint"] = (int64_t)PROPERTY_HINT_TYPE_STRING;
				property["hint_string"] = String::num_int64((int64_t)p_entry["element_variant_tag"]) + String(":");
			} else {
				property["hint"] = (int64_t)PROPERTY_HINT_NONE;
				property["hint_string"] = String();
			}
			break;
	}
	// A non-var is editable here too, because the host applies a stored value while the instance
	// is still unsealed -- before any Verse code has run. That is initialization, not mutation, so
	// it keeps the author's `var`/non-var distinction rather than reaching around it. The
	// difference the inspector cannot show is that a non-var written *after* _ready is refused,
	// which only a remote inspector on a running game can reach.
	property["usage"] = (int64_t)(PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SCRIPT_VARIABLE);
	return property;
}

} // namespace

TypedArray<Dictionary> VerseScript::_get_script_property_list() const {
	refresh_exports();
	return exports_cache;
}

// The heading the inspector shows above this script's own properties, built the way
// Script::get_class_category() builds it: a nameless NIL entry whose hint_string is the script's
// path, which is where the inspector goes for the icon and the class documentation.
//
// The name is the one place this diverges from GDScript. get_class_category() reads
// Resource::get_name(), which no script loaded from a file ever has, so GDScript shows `player.gd`
// even for a `class_name Player`. A registered class is the name Godot knows the script by
// everywhere else -- the node creation dialog, a typed property's filter -- so it is the name shown
// here, and only a script without @global_class falls back to its file.
Dictionary VerseScript::class_header() const {
	const StringName global_name = _get_global_name();

	Dictionary header;
	header["name"] = global_name == StringName() ? get_path().get_file() : String(global_name);
	header["type"] = (int64_t)Variant::NIL;
	header["hint"] = (int64_t)PROPERTY_HINT_NONE;
	header["hint_string"] = get_path();
	header["usage"] = (int64_t)PROPERTY_USAGE_CATEGORY;
	return header;
}

// Deliberately not gated on valid(). The export list is read out of the semantic program the last
// analysis left behind, and analysis re-runs on every edit, while code generation waits for a
// build -- so a file the author is halfway through editing still describes its classes, and a
// project whose last build failed still shows the shape of what it will be. Tying the inspector to
// the build instead would empty the properties for as long as the file is broken, which is exactly
// when the author wants to see them.
void VerseScript::refresh_exports() const {
	if (exports_current) {
		return;
	}
	exports_current = true;

	VerseRuntime *runtime = get_runtime();
	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	if (runtime == nullptr || language == nullptr) {
		placeholder_fallback_enabled = true;
		return;
	}

	// uLang recovers from an error and carries on, so a class that failed to analyse is still in
	// the program -- with however many of its members the recovery managed to reach. Believing
	// that list would drop the members it lost, and dropping a member is what erases its value.
	if (!language->diagnostics_for(get_path()).is_empty()) {
		placeholder_fallback_enabled = true;
		return;
	}

	bool found = false;
	const TypedArray<Dictionary> exports = runtime->class_exports(verse_class_name(), &found);
	if (!found) {
		placeholder_fallback_enabled = true;
		return;
	}

	TypedArray<Dictionary> properties;

	// Godot has no per-property section field: a PROPERTY_USAGE_CATEGORY, _GROUP or _SUBGROUP
	// entry claims every property that follows it, up to the next entry of that kind (or the end
	// of the list) -- the same positional rule `@export_group` follows in GDScript and
	// [ExportGroup] in C#. So the export list is walked once, in the declaration order
	// vh_class_export_list already returns, and a header is inserted only where a member opens a
	// section; that keeps the inspector, and the scene file Godot rewrites from it, in the order
	// the author wrote the class.
	int64_t group_kind = VH_EXPORT_GROUP_NONE;
	String group = String();
	for (int64_t i = 0; i < exports.size(); i++) {
		const Dictionary entry = exports[i];
		// A rejected member is still listed -- deliberately, so a later commit can report *why*,
		// at the line the host saw it declared on -- it is just not built into a property here.
		// Skipping it before the group comparison below means it can't leave a group header
		// stranded above nothing.
		if ((int64_t)entry["reject"] != VH_EXPORT_OK) {
			continue;
		}
		const Variant::Type type = variant_type_for((int32_t)(int64_t)entry["type"], (int32_t)(int64_t)entry["variant_tag"]);

		const int64_t entry_kind = entry["group_kind"];
		const String entry_group = entry["group_name"];
		if (entry_kind != VH_EXPORT_GROUP_NONE && (entry_kind != group_kind || entry_group != group)) {
			Dictionary group_entry;
			group_entry["name"] = entry_group;
			group_entry["type"] = (int64_t)Variant::NIL;
			group_entry["hint"] = (int64_t)PROPERTY_HINT_NONE;
			group_entry["hint_string"] = String();
			group_entry["usage"] = (int64_t)usage_for_group(entry_kind);
			properties.push_back(group_entry);
			group_kind = entry_kind;
			group = entry_group;
		}

		properties.push_back(property_for(entry, type));
	}

	if (!properties.is_empty()) {
		properties.insert(0, class_header());
	}

	exports_cache = properties;
	placeholder_fallback_enabled = false;
}

// ScriptEditor::script_goto_method calls this by name for the Connections dock's "Go to method"
// and for an animation method track; without it those jumps did nothing. class_members is the
// same list _validate walks for the outline, converted to Godot's 1-based line the same way.
// Not restricted to VH_LOOKUP_FUNCTION: the contract here is "a member", and a property is as
// legitimate a jump target as a method.
int32_t VerseScript::_get_member_line(const StringName &p_member) const {
	VerseRuntime *runtime = get_runtime();
	if (!is_compiled() || runtime == nullptr) {
		return -1;
	}

	const String name = p_member;
	const TypedArray<Dictionary> members = runtime->class_members(verse_class_name());
	for (int64_t i = 0; i < members.size(); i++) {
		const Dictionary member = members[i];
		if (String(member["name"]) != name) {
			continue;
		}
		const int64_t line = member["line"];
		return line >= 0 ? (int32_t)(line + 1) : -1;
	}
	return -1;
}

Dictionary VerseScript::_get_constants() const {
	VerseRuntime *runtime = get_runtime();
	return is_compiled() && runtime != nullptr
			? runtime->class_static_constants(verse_class_name())
			: Dictionary();
}

TypedArray<StringName> VerseScript::_get_members() const {
	return TypedArray<StringName>();
}

bool VerseScript::_is_placeholder_fallback_enabled() const {
	return placeholder_fallback_enabled;
}

// R-EXP-9's receiving half: what Godot's multiplayer asks a script for.
//
// The shape is Godot's own and is not documented anywhere but in the code that reads it --
// SceneRPCInterface::_parse_rpc_config walks the *keys* as method names and each value as a
// Dictionary of "rpc_mode", "call_local", "transfer_mode" and "channel". All four are written,
// not just the ones the author named: the host has already applied Godot's defaults, and writing
// them out is what keeps the two sides from disagreeing about what `@rpc("any_peer")` alone means.
//
// A rejected config is dropped rather than registered with whatever survived parsing. An `@rpc`
// with a misspelled word is a method the author believes is remote-callable, and half-applying it
// would make that belief nearly true, which is worse than not at all -- `_validate` says why at
// the method's line.
Variant VerseScript::_get_rpc_config() const {
	Dictionary config;
	for (int64_t i = 0; i < rpcs_cache.size(); i++) {
		const VerseRpcInfo &rpc = rpcs_cache[i];
		if (rpc.reject != VH_RPC_OK) {
			continue;
		}
		Dictionary entry;
		entry["rpc_mode"] = (int64_t)rpc.rpc_mode;
		entry["call_local"] = rpc.call_local;
		entry["transfer_mode"] = (int64_t)rpc.transfer_mode;
		entry["channel"] = (int64_t)rpc.channel;
		config[rpc.name] = entry;
	}
	return config;
}
