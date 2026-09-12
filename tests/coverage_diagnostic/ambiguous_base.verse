using { /Godot.org/Godot }

# The one thing the pre-build scan cannot know. `widget` names a file in `left/` and another in
# `right/`, and EditorFileSystem asks for this script's base type from a scan thread before anything
# is built -- where the only way to resolve `widget` is to match the stem across res://, which now
# matches twice. The class picker falls back to Node and the build says so.
ambiguous_base := class(widget):
    Which<public>()<transacts>:string = "ambiguous"
