using { /Godot.org/Godot }

# R-NODE-3, fourth spike: the two things `class_block_probe.verse` left open once it showed that a
# class `block:` clause runs per instance, runs for a derived archetype, and may carry effects.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/class_block_self_probe.verse --class class_block_self_probe
#
# For the mirror to mint the *right* Godot peer, the block on `vh_object` has to tell the host which
# class is being constructed -- and the block is declared on the base, which does not know it is a
# `helper`. The only thing that could carry that is the object itself.
#
#   S1  may a class `block:` clause name `Self`?
#   S2  may it pass `Self` to a function, which is what a minting native would need?
#   S3  does the block see the *derived* class through Self -- i.e. is the object already its final
#       type when the base's block runs, or still a bare base?
#   S4  does a block on a class the **host** instantiates (this probe class itself, built by
#       vh_instantiate) also run? If it does, the host path would mint a second peer for every
#       script instance and the design has to suppress one of the two.
#
# **What it answered.** S1 = 1, S3 = 2, S4 = 99.
#
#   S1/S2  a class `block:` clause may name `Self` and pass it to a function. It compiles.
#   S3     **the object is already its derived type when the base's block runs.** `Identify(Self)`
#          dispatches `Shape` virtually and answers 2, the override, not 1. So a native handed
#          `Self` from `vh_object`'s block can resolve the *concrete* Verse class -- which is what
#          decides which Godot class to mint, through the mapping GodotClassNames.gen.h already
#          carries both ways.
#   S4     **a block runs on the host's `vh_instantiate` path too** -- 99, on a class the probe
#          built through the ABI rather than from Verse. So the minting native cannot mint
#          unconditionally: the host already made a peer for a script instance, and a second one
#          would leak per node. The host has to mark its own construction and have the native
#          answer the handle it already holds. Which side of the host's Handle write the block lands
#          on is the one ordering detail this probe cannot see, and a flag settles it either way.

# S2/S3's instrument: a free function taking the base type, answering something only a subclass
# could make true. `Shape` is overridden, so a virtual dispatch through Self reports the real class.
Identify<public>(O:probe_self)<transacts>:int = O.Shape()

probe_self<public> := class:
    var Seen<public>:int = 0

    Shape<public>()<transacts>:int = 1

    # S1/S2/S3 together.
    block:
        set Seen = Identify(Self)

probe_self_derived<public> := class(probe_self):
    Shape<override>()<transacts>:int = 2

class_block_self_probe := class(object):

    # S4 -- did this class's own block run, under vh_instantiate?
    var HostBuilt<public>:int = 0

    block:
        set HostBuilt = 99

    # S1/S2 -- the base's block named Self and passed it on.
    S1<public>()<transacts>:int =
        V := probe_self{}
        V.Seen

    # S3 -- 2 means the object was already its derived type when the base's block ran; 1 means the
    # block saw a bare base and a minting native could not learn the concrete class from Self.
    S3<public>()<transacts>:int =
        V := probe_self_derived{}
        V.Seen

    # S4 -- 99 means a block runs on the host's vh_instantiate path too.
    S4<public>()<transacts>:int = HostBuilt

    Answer<public>()<transacts>:int = 42
