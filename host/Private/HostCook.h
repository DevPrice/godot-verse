// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "verse_host_abi.h"

#if VH_HOST_KIND == VH_HOST_KIND_COOKER

namespace GodotVerse {

/// Saves the Verse packages the running program holds as cooked UPackages under OutDir, which is
/// what a compiler-less Solaris loads and the only thing it loads (SolarisModule.cpp:2040-2055,
/// :3372-3404).
///
/// Three packages, at the paths the runtime host mounts them from:
///   <OutDir>/Cooked/GodotScripts_1/_Verse.uasset   the project's own code
///   <OutDir>/Cooked/GodotAttributes/_Verse.uasset  @export and friends, compiled from a string
///   <OutDir>/Engine/Content/_Verse/VNI/VerseHost.uasset   the mirror of Godot's API
///
/// False with OutError set on the first one that will not save. The caller has already compiled;
/// this does not.
AUTORTFM_DISABLE bool CookProjectPackages(const FString& OutDir, TArray<FString>& OutWritten, FUtf8String& OutError);

} // namespace GodotVerse

#endif // VH_HOST_KIND == VH_HOST_KIND_COOKER
