// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "verse_host_abi.h"

#if VH_HOST_KIND == VH_HOST_KIND_COOKER

namespace GodotVerse {

/// One cooked package: the UPackage path it was saved from and the `.uasset` it was saved to.
///
/// The container step needs both -- a legacy cooked file carries no package name, and
/// FCookedPackageStore::GetPackageNameFromFileName is how IoStoreUtilities asks for one.
struct FCookedPackageFile
{
    FString PackageName;
    FString Filename;
};

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
AUTORTFM_DISABLE bool CookProjectPackages(const FString& OutDir, TArray<FCookedPackageFile>& OutWritten, FUtf8String& OutError);

/// Converts the loose cook into an IoStore container, which is the only form of a Verse package a
/// loader can read (phase-7b-design.md 3.1: FLinkerLoad has no Verse::VCell override, so every
/// cell reference in a `.uasset` consumes zero bytes and everything after it mis-parses).
///
/// Writes <OutDir>/verse_scripts.utoc and .ucas, and nothing else. A *global* container is
/// produced too, because CreateIoStoreContainerFiles will not run without one, but it is written
/// beside the loose cook and thrown away with it: all it carries is the script-objects chunk, and
/// a monolithic host resolves script imports from the registrations FAsyncLoadingThread2 makes in
/// memory (§3.5, measured -- the game loads with the global container deleted).
///
/// The loose files under LooseDir are the input and are not touched.
///
/// False with OutError set. The caller has already cooked; this does not.
AUTORTFM_DISABLE bool BuildCookedContainer(const FString& LooseDir, const FString& OutDir,
    const TArray<FCookedPackageFile>& Packages, FUtf8String& OutError);

} // namespace GodotVerse

#endif // VH_HOST_KIND == VH_HOST_KIND_COOKER
