using { /Godot.org/Godot }

# R-EXP-5: a script that also runs in the editor.
#
# `@tool` is the bridge's own attribute, declared in the package the host adds at runtime beside
# `@global_class` and `@export`, so this file compiling at all is what says the attribute resolves.
# What it buys is Ready and Process running while the scene is merely open -- which nothing headless
# can observe, because there is no editor to be a hint of. Godot's own question is Script.is_tool(),
# and that is answered from this text rather than from the host: it is asked of scripts the editor
# has only scanned.
@tool
tool_probe := class(node2d):

    Ready<override>():void =
        Print("tool_probe ready")

    Which<public>()<transacts>:string = "tool"
