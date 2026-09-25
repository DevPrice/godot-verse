#pragma once

#include <godot_cpp/classes/editor_export_plugin.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

// What an export does with a Verse project (R-DIST-9 … R-DIST-11).
//
// Three things, none of which Godot would do on its own:
//
//   - runs `verse_cook.exe` over the project's sources and puts what it writes in a directory
//     beside the executable, the way .NET puts `data_<csproj>_<platform>_<arch>` there;
//   - strips every `.verse` to a one-byte stub, so the scene's `ext_resource path=` still
//     resolves and the source does not ship (D10, C#'s ExportPlugin.cs:120-153);
//   - refuses a platform this bridge does not reach, with a sentence (R-PLAT-4);
//   - and, on Web, refuses `verse/runtime/backend != "vm"` and ships `verse_data` inside the `.pck`
//     at `res://verse_data` rather than beside an executable that does not exist there
//     (docs/phase-7.5-design.md §9, T6.1).
//
// The cook is a subprocess rather than a second engine in this one: two FEngineLoops in one
// process has never been tried and has no reason to be, and a subprocess's stdout is the export
// log (D3).
class VerseExportPlugin : public godot::EditorExportPlugin {
	GDCLASS(VerseExportPlugin, godot::EditorExportPlugin)

protected:
	static void _bind_methods();

public:
	godot::String _get_name() const override;

	void _export_begin(const godot::PackedStringArray &features, bool is_debug, const godot::String &path, uint32_t flags) override;
	void _export_file(const godot::String &path, const godot::String &type, const godot::PackedStringArray &features) override;
	void _export_end() override;

private:
	// Where the cooker wrote, for _export_end to remove. Empty when nothing was cooked.
	godot::String temp_dir;

	// Set when _export_begin has already said why this export carries no Verse, so _export_file
	// does not strip sources out of an export that was never going to work.
	bool refused = false;

	// The .gdextension this export rewrote to drop [dependencies], and what to put back in
	// _export_end -- empty when this export used the host backend and nothing was touched. See
	// _export_begin: the file is generated from what tools/build_host.py staged, not from any
	// project's `verse/runtime/backend`, so a vm-backend export has to withhold the runtime host
	// and tbbmalloc.dll for itself rather than have the library declare them conditionally.
	godot::String rewritten_gdextension_path;
	godot::String rewritten_gdextension_original;

	void say(int p_message_type, const godot::String &p_message);
};
