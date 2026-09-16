using { /Godot.org/Godot }

# R-EXP-7: a Verse script as an autoload singleton.
#
# Godot's own rules decide this stage, and the phase follows them rather than deviating:
# `_create_autoload` takes the script's `get_instance_base_type()` and refuses anything that is not
# a Node (`editor_autoload_settings.cpp:354-355`), and whether an autoload exists in the editor is
# `scr->is_tool()` (`:390`). So this one derives from `node` and carries no `@tool`, which makes it
# a singleton in a running game and absent from the editor -- the same bargain C# makes.
#
# Deliberately stateful. An autoload that only answered a constant would not show the thing that
# makes a singleton worth having: that every scene reaches the *same* object, so a write from one
# is visible to the next.

game_state := class(node):

    var Score<public>:int = 0

    var Label<public>:string = "fresh"

    AddScore<public>(Points:int)<transacts>:int =
        set Score = Score + Points
        Score

    Rename<public>(To:string)<transacts>:void =
        set Label = To

    Describe<public>()<transacts>:string = "{Label} {Score}"
