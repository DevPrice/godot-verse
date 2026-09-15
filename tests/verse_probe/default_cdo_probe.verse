using { /Godot.org/Godot }

# R-NODE-3, fifth spike, and the one that decides between the two routes to a bare `helper{}`.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/default_cdo_probe.verse --class default_cdo_probe
#
# Two places in a class body could mint an object's Godot peer:
#
#   the data-member default   `Handle<native>:int = VhMintPeer()`, legal only for a `<converges>`
#                             native, since SemanticAnalyzer.cpp:15256 tests `EEffect::diverges`
#   the `block:` clause       proved per-instance by class_block_probe.verse
#
# The default route is the one that would need no `var` and no `Self`. What decides it is not
# whether it compiles but *when it is evaluated*, and SemanticAnalyzer.cpp:15262 says:
#
#   "...the CDO trickery we do instead of evaluating the default initializer on class instantiation"
#
# If a default is evaluated once onto the class default object, one Godot peer would serve every
# instance of `helper` and the route is dead however well its rollback behaves. This asks that
# directly, and needs no native to do it: a default that reads a *sibling field the archetype
# supplies* can only answer per-instance if it is evaluated per instance.
#
#   D1  a default reading a sibling the archetype supplied
#   D2  the same through a subclass, which is R-NODE-3's shape
#
# **What it answered: the default route is dead, and for a blunter reason than CDO evaluation.** The
# file does not compile:
#
#   error 3502: Accessing instance member `Seed` from this scope is not yet implemented.
#
# A data-member default cannot read the instance *at all* -- not a sibling field, and so not `Self`
# either. The error is its result, the way `async_reject.verse`'s are.
#
# That closes the `<converges><native>` route independently of anything about rollback. Such a
# native would compile in a default -- SemanticAnalyzer.cpp:15256 tests only `EEffect::diverges` and
# a total native has it clear -- but it could be handed nothing to identify the object being
# constructed, so it could not learn which Godot class to mint. A `helper` deriving from
# `ref_counted` and one deriving from `resource` would reach the same argument-less call. Nor can
# the mirror give each generated class its own default: a member may not shadow an inherited one.
#
# `class_block_probe.verse` B3 answered 16 for the same per-instance question asked of a `block:`
# clause, and `class_block_self_probe.verse` S3 showed the block sees `Self` already at its derived
# type. The two are genuinely different mechanisms, and only the block clause can mint a peer per
# object and know what to mint.

probe_default<public> := class:
    Seed<public>:int = 0
    # No call -- just an expression over a sibling. A `<converges>` native would sit exactly here.
    Doubled<public>:int = Seed * 2

probe_default_derived<public> := class(probe_default):
    Own<public>:int = 0

default_cdo_probe := class(object):

    # D1 -- 10 means defaults are per-instance; 0 means they are evaluated against the class.
    D1<public>()<transacts>:int =
        V := probe_default{Seed := 5}
        V.Doubled

    # D2 -- the subclass case. 42 means per-instance.
    D2<public>()<transacts>:int =
        V := probe_default_derived{Seed := 21, Own := 1}
        V.Doubled

    Answer<public>()<transacts>:int = 42
