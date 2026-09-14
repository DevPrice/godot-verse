using { /Godot.org/Godot }

# Spike S-5 (docs/phase-6-design.md 2): does the compiler accept `profile{}` in a /user@localhost
# package at Version::LatestUnstable? `profile` is in the compiler's reserved symbols
# (src/verse_keywords.h) and VVMInterpreter.cpp raises FVerseProfilingDelegates::OnEndProfilingEvent
# for the opcode, but reserved is not the same as available to a user package.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       <repo>/tests/verse_probe/profile_probe.verse --class profile_probe
#
# What a clean compile answers: the profiler's inner rows (D13's second half) are reachable, and an
# author can ask for one by writing the block.

profile_probe := class(object):

	Work<public>()<transacts>:int =
		var Total:int = 0
		profile("probe_tagged"):
			for (I := 0..99):
				set Total += I
		Total

	# Does it nest, and does a block with a value answer it?
	Nested<public>()<transacts>:int =
		profile("probe_outer"):
			profile("probe_inner"):
				7
