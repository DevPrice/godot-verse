using { /Godot.org/Godot }

# What the debugger cases in host_smoke.cpp drive (R-DIAG-4). The line numbers matter: the cases
# name them, so a line added above one moves the breakpoint it arms.
debug_probe := class(object):

	Health<public>:int = 7

	Label<public>:string = "probe"

	# Line 13 binds `Total`; 14, 15 and 16 are the three statements after it.
	Count<public>()<transacts>:int =
		Total := Helper()
		Doubled := Total * 2
		Named := "stopped"
		Doubled + Named.Length

	# Called from Count, so a stop inside it has a caller frame -- which is what the stack depth
	# and the step-over relation are checked against.
	Helper<public>()<transacts>:int =
		Inner := 21
		Inner

	# The profiler's opt-in inner row (R-DIAG-5). A Verse function called from another Verse
	# function has no row of its own -- the VM offers no per-call hook -- so this block is how an
	# author asks for one, and its tag is what the row is named after.
	Tagged<public>()<transacts>:int =
		var Total:int = 0
		profile("smoke_tag"):
			for (I := 0..9):
				set Total += I
		Total

	# Nothing calls this from Verse; the profiler case calls it a known number of times and checks
	# the count is exactly that.
	Tick<public>()<transacts>:int = Health
