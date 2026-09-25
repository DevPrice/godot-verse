using { /Godot.org/Godot }
using { /Verse.org/Concurrency }

# docs/web-vm/spec/tasks.md's third fixture: the corners of EndTask. A parent that ends while a
# child is running has to wait for it; a task whose cancellation was requested while it ran ends
# canceled even if it never suspends again; two race arms woken by one Signal produce one winner.
# Same driver contract as vm_tasks_probe.verse.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/vm_tasks_probe3.verse --class vm_tasks_probe3

vm_tasks_probe3 := class(object):

	EvP:event(int) = event(int){}
	EvGo:event(int) = event(int){}
	EvKid:event(int) = event(int){}
	EvQ:event(int) = event(int){}
	EvRace:event(int) = event(int){}

	var PTask:?task(int) = false
	var QTask:?task(int) = false

	Arm(Name:string, Ev:event(int))<suspends>:int =
		defer:
			Print("  {Name} defer")
		Print("  {Name} start")
		V := Ev.Await()
		Print("  {Name} end")
		V

	Watch(Name:string, T:task(int))<suspends>:void =
		Print("  {Name} awaiting")
		R := T.Await()
		Print("  {Name} got {R}")

	# --- a parent whose body ends while its child is the task that resumed it -------------

	PParent()<suspends>:int =
		defer:
			Print("  p parent defer")
		branch:
			defer:
				Print("  p child defer")
			EvGo.Await()
			Print("  p child signals the parent's event")
			EvP.Signal(0)
			Print("  p child after signal")
			EvKid.Await()
			Print("  p child end (never)")
		Print("  p parent waits on its event")
		EvP.Await()
		Print("  p parent body ends")
		7

	A1_Start():void =
		Print("A1 start")
		T := spawn{PParent()}
		set PTask = option{T}
		spawn{Watch("p-watcher", T)}
		Print("A1 end")

	A2_Go():void =
		Print("A2 go")
		EvGo.Signal(0)
		Print("A2 signal returned")

	# --- cancellation requested while running, then the body just ends ---------------------

	QParent()<suspends>:int =
		defer:
			Print("  q parent defer")
		EvQ.Await()
		Print("  q parent resumed")
		branch:
			defer:
				Print("  q child defer")
			Print("  q child cancels the parent")
			if (T := QTask?):
				T.Cancel()
			Print("  q child after cancel (never)")
		Print("  q parent body ends without suspending")
		8

	B1_Start():void =
		Print("B1 start")
		T := spawn{QParent()}
		set QTask = option{T}
		spawn{Watch("q-watcher", T)}
		Print("B1 end")

	B2_Signal():void =
		Print("B2 signal")
		EvQ.Signal(0)
		Print("B2 signal returned")

	B3_Phase():void =
		if (T := QTask?):
			if (T.Canceled[]):
				Print("B3 q task reads Canceled")
			if (T.Completed[]):
				Print("B3 q task reads Completed")

	# --- cancelling the task that resumed you ----------------------------------------------

	EvW:event(int) = event(int){}
	EvT:event(int) = event(int){}
	EvT2:event(int) = event(int){}
	var TTask:?task(int) = false

	TTarget()<suspends>:int =
		defer:
			Print("  t target defer")
		EvT.Await()
		Print("  t target signals the watcher's event")
		EvW.Signal(0)
		Print("  t target continues after Signal")
		EvT2.Await()
		Print("  t target end (never)")
		1

	TCanceller()<suspends>:void =
		EvW.Await()
		Print("  t canceller resumed, cancelling its resumer")
		if (T := TTask?):
			T.Cancel()
		Print("  t canceller: cancel returned")

	D1_Start():void =
		Print("D1 start")
		T := spawn{TTarget()}
		set TTask = option{T}
		spawn{TCanceller()}
		Print("D1 end")

	D2_Signal():void =
		Print("D2 signal")
		EvT.Signal(0)
		Print("D2 signal returned")

	# --- two race arms woken by one Signal ------------------------------------------------

	RDriver()<suspends>:void =
		X := race:
			Arm("r-arm1", EvRace)
			Arm("r-arm2", EvRace)
		Print("  race result {X}")

	C1_Start():void =
		Print("C1 start")
		spawn{RDriver()}
		Print("C1 end")

	C2_Signal():void =
		Print("C2 signal")
		EvRace.Signal(3)
		Print("C2 signal returned")
