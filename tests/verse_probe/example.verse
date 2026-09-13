using { /Godot.org/Godot }

# An example of what a probe file is for, and at the same time the fixture behind spike S-B
# (docs/phase-4-design.md 2), so the claim that decided Phase 4's signal design can be re-run rather
# than taken on trust:
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/example.verse --class example
#
# What it asks, and what a clean compile answers:
#
#   - a signal can be a typed class member whose type is parametric, and the member can be
#     constructed in the archetype;
#   - a payload of more than one value is a tuple, and `Struck.Subscribe(OnStruck)` typechecks with
#     `OnStruck(Damage:int, By:string)` -- a Verse function's parameter *is* its tuple, which is why
#     Godot can see N arguments while Verse sees one payload;
#   - a method reference is a function value, so Subscribe takes one with no lambda (Verse has none);
#   - a zero-payload signal is `tuple()`, and its handler decorates with no parameters at all.
#
# Verse's own `signalable`/`subscribable` are deliberately *not* implemented here: their domains are
# no_rollback, and every Godot callback runs inside a transaction. That is the finding, and it is
# why this shape is Verse's vocabulary rather than Verse's interfaces.
godot_signal_probe<public>(t:type) := class:
	Signal<public>(Val:t)<transacts>:void = {}

	Subscribe<public>(Callback(:t)<transacts>:void)<transacts>:cancelable = probe_cancel{}

probe_cancel<public> := class(cancelable):
	Cancel<override>()<transacts>:void = {}

example := class(object):

	Hit<public>:godot_signal_probe(tuple()) = godot_signal_probe(tuple()){}

	Struck<public>:godot_signal_probe(tuple(int, string)) = godot_signal_probe(tuple(int, string)){}

	OnHit<public>(Payload:tuple())<transacts>:void = {}

	OnStruck<public>(Damage:int, By:string)<transacts>:void = {}

	Emit<public>()<transacts>:void =
		Hit.Signal(())
		Struck.Signal((7, "probe"))

	Wire<public>()<transacts>:void =
		Unsub := Hit.Subscribe(OnHit)
		Unsub2 := Struck.Subscribe(OnStruck)
		Unsub.Cancel()
		Unsub2.Cancel()

	Answer<public>()<transacts>:int = 42
