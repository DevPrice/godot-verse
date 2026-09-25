using { /Godot.org/Godot }

# T1.4 (docs/web-vm/spec/modules.md): module-scope data, what may define it and what a script
# reads back. The first draft of this file gave every initializer a Print through a <transacts>
# helper, to watch the order they ran in; every one was refused at compile time with glitch 3582,
# "Divergent calls (calls that might not complete) cannot be used to define data-members" -- so a
# module-level initializer can call nothing a script can write, and has no observable side effect.
# Reading `LaterData` from an initializer above it was glitch 3502 ("Accessing a variable from the
# initializer of a variable that precedes it in the same snippet ... isn't implemented yet"), and
# a module-level object of a class without <computes> was glitch 3512 ("This archetype
# instantiation constructs a class that has the 'transacts' effect, which is not allowed by its
# context"). What remains below is what does compile.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/vm_modules_probe.verse --class vm_modules_probe

FirstData:int = 1
SecondData:int = FirstData + 1
Greeting:string = "hello"
Numbers:[]int = array{FirstData, SecondData, 3}

inner_mod := module:
	InnerData<public>:int = 100

UsesInner:int = inner_mod.InnerData + 1

plain_holder := class<computes>:
	Held:int = 7
	Label:string = "held"

GlobalHolder:plain_holder = plain_holder{Held := 9}

vm_modules_probe := class(object):

	ReadData<public>()<transacts>:void =
		Print("FirstData = {FirstData}")
		Print("SecondData = {SecondData}")
		Print("Greeting = {Greeting}")
		Print("Numbers.Length = {Numbers.Length}")
		Print("UsesInner = {UsesInner}")
		Print("GlobalHolder.Held = {GlobalHolder.Held}, Label = {GlobalHolder.Label}")
