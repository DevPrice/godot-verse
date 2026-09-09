#include "verse_ticker.h"

#include "verse_runtime.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {

VerseRuntime *get_runtime() {
	return Object::cast_to<VerseRuntime>(Engine::get_singleton()->get_singleton("VerseRuntime"));
}

} // namespace

void VerseTicker::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_verse_script", "path"), &VerseTicker::set_verse_script);
	ClassDB::bind_method(D_METHOD("get_verse_script"), &VerseTicker::get_verse_script);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "verse_script", PROPERTY_HINT_FILE, "*.verse"), "set_verse_script", "get_verse_script");

	ClassDB::bind_method(D_METHOD("set_budget_ms", "budget_ms"), &VerseTicker::set_budget_ms);
	ClassDB::bind_method(D_METHOD("get_budget_ms"), &VerseTicker::get_budget_ms);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "budget_ms"), "set_budget_ms", "get_budget_ms");
}

void VerseTicker::set_verse_script(const String &p_path) {
	verse_script = p_path;
}

String VerseTicker::get_verse_script() const {
	return verse_script;
}

void VerseTicker::set_budget_ms(double p_budget_ms) {
	budget_ms = p_budget_ms;
}

double VerseTicker::get_budget_ms() const {
	return budget_ms;
}

void VerseTicker::_ready() {
	script_loaded = false;
	has_ready_fn = false;
	has_update_fn = false;

	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}

	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr) {
		UtilityFunctions::push_error("VerseTicker: VerseRuntime singleton is not available");
		return;
	}

	if (runtime->load_host() != OK) {
		return;
	}

	const String globalized = ProjectSettings::get_singleton()->globalize_path(verse_script);
	if (runtime->compile_file(globalized) != OK) {
		return;
	}
	script_loaded = true;

	has_ready_fn = runtime->script_has_function("Ready");
	has_update_fn = runtime->script_has_function("Update(:float)");

	if (has_ready_fn) {
		runtime->call_void("Ready");
	}
}

void VerseTicker::_process(double p_delta) {
	if (!script_loaded) {
		return;
	}

	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr) {
		return;
	}

	runtime->tick(budget_ms / 1000.0);

	if (has_update_fn) {
		runtime->call_void_float("Update(:float)", p_delta);
	}
}

void VerseTicker::_exit_tree() {
	VerseRuntime *runtime = get_runtime();
	if (runtime != nullptr) {
		runtime->release_script();
	}
	script_loaded = false;
	has_ready_fn = false;
	has_update_fn = false;
}
