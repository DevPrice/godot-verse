// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostCooked.h"

#include "HAL/FileManager.h"
#include "HostScript.h"
#include "HostSidecar.h"
#include "ISolarisModule.h"
#include "ISolarisRuntime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace {

/// Every mount point the cooker wrote under <Cooked>/, which is one directory per mount point and
/// a tree of `.uasset` beneath it. Read off the directory rather than from a list, so a package
/// added to the cook needs no second place to say so.
TArray<FString> CookedMountPoints(const FString& CookedDir)
{
    TArray<FString> Names;
    IFileManager::Get().FindFiles(Names, *(CookedDir / TEXT("*")), /*Files*/ false, /*Directories*/ true);
    return Names;
}

/// The Verse package directories inside one mount point: `<Cooked>/GodotScripts_1/_Verse.uasset`
/// is the package `GodotScripts_1`. A VNI package is `<mount>/_Verse/VNI/<module>.uasset` and is
/// not one of these -- Solaris loads those itself, by path, during module startup.
bool HasVersePackage(const FString& CookedDir, const FString& MountPoint)
{
    return IFileManager::Get().FileExists(*(CookedDir / MountPoint / TEXT("_Verse.uasset")));
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::RegisterCookedMountPoints(const FString& CookedDir, FUtf8String& OutError)
{
    if (!IFileManager::Get().DirectoryExists(*CookedDir))
    {
        OutError = FUtf8String(FString::Printf(
            TEXT("the cooked Verse directory %s is not there. Export the project again."), *CookedDir));
        return false;
    }

    const TArray<FString> MountPoints = CookedMountPoints(CookedDir);
    if (MountPoints.IsEmpty())
    {
        OutError = FUtf8String(FString::Printf(
            TEXT("%s holds no cooked Verse package. Export the project again."), *CookedDir));
        return false;
    }

    for (const FString& MountPoint : MountPoints)
    {
        const FString Root = FString::Printf(TEXT("/%s/"), *MountPoint);
        if (FPackageName::MountPointExists(Root))
        {
            // Already there: /Engine/ is, and the data directory is the engine directory, so it
            // already points where the cook wrote. Re-registering it would shadow it with the
            // same path and unregister badly later.
            continue;
        }
        FPackageName::RegisterMountPoint(Root, CookedDir / MountPoint / TEXT(""));
    }
    return true;
}

AUTORTFM_DISABLE bool GodotVerse::LoadCookedProject(const FString& CookedDir, FUtf8String& OutError)
{
    TSharedRef<ISolarisRuntime> Runtime = ISolarisModule::Get().GetRuntime();

    // A Verse package P is the UPackage /P/_Verse (VVMNames.cpp:342, 385). The VNI packages -- the
    // mirror, the standard library -- are not loaded here: Solaris loaded them itself during
    // module startup, out of the mount points RegisterCookedMountPoints put down first.
    FString ScriptPackageName;
    for (const FString& MountPoint : CookedMountPoints(CookedDir))
    {
        if (!HasVersePackage(CookedDir, MountPoint))
        {
            continue;
        }

        const FString PackagePath = FString::Printf(TEXT("/%s/_Verse"), *MountPoint);
        UPackage* Package = LoadPackage(nullptr, *PackagePath, LOAD_None);
        if (!Package)
        {
            OutError = FUtf8String(FString::Printf(TEXT("could not load the cooked package %s"), *PackagePath));
            return false;
        }
        Package->FullyLoad();
        Runtime->AddCompiledUPackage(Package);

        // The generation the cook published, which is always 1: the cooker is a fresh process and
        // CompileProject numbers from there (D20).
        if (MountPoint.StartsWith(TEXT("GodotScripts_")))
        {
            ScriptPackageName = MountPoint;
        }
    }

    if (ScriptPackageName.IsEmpty())
    {
        OutError = FUtf8String(FString::Printf(
            TEXT("%s holds cooked packages but none of the project's own code"), *CookedDir));
        return false;
    }

    AdoptCookedGeneration(FUtf8String(ScriptPackageName), 1);

    const FString SidecarPath = FPaths::GetPath(CookedDir) / TEXT("verse_classes.json");
    FUtf8String SidecarError;
    if (!LoadClassSidecar(SidecarPath, SidecarError))
    {
        OutError = SidecarError;
        return false;
    }
    return true;
}
