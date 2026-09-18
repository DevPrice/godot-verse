using { /Godot.org/Godot }

# Does `?node2d` satisfy a parameter declared `?object`?
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/option_variance_probe.verse --class probe_main
#
# A plain `node2d` satisfies `object`, because Verse subtyping is what it looks like. Whether an
# *option* of one satisfies an option of the other is a separate question, and it decides how a
# generated object argument is packed: one shared `VhFromMaybeObject(?object)` if options are
# covariant, or an unwrap inlined at each of the mirror's 865 call sites if they are not.
#
# Answered by whether this file compiles, and by which line is named if it does not.

probe_main := class:

    TakesMaybeObject<public>(O:?object)<transacts>:int =
        if (O?) then 1 else 0

    # 1. The option of a subclass, unwrapped and rewrapped by hand, which certainly works.
    PassRewrapped<public>(N:?node2d)<transacts>:int =
        if (V := N?) then TakesMaybeObject(option{V}) else TakesMaybeObject(false)

    # 2. The option of a subclass, as it stands. **This is the question.**
    PassMaybeSubclass<public>(N:?node2d)<transacts>:int =
        TakesMaybeObject(N)
