// Copyright Epic Games, Inc. All Rights Reserved.

#include "GodotClasses.h"

#include "AutoRTFM.h"
#include "HostRuntime.h"
#include "HostScript.h"

#include "Godot.vh_object.gen.ipp"
#include "Godot.godot_ref.gen.ipp"
#include "Godot.vh_signal.gen.ipp"
#include "Godot.variant.gen.ipp"

void verse::vh_object::BeginDestroy()
{
	// Open, because the release walks a host table and reaches a Godot callback in a DLL the
	// AutoRTFM compiler never saw. Nothing here is in a transaction -- this is the collector -- but
	// that is not something the compiler can be told, only shown.
	AutoRTFM::Open([this] { GodotVerse::ReleaseMintedPeer(this, Handle.Get()); });
	UObject::BeginDestroy();
}

void verse::godot_ref::BeginDestroy()
{
	// Ref is read outside any Verse context: BeginDestroy runs on the collector's terms, not on a
	// script's, and TVal's stored value is plain data by this point.
	GodotVerse::ReleaseGodotRef(Ref.Get());
	UObject::BeginDestroy();
}
