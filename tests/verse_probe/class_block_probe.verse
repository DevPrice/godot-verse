using { /Godot.org/Godot }

# R-NODE-3, third spike, and the one that found the mechanism.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/class_block_probe.verse --class class_block_probe
#
# `construct_probe.verse` established that `helper{}` compiles and that the inherited `Handle` is
# unwritable from user scope (3593); `ctor_delegate_probe.verse` closed constructor delegation
# (3552) and calls in data-member defaults (3582). What neither asked about is the third thing a
# class body may contain, which `SemanticAnalyzer.cpp:15266` names in passing while explaining why
# the second is banned:
#
#   "This code was previously working around this restriction by using a do clause and mutation,
#    which is evaluated each time the class is instantiated, and so we can allow it to have side
#    effects."
#
# A `block:` clause in a class body is analysed at SemanticAnalyzer.cpp:18937 and is legal on any
# class. If it runs per instantiation, carries effects, and runs for a *subclass* the user writes,
# then the mirror can give `vh_object` a block that mints the object's Godot peer, and a bare
# `helper{}` needs no new spelling at all.
#
#   B1  does a class `block:` clause run at all, and can it `set` a `var` member?
#   B2  does it run for a derived class the archetype names?
#   B3  does it run once per *instance* -- derived from a field the archetype supplies, so a value
#       that differs between two constructions can only come from a per-instance evaluation
#   B4  may the block call a `<transacts>` function, which is what minting a Godot peer would be?
#
# **What it answered: yes to all four.** B1 = 7, B2 = 7, B3 = 16, B3Derived = 42.
#
#   B1  the clause runs, and `set` on a `var` member works from it.
#   B2  it runs for a *derived* class the archetype names -- the base declares the block, the user
#       writes `derived{}`, and the base's block still runs. This is R-NODE-3's case exactly.
#   B3  **per instance, not once for the class.** 16 is 3*2 + 5*2: each construction evaluated the
#       block against the `Seed` its own archetype supplied. A CDO evaluation would have answered 0,
#       which is what SemanticAnalyzer.cpp:15262's "CDO trickery" remark had made the worry.
#   B4  `Doubler` is `<transacts>` and the block calls it, so effects are legal inside the clause --
#       which is exactly what the 15266 comment says a do clause is allowed that a default is not.
#
# So the mirror can give `vh_object` a `block:` that mints the object's Godot peer, and a bare
# `helper{}` needs no new spelling. `class_block_self_probe.verse` asks the two questions this
# leaves: whether the block can reach `Self`, and what happens on the host's own construction path.

Doubler<public>(X:int)<transacts>:int = X * 2

probe_block<public> := class:
    Seed<public>:int = 0
    var Marked<public>:int = 0
    var Derived<public>:int = 0

    # B1/B3/B4 together: a `set` of a var member, reading a field the archetype supplied, through
    # a `<transacts>` call.
    block:
        set Marked = 7
        set Derived = Doubler(Seed)

# B2 -- the shape that matters: the base declares the block, the archetype names the subclass.
probe_block_derived<public> := class(probe_block):
    Own<public>:int = 0

class_block_probe := class(object):

    # B1 -- the block ran and wrote the member.
    B1<public>()<transacts>:int =
        V := probe_block{}
        V.Marked

    # B2 -- the base's block ran for a derived archetype.
    B2<public>()<transacts>:int =
        V := probe_block_derived{Own := 1}
        V.Marked

    # B3 -- two constructions with different seeds. 16 means each instance evaluated the block
    # against its own Seed; 0 means it ran once for the class and saw neither.
    B3<public>()<transacts>:int =
        A := probe_block{Seed := 3}
        B := probe_block{Seed := 5}
        A.Derived + B.Derived

    # B3 again through the subclass, which is the case R-NODE-3 actually needs.
    B3Derived<public>()<transacts>:int =
        V := probe_block_derived{Seed := 21, Own := 1}
        V.Derived

    Answer<public>()<transacts>:int = 42
