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

    # Verse interpolating the object itself. This does NOT reach the extension method below
    # directly -- `"{X}"` desugars to the free `ToString(X)`, which is the mirror's
    # `ToString(:object)` -- so it goes out to Godot's to_string() and comes back, which is the one
    # implementation serving both languages.
    DescribeSelf<public>()<transacts>:string = "{Self}"


# R-NODE-10's first hook, and the whole of what a script writes for it: an extension method, which
# is Verse's own spelling for "what this value prints as". Not a `_ToString` override -- there is no
# such virtual, deliberately, because a class member of that name cannot even be declared (it is
# ambiguous with /Verse.org/Verse's own ToString). `tests/verse_probe/tostring_probe.verse` carries
# the five rounds that settled it.
#
# Godot's `to_string()` finds this, so `print(GameState)` in GDScript shows it -- and so does Verse's
# own `"{Obj}"`, which gets here the long way round, out through Godot's to_string and back.
#
# **No `<public>`**, and that is not a style choice: an extension method may be no more accessible
# than the type it extends, and a script class written the ordinary way -- `game_state := class(node)`
# -- is internal. Writing `<public>` here is uLang glitch 3593, whose message talks about subpaths of
# /user@localhost and never mentions the word `<public>`.
(X:game_state).ToString()<transacts>:string = "game_state({X.Label} {X.Score})"
