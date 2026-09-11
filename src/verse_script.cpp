#include "verse_script.h"

#include "verse_class_decl.h"
#include "verse_runtime.h"
#include "verse_script_instance.h"
#include "verse_script_language.h"

#include <gdextension_interface.h>

#include <godot_cpp/classes/engine.hpp>
#ifdef TOOLS_ENABLED
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#endif
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

using namespace godot;

namespace {

VerseRuntime *get_runtime() {
	return Object::cast_to<VerseRuntime>(Engine::get_singleton()->get_singleton("VerseRuntime"));
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
		return ERR_UNCONFIGURED;
	}

	// One file cannot be compiled on its own: the whole project is built together, and it is
	// built once. This script is only usable if that build succeeded outright — the linker
	// requires a complete program, so one bad file leaves nothing assembled.
	language->ensure_project_built();

	// Reached on save and on reload, where the answer has to be about the text being saved. When
	// the host is not already holding it, that costs a whole-project analysis -- ~100ms, which
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
	if (!awaiting_analysis || language == nullptr) {
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

void VerseScript::refresh_from_analysis() {
	VerseRuntime *runtime = get_runtime();
	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	if (runtime == nullptr || language == nullptr) {
		return;
	}

	// A .verse file is only a script if the compiled project defines the class it is named after:
	// there is no other shape a script can take, so a file without one cannot be attached.
	valid = language->ensure_project_built() == OK
			&& language->diagnostics_for(get_path()).is_empty()
			&& runtime->has_class(verse_class_name());

	// The export list only exists once the project has been analysed, and a placeholder created
	// before that got an empty one.
	update_placeholders();
}

String VerseScript::verse_class_name() const {
	return get_path().get_file().get_basename();
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

Error VerseScript::call_instance_void(vh_instance *p_instance, const char *p_decorated_name) const {
	VerseRuntime *runtime = get_runtime();
	return runtime != nullptr ? runtime->call_instance_void(p_instance, p_decorated_name) : ERR_UNAVAILABLE;
}

Error VerseScript::call_instance_void_float(vh_instance *p_instance, const char *p_decorated_name, double p_arg) const {
	VerseRuntime *runtime = get_runtime();
	return runtime != nullptr ? runtime->call_instance_void_float(p_instance, p_decorated_name, p_arg) : ERR_UNAVAILABLE;
}

bool VerseScript::is_compiled() const {
	return valid;
}

bool VerseScript::_editor_can_reload_from_file() {
	return true;
}

void VerseScript::_placeholder_erased(void *p_placeholder) {
	for (size_t i = 0; i < placeholders.size(); i++) {
		if (placeholders[i] == p_placeholder) {
			placeholders.erase(placeholders.begin() + i);
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
// Safe to answer from here even during the editor's first file scan: loading a .verse resource
// compiles the project on the way in, so by the time Godot asks there is an analysis to read.
// With no host there is simply nothing to say, and Godot re-asks when the script is saved.
TypedArray<Dictionary> VerseScript::_get_documentation() const {
	TypedArray<Dictionary> docs;

	VerseRuntime *runtime = get_runtime();
	const String class_name = verse_class_name();
	if (class_name.is_empty() || runtime == nullptr || !runtime->is_host_loaded()) {
		return docs;
	}

	const TypedArray<Dictionary> members = runtime->class_members(class_name);
	const String source = verse_newline_normalized(source_code);
	const VerseClassDecl decl = verse_scan_class_decl(source.utf8().get_data());

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

String VerseScript::_get_class_icon_path() const {
	return String();
}

Variant VerseScript::_get_script_method_argument_count(const StringName &p_method) const {
	return Variant();
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
	return false;
}

bool VerseScript::_is_abstract() const {
	return false;
}

StringName VerseScript::_get_instance_base_type() const {
	// From the declared Verse superclass, so a `class(node2d)` script attaches at Node2D rather
	// than at the Node floor. Text again, not the host: this is asked of scripts the editor has
	// merely scanned, well before anything is built.
	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	if (language == nullptr) {
		return StringName("Node");
	}
	const VerseClassDecl decl = verse_scan_class_decl(source_code.utf8().get_data());
	return language->base_types_for(decl).instance_base;
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
		placeholders.push_back(placeholder);
		const_cast<VerseScript *>(this)->update_placeholders();
	}
	return placeholder;
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

Error VerseScript::_reload(bool p_keep_state) {
	return compile();
}

bool VerseScript::_has_method(const StringName &p_method) const {
	// Every script class inherits Ready, Process and PhysicsProcess from `object`, so a valid
	// script has all three whether or not it overrides them. Whether an override exists is an
	// instance question, and VerseScriptInstance answers it — that is what decides whether Godot
	// puts the node in the per-frame process list.
	return is_compiled() && VerseScriptInstance::verse_name_for(p_method) != nullptr;
}

bool VerseScript::_has_static_method(const StringName &p_method) const {
	return false;
}

Dictionary VerseScript::_get_method_info(const StringName &p_method) const {
	Dictionary info;
	if (!_has_method(p_method)) {
		return info;
	}
	info["name"] = p_method;
	info["flags"] = (int64_t)METHOD_FLAG_NORMAL;
	return info;
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
	const VerseClassDecl decl = verse_scan_class_decl(source_code.utf8().get_data());
	return decl.is_global ? StringName(String(verse_pascal_case(decl.name).c_str())) : StringName();
}

bool VerseScript::_inherits_script(const Ref<Script> &p_script) const {
	return false;
}

bool VerseScript::_has_script_signal(const StringName &p_signal) const {
	return false;
}

TypedArray<Dictionary> VerseScript::_get_script_signal_list() const {
	return TypedArray<Dictionary>();
}

bool VerseScript::_has_property_default_value(const StringName &p_property) const {
	return _get_property_default_value(p_property).get_type() != Variant::NIL;
}

Variant VerseScript::_get_property_default_value(const StringName &p_property) const {
	VerseRuntime *runtime = get_runtime();
	if (!valid || runtime == nullptr) {
		return Variant();
	}
	return runtime->class_default_field(verse_class_name(), String(p_property));
}

void VerseScript::_update_exports() {
	update_placeholders();
}

void VerseScript::update_placeholders() {
	refresh_exports();

	GDExtensionInterfacePlaceholderScriptInstanceUpdate update = get_placeholder_instance_update_fn();
	if (update == nullptr || placeholders.empty()) {
		return;
	}

	// Nothing to hand over that would not be a downgrade. A placeholder keeps whatever it was
	// last given, and fallback is what makes Godot read the inspector out of that copy instead of
	// asking this script -- so the author sees the properties they had while they fix the file.
	if (placeholder_fallback_enabled) {
		return;
	}

	const TypedArray<Dictionary> properties = exports_cache;

	Dictionary values;
	for (int64_t i = 0; i < properties.size(); i++) {
		const Dictionary property = properties[i];
		// A group header is a layout marker, not a property, and has no value to report.
		if (((int64_t)property["usage"] & PROPERTY_USAGE_GROUP) != 0) {
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

	for (void *placeholder : placeholders) {
		update(placeholder, (GDExtensionConstTypePtr)&properties, (GDExtensionConstTypePtr)&values);
	}
}

TypedArray<Dictionary> VerseScript::_get_script_method_list() const {
	TypedArray<Dictionary> methods;
	static const char *const lifecycle_methods[] = { "_ready", "_process", "_physics_process" };
	for (const char *method_name : lifecycle_methods) {
		const StringName method(method_name);
		if (_has_method(method)) {
			methods.push_back(_get_method_info(method));
		}
	}
	return methods;
}

namespace {
// VariantTag is Godot's own Variant::Type numbering (see the vh_variant_tag comment in the ABI
// header), not another vh_type to translate -- so a non-zero tag is used as one directly and the
// vh_type switch below only covers the pre-v3 callers that still leave it at 0.
Variant::Type variant_type_for(int64_t p_vh_type, int64_t p_variant_tag) {
	if (p_variant_tag != 0) {
		return (Variant::Type)p_variant_tag;
	}
	switch ((vh_type)p_vh_type) {
		case VH_TYPE_LOGIC:
			return Variant::BOOL;
		case VH_TYPE_INT:
			return Variant::INT;
		case VH_TYPE_FLOAT:
			return Variant::FLOAT;
		case VH_TYPE_STRING:
			return Variant::STRING;
		default:
			return Variant::NIL;
	}
}
// The smallest change the inspector will make to a value of this type. Godot's own default for a
// float field, so a bound rounded to it lands where the spinbox was going to land anyway; an
// integer field steps by one.
static double inspector_step_for(Variant::Type p_type) {
	if (p_type == Variant::INT) {
		return 1.0;
	}
#ifdef TOOLS_ENABLED
	EditorInterface *editor = EditorInterface::get_singleton();
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

Dictionary property_for(const Dictionary &p_entry, Variant::Type p_type) {
	Dictionary property;
	property["name"] = p_entry["name"];
	property["type"] = (int64_t)p_type;

	switch ((int64_t)p_entry["hint"]) {
		case VH_EXPORT_HINT_RANGE:
			property["hint"] = (int64_t)PROPERTY_HINT_RANGE;
			property["hint_string"] = range_hint_for(p_entry, p_type);
			break;
		case VH_EXPORT_HINT_ENUM:
		case VH_EXPORT_HINT_CLASS:
			// The host rejects a member carrying either hint until its *value*, not just its
			// declared type, can cross the ABI -- an enum's ordinal, an object handle -- so no
			// property is built from one yet. CLASS additionally needs the Verse-to-Godot class
			// name mapping and the node-versus-resource split that belongs with that work.
		case VH_EXPORT_HINT_NONE:
		default:
			property["hint"] = (int64_t)PROPERTY_HINT_NONE;
			property["hint_string"] = String();
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

// Deliberately not gated on valid(). The export list is read out of the semantic program the last
// analysis left behind, and analysis re-runs on every edit, while code generation may only happen
// once per process -- so a project whose *first* build failed can still describe its classes once
// the author fixes them. Tying the inspector to the build instead would leave the properties gone
// for the rest of the session, with nothing the author could do about it but restart.
void VerseScript::refresh_exports() const {
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
		const Variant::Type type = variant_type_for(entry["type"], entry["variant_tag"]);

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

	exports_cache = properties;
	placeholder_fallback_enabled = false;
}

int32_t VerseScript::_get_member_line(const StringName &p_member) const {
	return -1;
}

Dictionary VerseScript::_get_constants() const {
	return Dictionary();
}

TypedArray<StringName> VerseScript::_get_members() const {
	return TypedArray<StringName>();
}

bool VerseScript::_is_placeholder_fallback_enabled() const {
	return placeholder_fallback_enabled;
}

Variant VerseScript::_get_rpc_config() const {
	return Variant();
}
