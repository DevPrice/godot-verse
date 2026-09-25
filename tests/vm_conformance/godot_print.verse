using { /Godot.org/Godot }

# spec/godot-natives.md §8.1: Print<transacts> drives the Print callback, deferred to commit.

godot_print := class(object):

	PrintBasic<public>():void =
		Print("hello")

	PrintEmpty<public>():void =
		Print("")

	PrintUnicode<public>():void =
		Print("héllo wörld")

	PrintInterpolated<public>():void =
		Print("x={1 + 1} y={2.0 * 3.0}")
