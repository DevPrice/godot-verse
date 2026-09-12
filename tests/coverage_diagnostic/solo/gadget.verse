using { /Godot.org/Godot }

# Declared in exactly one module, which is the case R-TOOL-12 can act on: needs_import.verse at the
# project root calls this without importing anything, and the editor is expected to say which one
# line would fix it -- and, when the file is the one on screen, to write that line itself.
SoloHelper<public>():int = 7

gadget := class(node2d):
    Which<public>()<transacts>:string = "solo"
