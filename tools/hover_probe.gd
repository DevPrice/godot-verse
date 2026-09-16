extends SceneTree

# The driver half of the hover harness. tools/probe_hover.py copies this into the project under
# test and runs it:
#
#     godot --headless --path <project> --script res://hover_probe_driver.gd
#
# It prints one JSON object per line, each the answer `_lookup_code` would give for one hover, and
# the Python half reads them off stdout and applies the rules. Nothing is asserted here: a rule
# that lived in the project could only be checked against the project it lives in, and the point
# of the harness is to point it at a corpus.
#
# The language object is reached by class name rather than by extension, because ScriptLanguage
# exposes no `get_name` to ClassDB -- `Engine.get_script_language(i)` hands back a bare
# ScriptLanguage and `get_class()` is the only question it answers about itself.


func _init() -> void:
	var lang: Object = _verse_language()
	if lang == null:
		printerr("hover_probe: no VerseScriptLanguage registered -- is the GDExtension loaded?")
		quit(1)
		return

	var scripts: PackedStringArray = _verse_scripts("res://")
	scripts.sort()
	var rows := 0
	for path in scripts:
		for row in lang.call("probe_hover", path):
			row["path"] = path
			print("HOVER\t%s" % JSON.stringify(row))
			rows += 1
	print("HOVER_DONE\t%d files\t%d rows" % [scripts.size(), rows])
	quit(0)


func _verse_language() -> Object:
	for i in Engine.get_script_language_count():
		var lang := Engine.get_script_language(i)
		if lang != null and lang.get_class() == "VerseScriptLanguage":
			return lang
	return null


func _verse_scripts(dir_path: String) -> PackedStringArray:
	var found := PackedStringArray()
	var dir := DirAccess.open(dir_path)
	if dir == null:
		return found
	dir.list_dir_begin()
	var entry := dir.get_next()
	while entry != "":
		if entry.begins_with("."):
			entry = dir.get_next()
			continue
		var full := dir_path.path_join(entry)
		if dir.current_is_dir():
			if entry != "addons":
				found.append_array(_verse_scripts(full))
		elif entry.ends_with(".verse"):
			found.append(full)
		entry = dir.get_next()
	dir.list_dir_end()
	return found
