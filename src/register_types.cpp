#include "register_types.h"

#include <gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/godot.hpp>

#include "verse_resource_format.h"
#include "verse_ref_table.h"
#include "verse_runtime.h"
#include "verse_script.h"
#include "verse_script_language.h"

#ifdef TOOLS_ENABLED
#include "verse_editor_plugin.h"
#include "verse_export_plugin.h"
#include "verse_syntax_highlighter.h"

#include <godot_cpp/classes/editor_plugin_registration.hpp>
#endif

using namespace godot;

static VerseRuntime *verse_runtime_singleton = nullptr;
static VerseScriptLanguage *verse_script_language = nullptr;
static Ref<VerseResourceFormatLoader> verse_loader;
static Ref<VerseResourceFormatSaver> verse_saver;

void initialize_gdextension_types(const ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(VerseRuntime);
		GDREGISTER_CLASS(VerseScriptLanguage);
		GDREGISTER_CLASS(VerseScript);
		GDREGISTER_CLASS(VerseResourceFormatLoader);
		GDREGISTER_CLASS(VerseResourceFormatSaver);

		verse_runtime_singleton = memnew(VerseRuntime);
		Engine::get_singleton()->register_singleton("VerseRuntime", verse_runtime_singleton);

		// The language before the loader: ResourceLoader hands a freshly loaded VerseScript to
		// VerseScript::compile(), which reports diagnostics through the language singleton.
		verse_script_language = memnew(VerseScriptLanguage);
		Engine::get_singleton()->register_script_language(verse_script_language);

		verse_loader.instantiate();
		ResourceLoader::get_singleton()->add_resource_format_loader(verse_loader);
		verse_saver.instantiate();
		ResourceSaver::get_singleton()->add_resource_format_saver(verse_saver);
	}

#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_CLASS(VerseSyntaxHighlighter);
		GDREGISTER_CLASS(VerseModuleMenu);
		GDREGISTER_CLASS(VerseExportPlugin);
		GDREGISTER_CLASS(VerseEditorPlugin);
		EditorPlugins::add_by_type<VerseEditorPlugin>();
	}
#endif
}

void uninitialize_gdextension_types(const ModuleInitializationLevel p_level) {
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorPlugins::remove_by_type<VerseEditorPlugin>();
	}
#endif

	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	// The reference table first, and here rather than in ~VerseRuntime, because of what is in it.
	// A Godot Callable can name a GDScript lambda; destroying one after its script has gone is a
	// crash at exit rather than a leak, and ~VerseRuntime runs late enough for that to happen.
	// ScriptLanguage::finish would be the natural place and Godot never calls it on an extension
	// language, so this is the earliest hook that actually runs.
	verse_ref_table().clear();

	if (verse_saver.is_valid()) {
		ResourceSaver::get_singleton()->remove_resource_format_saver(verse_saver);
		verse_saver.unref();
	}
	if (verse_loader.is_valid()) {
		ResourceLoader::get_singleton()->remove_resource_format_loader(verse_loader);
		verse_loader.unref();
	}

	if (verse_script_language != nullptr) {
		Engine::get_singleton()->unregister_script_language(verse_script_language);
		memdelete(verse_script_language);
		verse_script_language = nullptr;
	}

	if (verse_runtime_singleton != nullptr) {
		Engine::get_singleton()->unregister_singleton("VerseRuntime");
		memdelete(verse_runtime_singleton);
		verse_runtime_singleton = nullptr;
	}
}

extern "C" {
GDExtensionBool GDE_EXPORT godot_verse_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_gdextension_types);
	init_obj.register_terminator(uninitialize_gdextension_types);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
