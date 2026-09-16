using { /Godot.org/Godot }

# R-EXP-1's remaining five, and R-EXP-8's `@icon`.
#
# The rule R-EXP-1 states is that an export is **type-driven where the Verse type can say it, and an
# attribute only where it cannot**. Everything a bounded number, an enum or a mirrored class implies
# is already read off the declaration and needs nothing written. These five are the set where it
# cannot: the declared type is `string` or `int`, and neither says whether the value is a path, a
# directory, a paragraph, a bitmask or a node.
#
# Each takes one string or none, for the reason `@rpc` does -- the toolchain refuses a
# several-argument attribute today, and says so in words that read as unfinished rather than
# decided. `docs/spec.md` R-EXP-9 has the two refusals and what to move to when either lifts.
#
# `@icon` is not read here at all: it is scanned out of the source text by `verse_scan_class_decl`,
# because Godot asks `get_class_icon_path` of a script it has merely scanned, before anything has
# been built. It is declared in the attribute package so that a script carrying it compiles, and
# read twice for the same reason `@global_class` is.
@icon("res://icon.svg")
@global_class
hints := class(node2d):

    # A path, filtered Godot's own way. `*` would accept any file.
    @export
    @export_file("*.png,*.jpg")
    var Portrait<public>:string = ""

    # A directory rather than a file. A marker with no argument, like `@tool`.
    @export
    @export_dir
    var SaveFolder<public>:string = ""

    # A paragraph: Godot draws a multi-line box instead of a one-line field.
    @export
    @export_multiline
    var Notes<public>:string = ""

    # A bitmask over named bits, which Verse has no flag enum for. Comma separated, which is
    # Godot's own spelling for this hint -- so the string passes through untranslated.
    @export
    @export_flags("Fire,Water,Earth")
    var Elements<public>:int = 0

    # A NodePath, which is a `string` on this wire. The argument is the class a picked node must be.
    @export
    @export_node_path("Node2D")
    var Target<public>:string = ""

    # An ordinary export beside them, so a case can say the five did not disturb what was already
    # working: a bounded int still gets its range off the type with nothing written.
    @export
    var Plain<public>:int = 7

    # The pairing the author is the only one who can get right, and the one the bridge can check:
    # `@export_flags` describes an `int` and this is a string. Refused with a sentence at the
    # member rather than drawn as a plain field, which is what it would otherwise be -- and which
    # is also what no attribute at all draws, so the mistake is invisible without the diagnostic.
    @export
    @export_flags("Fire,Water")
    var Mismatched<public>:string = ""

    ReadPortrait<public>()<transacts>:string = Portrait
    ReadElements<public>()<transacts>:int = Elements
