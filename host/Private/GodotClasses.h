// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VerseValue.h"

#include "Godot.object.gen.h"

namespace verse {

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
