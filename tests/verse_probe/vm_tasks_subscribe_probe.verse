using { /Godot.org/Godot }
using { /Verse.org/Concurrency }

# docs/web-vm/spec/tasks.md: the order a subscribable event calls its subscribers in, and where its
# awaiters fall relative to them. Run it more than once -- the question is whether the order
# changes between runs.
#
# Subscribe is reached through a `listenable(int)`: the Godot package's own extension method
# `event(t).Subscribe` makes an unqualified `Subscribe` on a subscribable_event_intrnl ambiguous
# (glitch 3518), and a qualified member access does not find it (3506).
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/vm_tasks_subscribe_probe.verse --class vm_tasks_subscribe_probe

vm_tasks_subscribe_probe := class(object):

	Ev:subscribable_event_intrnl(int) = subscribable_event_intrnl(int){}

	H1(X:int):void = Print("  sub 1 got {X}")
	H2(X:int):void = Print("  sub 2 got {X}")
	H3(X:int):void = Print("  sub 3 got {X}")
	H4(X:int):void = Print("  sub 4 got {X}")
	H5(X:int):void = Print("  sub 5 got {X}")
	H6(X:int):void = Print("  sub 6 got {X}")

	Waiter()<suspends>:void =
		V := Ev.Await()
		Print("  awaiter got {V}")

	A1_Subscribe():void =
		L:listenable(int) = Ev
		L.Subscribe(H1)
		L.Subscribe(H2)
		L.Subscribe(H3)
		L.Subscribe(H4)
		L.Subscribe(H5)
		L.Subscribe(H6)
		spawn{Waiter()}
		Print("A1 subscribed six and one awaiter")

	A2_Signal():void =
		Print("A2 signal")
		Ev.Signal(1)
		Print("A2 signal returned")

	A3_Signal():void =
		Print("A3 signal")
		Ev.Signal(2)
		Print("A3 signal returned")
