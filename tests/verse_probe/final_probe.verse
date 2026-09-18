using { /Godot.org/Godot }

# Spike 2 of docs/generated-bindings.md §7: is `<final>` reachable from a `/user@localhost` package
# at all? The decision not to use it for generated bindings only means something if it is.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/final_probe.verse --class final_probe
#
# It is: this file compiles and `AskFinal` answers 7. `final_reject.verse` is the other half -- the
# subclass, refused with glitch **3569**, which is the number `generated-bindings.md` §4 guessed at
# as `ErrSemantic_FinalSuperclass` and did not have.
#
# So `<final>` is available and is deliberately not used. Marking a binding class final would refuse
# the generated `boss := class(mob)` that a GDScript hierarchy forces, and that refusal would come
# from the language rather than from the bridge -- with no way to say which of the three inheritance
# cases in §5 the author had actually written.

final_base<public> := class<final>(object):
	Ask<public>():int = 7

final_probe := class(object):
	AskFinal<public>():int = final_base{}.Ask()
