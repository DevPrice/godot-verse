// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VerseValue.h"

#include "Godot.object.gen.h"
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

/// The C++ shadow for Verse's `object`. It exists so that a user's `class(node2d)`
/// has a UObject representation the host can instantiate and call into; it carries no behaviour
/// of its own, and every Godot class above it in the API is ordinary Verse.
class object : public UObject
{
	OBJECT_BODY();

	// Verse API

public:
	TVal<int64> Handle;
};

} // namespace verse
