extends SceneTree

# The driver half of the completion harness. tools/probe_complete.py copies this into the project
# under test, writes the positions beside it, and runs:
#
#     godot --headless --path <project> --script res://complete_probe_driver.gd
#
# It prints one JSON object per line, each what `_complete_code` would answer at one caret, and the
# Python half reads them off stdout and applies the rules. Nothing is asserted here, for the reason
# hover_probe.gd gives: a rule that lived in the project could only be checked against the project
# it lives in.
#
# The positions come from a file rather than being walked here, because a completion costs an
# analysis where a hover costs none -- the buffer the host is asked about carries a placeholder in
# place of the identifier being typed, so it differs per caret. Choosing them is the Python half's
# job, where the choice can be argued with.


const POSITIONS := "res://complete_probe_positions.json"


func _init() -> void:
	var lang: Object = _verse_language()
	if lang == null:
		printerr("complete_probe: no VerseScriptLanguage registered -- is the GDExtension loaded?")
		quit(1)
		return

	var text := FileAccess.get_file_as_string(POSITIONS)
	if text.is_empty():
		printerr("complete_probe: no %s -- the Python half writes it" % POSITIONS)
		quit(1)
		return
	var by_path: Dictionary = JSON.parse_string(text)
	if by_path == null:
		printerr("complete_probe: %s is not JSON" % POSITIONS)
		quit(1)
		return

	var paths := by_path.keys()
	paths.sort()
	var rows := 0
	for path in paths:
		# Flat (line, column) pairs, which is what the seam takes: a PackedInt32Array crosses the
		# binding without a per-element Variant, and there are thousands of them.
		var flat := PackedInt32Array()
		for pair in by_path[path]:
			flat.append(int(pair[0]))
			flat.append(int(pair[1]))
		for row in lang.call("probe_complete", path, flat):
			row["path"] = path
			print("COMPLETE\t%s" % JSON.stringify(row))
			rows += 1
	print("COMPLETE_DONE\t%d files\t%d rows" % [paths.size(), rows])
	quit(0)


func _verse_language() -> Object:
	for i in Engine.get_script_language_count():
		var lang := Engine.get_script_language(i)
		if lang != null and lang.get_class() == "VerseScriptLanguage":
			return lang
	return null
