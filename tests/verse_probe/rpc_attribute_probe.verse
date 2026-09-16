using { /Verse.org/Verse }

# Whether the `@rpc` attribute package HostScript.cpp injects can be declared at all.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/rpc_attribute_probe.verse --class rpc_attribute_probe
#
# The question is the one phase-4b-design.md §15 raised about `MakeVariant` and did not settle for
# this shape: **a `<constructor>` is a module-level function**, and module-level overloading is far
# narrower than a class's. Four arities of one constructor name is exactly the arrangement that
# was refused for `VariantFrom`, and the attribute package is compiled by the *host* at runtime --
# where a refusal is not a diagnostic against any script, so it fails silently.
#
# Declared here in user scope rather than read out of the package, because that is the only place
# a probe can put it.

@attribscope_function
rpc_attribute<public> := class<computes>(attribute) {}

rpc<public><constructor>(Mode:string)<computes> := rpc_attribute {}
rpc<public><constructor>(Mode:string, Second:string)<computes> := rpc_attribute {}
rpc<public><constructor>(Mode:string, Second:string, Third:string)<computes> := rpc_attribute {}
rpc<public><constructor>(Mode:string, Second:string, Third:string, Channel:int)<computes> := rpc_attribute {}

rpc_attribute_probe := class:

    @rpc("authority")
    One<public>():int = 1

    @rpc("any_peer", "call_local")
    Two<public>():int = 2

    @rpc("any_peer", "call_local", "reliable")
    Three<public>():int = 3

    @rpc("any_peer", "call_local", "reliable", 3)
    Four<public>():int = 4
