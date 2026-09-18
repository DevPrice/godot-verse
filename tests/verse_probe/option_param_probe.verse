using { /Godot.org/Godot }

# Does a plain object satisfy a parameter declared `?class`, or must the caller wrap it?
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/option_param_probe.verse --class probe_main
#
# The question decides the cost of declaring a generated parameter optional. Godot's own object
# arguments are nullable -- 908 of the mirror's 1020 are, by the `required` metadata the dump
# carries -- and Verse has no way to spell null for a parameter typed `node2d`. Declaring it
# `?node2d` is the fix; whether that is additive or a break across every call site in every
# project depends entirely on what this file compiles to.
#
# A clean compile means a plain value coerces and nothing breaks. A refusal at PassPlain names the
# line and the cost.

probe_main := class:

    # The declaration under test.
    TakesMaybe<public>(N:?node2d)<transacts>:int =
        if (N?) then 1 else 0

    # 1. The empty option, which is the whole point of the change.
    PassNothing<public>()<transacts>:int =
        TakesMaybe(false)

    # 2. A value wrapped by the caller, which is the spelling that certainly works.
    PassWrapped<public>(N:node2d)<transacts>:int =
        TakesMaybe(option{N})

    # 3. A plain value, unwrapped. **This is the question.** If it compiles, changing a generated
    #    declaration from `node2d` to `?node2d` costs no caller a character.
    PassPlain<public>(N:node2d)<transacts>:int =
        TakesMaybe(N)
