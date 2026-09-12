// Copyright Epic Games, Inc. All Rights Reserved.

#include "GodotClasses.h"

#include "HostRuntime.h"

#include "Godot.object.gen.ipp"
#include "Godot.godot_ref.gen.ipp"
#include "Godot.variant.gen.ipp"

void verse::godot_ref::BeginDestroy()
{
	// Ref is read outside any Verse context: BeginDestroy runs on the collector's terms, not on a
	// script's, and TVal's stored value is plain data by this point.
	GodotVerse::ReleaseGodotRef(Ref.Get());
	UObject::BeginDestroy();
}
