using { /Godot.org/Godot }

# T1.5 (docs/web-vm/spec/unification.md): what unification looks like from Verse source, run on
# Epic's VerseVM through the UE host. Every public zero-argument method is called by the probe
# driver in declaration order, each as its own VM entry, and every printed line is the record.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       <abs path>/tests/verse_probe/vm_unification_probe.verse --class vm_unification_probe
#
# Run it from bin/, where build_host.py leaves tbbmalloc.dll.
#
# Refused at compile time, and so not below: a local declared without a value (`X:int` in a
# function body) is error 3601, "Data definitions at this scope must be initialized with a value".
# That removes the one spelling that would have let a script read a local before binding it.

probe_pair := struct:
	A:int
	B:string

# A class is comparable only when it is <unique>; without it `C1 = C1` is error 3509.
probe_ref := class<unique>:
	V:int = 0

probe_fields := class:
	First:int = 3
	Second:int = 4
	block:
		Print("probe_fields block: First = {First}, Second = {Second}")

probe_needs := class:
	Given:int
	block:
		Print("probe_needs block: Given = {Given}")

vm_unification_probe := class(object):

	# Unifying two concrete values: equal succeeds, unequal fails.
	UnifyConcrete<public>()<transacts>:void =
		X := 5
		Print("UnifyConcrete: X = 5 {if (X = 5) then "succeeds" else "fails"}")
		Print("UnifyConcrete: X = 6 {if (X = 6) then "succeeds" else "fails"}")
		Print("UnifyConcrete: array = array {if (array{1, 2} = array{1, 2}) then "succeeds" else "fails"}")
		Print("UnifyConcrete: array = longer array {if (array{1, 2} = array{1, 2, 3}) then "succeeds" else "fails"}")
		Print("UnifyConcrete: string = string {if ("ab" = "ab") then "succeeds" else "fails"}")
		Print("UnifyConcrete: string = other string {if ("ab" = "ac") then "succeeds" else "fails"}")
		Print("UnifyConcrete: option = option {if (option{3} = option{3}) then "succeeds" else "fails"}")
		Print("UnifyConcrete: option = other option {if (option{3} = option{4}) then "succeeds" else "fails"}")
		Print("UnifyConcrete: int = float-valued int {if (2 = 2) then "succeeds" else "fails"}")
		Print("UnifyConcrete: 0.0 = -0.0 {if (0.0 = -0.0) then "succeeds" else "fails"}")

	# Structs compare field by field; class instances by identity.
	UnifyAggregates<public>()<transacts>:void =
		P := probe_pair{A := 1, B := "x"}
		Q := probe_pair{A := 1, B := "x"}
		R := probe_pair{A := 2, B := "x"}
		Print("UnifyAggregates: struct = equal struct {if (P = Q) then "succeeds" else "fails"}")
		Print("UnifyAggregates: struct = unequal struct {if (P = R) then "succeeds" else "fails"}")
		C1 := probe_ref{}
		C2 := probe_ref{}
		Print("UnifyAggregates: object = itself {if (C1 = C1) then "succeeds" else "fails"}")
		Print("UnifyAggregates: object = another object {if (C1 = C2) then "succeeds" else "fails"}")
		T1 := (1, "a", 2.5)
		T2 := (1, "a", 2.5)
		Print("UnifyAggregates: tuple = equal tuple {if (T1 = T2) then "succeeds" else "fails"}")
		M1 := map{1 => "a", 2 => "b"}
		M2 := map{1 => "a", 2 => "b"}
		M3 := map{2 => "b", 1 => "a"}
		Print("UnifyAggregates: map = equal map {if (M1 = M2) then "succeeds" else "fails"}")
		Print("UnifyAggregates: map = same pairs other order {if (M1 = M3) then "succeeds" else "fails"}")

	# A definition binds its name exactly once; `=` on an already-bound name is a test.
	DefinitionIsTest<public>()<transacts>:void =
		X := 7
		Y := X
		Print("DefinitionIsTest: Y = {Y}, X = Y {if (X = Y) then "succeeds" else "fails"}")

	# Class construction: field defaults are all in place before any block runs, and a field
	# without a default is supplied by the archetype before the block reads it.
	ConstructionOrder<public>()<transacts>:void =
		F := probe_fields{}
		N := probe_needs{Given := 11}
		Print("ConstructionOrder: F.Second = {F.Second}, N.Given = {N.Given}")
