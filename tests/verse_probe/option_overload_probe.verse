using { /Godot.org/Godot }

# May an overload set hold both `f(:node2d)` and `f(:?node2d)`?
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/option_overload_probe.verse --class probe_main
#
# `option_param_probe.verse` establishes that a plain value does not coerce to an option, so
# declaring a generated parameter `?node2d` costs every existing call site an `option{}`. An
# overload would cost none -- if Verse accepts the pair. The rule to beat is the emptiable family
# (CLAUDE.md, `variant_api_probe.verse`): an overload set may hold at most one parameter that is a
# `logic`, an option or an array, and `?node2d` is one while `node2d` is not, so the pair should be
# inside the rule rather than outside it.
#
# Two answers matter and they are separate. Whether the *declarations* are accepted, and whether a
# *call* then resolves -- a set that compiles but makes every call ambiguous is no use.

probe_main := class:

    Take<public>(N:node2d)<transacts>:int = 1

    Take<public>(N:?node2d)<transacts>:int = 0

    # Does a plain value pick the first?
    CallPlain<public>(N:node2d)<transacts>:int =
        Take(N)

    # And the empty option the second?
    CallNothing<public>()<transacts>:int =
        Take(false)
