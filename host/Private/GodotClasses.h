// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VerseValue.h"

#include "Godot.vh_object.gen.h"
#include "Godot.godot_ref.gen.h"
#include "Godot.vh_signal.gen.h"
#include "Godot.variant.gen.h"

namespace verse {

/// The C++ shadow for Verse's `variant`: one Godot Variant, as fixed-width lanes.
///
/// Field for field with the Verse declaration in Godot.native.verse, which is where the reasoning
/// for the shape is. VNI checks the correspondence -- V_STATIC_ASSERT_HAS_DATA per field -- so a
/// lane added on one side and forgotten on the other fails the build rather than the run.
struct variant
{
	VARIANT_BODY();

public:
	int64 Tag = 0;
	int64 Ref = 0;
	int64 I0 = 0;
	int64 I1 = 0;
	int64 I2 = 0;
	int64 I3 = 0;
	double F0 = 0.0;
	double F1 = 0.0;
	double F2 = 0.0;
	double F3 = 0.0;
	double F4 = 0.0;
	double F5 = 0.0;
	double F6 = 0.0;
	double F7 = 0.0;
	double F8 = 0.0;
	double F9 = 0.0;
	double F10 = 0.0;
	double F11 = 0.0;
	double F12 = 0.0;
	double F13 = 0.0;
	double F14 = 0.0;
	double F15 = 0.0;
	verse::string Text;
};

/// The C++ shadow for Verse's `vh_object`, the root of the mirrored hierarchy. It exists so that a
/// user's `class(node2d)` has a UObject representation the host can instantiate and call into; it
/// carries no behaviour of its own, and every Godot class above it -- the mirror of Godot's own
/// Object included -- is ordinary Verse.
class vh_object : public UObject
{
	VH_OBJECT_BODY();

	// Verse API

public:
	TVal<int64> Handle;
};

/// The C++ shadow for Verse's `godot_ref`: an Array, a Dictionary, a Callable, a Signal or a
/// packed array, held as an id in the GDExtension's table.
///
/// It exists to have a destructor. A Verse value has none, but a native class is a UObject, and a
/// UObject is told when it is collected -- which is the only moment at which "Verse has dropped
/// this" is knowable. Measured before it was relied on: docs/abi-v2-design.md 1a.
class godot_ref : public UObject
{
	GODOT_REF_BODY();

	// Verse API

public:
	TVal<int64> Ref;

	/// Releases the table entry. Runs on the collection that finds this unreachable, so the entry
	/// outlives the Verse value by up to one cycle -- which is why the host asks for a cycle when
	/// the table grows rather than waiting to be asked.
	void BeginDestroy() override;
};

/// The C++ shadow for Verse's `vh_signal`: the binding half of a script-declared signal.
///
/// Id names a row the host keeps -- the owner's handle, the signal's name, and what its payload
/// decomposes into. Written at construction, the way vh_object's Handle is, so a script never
/// spells any of it twice.
class vh_signal : public UObject
{
	VH_SIGNAL_BODY();

	// Verse API

public:
	TVal<int64> Id;
};

} // namespace verse
