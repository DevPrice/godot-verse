// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/UnrealString.h"

namespace GodotVerse {

/// Registers one mount point per package directory the cooker wrote, and does nothing else.
///
/// Separate from the load below, and called *before* ISolarisModule::Get(), because Solaris loads
/// its own VNI packages during module startup: FSolarisModule::JitVniPackages asks
/// FPackageName::DoesPackageExist for each one (SolarisModule.cpp:3372-3404), and a mount point
/// registered after that has nothing left to answer. The mirror lands under /Engine/, which needs
/// no registration because the data directory *is* the engine directory (D7); the standard
/// library's packages are under a plugin's mount point, which does not exist in a monolithic
/// Program and so does.
AUTORTFM_DISABLE bool RegisterCookedMountPoints(const FString& CookedDir, FUtf8String& OutError);

/// Loads the cooked project an exported game ships, from the directory vh_init was handed.
///
/// A host with no compiler gets Verse from cooked UPackages and from nothing else. This is the
/// half that is the project's own: load each package the cooker wrote, hand it to the runtime,
/// and read back the class sidecar as the analysis snapshot the class-describing entry points
/// answer from.
///
/// False with OutError set, and the caller turns that into VH_ERR_INIT with the path named.
AUTORTFM_DISABLE bool LoadCookedProject(const FString& CookedDir, FUtf8String& OutError);

/// Unmounts the containers and drops every reference this file holds, which vh_shutdown must do
/// *before* AppExit: the backends and the container headers are allocated through GMalloc, and
/// nothing allocated through GMalloc may survive into static destruction.
AUTORTFM_DISABLE void ReleaseCookedContainers();

} // namespace GodotVerse
