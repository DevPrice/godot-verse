// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostCooked.h"

#include "FileIoDispatcherBackend.h"
#include "FilePackageStore.h"
#include "HAL/FileManager.h"
#include "HostScript.h"
#include "HostSidecar.h"
#include "IO/IoContainerHeader.h"
#include "IO/IoDispatcher.h"
#include "IO/PlatformIoDispatcher.h"
#include "IoDispatcherFileBackend.h"
#include "ISolarisModule.h"
#include "ISolarisRuntime.h"
#include "Misc/AES.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/PackageStore.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace {

/// The I/O dispatcher backend and the package store backend the containers are mounted into, which
/// have to outlive every load. This is FPakPlatformFile's pair of members (IPlatformFilePak.h:2136)
/// held by a host that has no FPakPlatformFile: there is no pak here, only the containers the
/// cooker wrote, and the mount is the iostore half of FPakPlatformFile::Mount with the pak half
/// removed.
TSharedPtr<UE::IoStore::IFileIoDispatcherBackend> GIoBackend;
TSharedPtr<FFilePackageStoreBackend> GPackageStoreBackend;
TArray<TUniquePtr<FIoContainerHeader>> GContainerHeaders;
TArray<FString> GMountedTocs;

/// The sidecar sits beside the cooked directory rather than in it, so both halves of this file
/// agree on where to look.
FString SidecarPathFor(const FString& CookedDir)
{
    return FPaths::GetPath(CookedDir) / TEXT("verse_classes.json");
}

/// `/GodotScripts_1/_Verse` -> `GodotScripts_1`. Empty for a path this does not understand.
FString MountPointOf(const FString& PackagePath)
{
    FString Rest = PackagePath;
    if (!Rest.RemoveFromStart(TEXT("/")))
    {
        return FString();
    }
    FString MountPoint;
    if (!Rest.Split(TEXT("/"), &MountPoint, &Rest))
    {
        return FString();
    }
    return MountPoint;
}

/// A VNI package -- the mirror, the standard library -- is Solaris's to load during module startup.
/// Everything else is the project's own and is this file's.
bool IsVniPackage(const FString& PackagePath)
{
    return PackagePath.Contains(TEXT("/_Verse/VNI/"));
}

/// Mounts every container the cooker wrote under CookedDir.
///
/// The global container is mounted into the I/O dispatcher and *not* into the package store, which
/// is what FPakPlatformFile::Initialize does with it (IPlatformFilePak.cpp:5834-5855): it carries
/// the script-objects chunk and no container header, and nothing in a monolithic host reads that
/// chunk -- script imports resolve from the registrations FAsyncLoadingThread2 makes in memory.
bool MountCookedContainers(const FString& CookedDir, FUtf8String& OutError)
{
    TArray<FString> TocNames;
    IFileManager::Get().FindFiles(TocNames, *(CookedDir / TEXT("*.utoc")), /*Files*/ true, /*Directories*/ false);
    if (TocNames.IsEmpty())
    {
        OutError = FUtf8String(FString::Printf(
            TEXT("%s holds no cooked Verse container. Export the project again."), *CookedDir));
        return false;
    }

    if (!FIoDispatcher::IsInitialized())
    {
        OutError = UTF8TEXT("the I/O dispatcher is not running, so no cooked Verse container can be mounted");
        return false;
    }

    // Constructed is not running. LaunchEngineLoop brings the dispatcher up only under
    // USE_IO_DISPATCHER, which is `WITH_ENGINE || WITH_IOSTORE_IN_EDITOR || !(IS_PROGRAM ||
    // WITH_EDITOR)` (LaunchEngineLoop.cpp:98-99) -- all three false for this host, a Program with
    // no Engine -- so the only thing that ever touched the dispatcher was the async loader's
    // `Initialize()` (AsyncPackageLoader.cpp:195-200), which allocates it and nothing more.
    // Without this call Mount takes the backend and neither initializes it nor starts the
    // dispatcher thread (IoDispatcher.cpp:643-659), and every read is issued and never completes:
    // no error, no timeout, a package that stays queued forever. Idempotent.
    FIoDispatcher::InitializePostSettings();

    // Which backend is FPakPlatformFile's own choice, made the same way: the platform I/O
    // dispatcher exists only when this build and this command line enabled it, and the file
    // backend that goes with it is a different class.
    if (UE::FPlatformIoDispatcher::TryGet())
    {
        GIoBackend = UE::IoStore::MakeFileIoDispatcherBackend();
    }
    else
    {
        GIoBackend = CreateIoDispatcherFileBackend();
    }
    FIoDispatcher::Get().Mount(GIoBackend.ToSharedRef());

    GPackageStoreBackend = MakeShared<FFilePackageStoreBackend>();
    FPackageStore::Get().Mount(GPackageStoreBackend.ToSharedRef());

    for (const FString& TocName : TocNames)
    {
        const FString TocPath = CookedDir / TocName;
        GMountedTocs.Add(TocPath);
        TIoStatusOr<FIoContainerHeader> Mounted =
            GIoBackend->Mount(*TocPath, /*Order*/ 0, FGuid(), FAES::FAESKey());
        if (!Mounted.IsOk())
        {
            OutError = FUtf8String(FString::Printf(TEXT("could not mount %s: %s"),
                                                   *TocPath, *Mounted.Status().ToString()));
            return false;
        }
        if (FPaths::GetBaseFilename(TocName) == TEXT("global"))
        {
            continue;
        }
        TUniquePtr<FIoContainerHeader>& Header =
            GContainerHeaders.Add_GetRef(MakeUnique<FIoContainerHeader>(Mounted.ConsumeValueOrDie()));
        GPackageStoreBackend->Mount(Header.Get(), /*Order*/ 0);
    }
    return true;
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

    if (!MountCookedContainers(CookedDir, OutError))
    {
        return false;
    }

    // The bytes come from the container, but a package is still loaded by name, and a name outside
    // a registered mount point is not a package path at all -- FPackageName::DoesPackageExistEx
    // answers None for an unmounted path before it ever asks the I/O dispatcher
    // (PackageName.cpp:2474-2477). A container header carries package *ids*, which are hashes, so
    // the names have to come from somewhere else: the sidecar carries the list the cook wrote.
    TArray<FString> Packages;
    int32 Generation = 0;
    if (!ReadCookedManifest(SidecarPathFor(CookedDir), Packages, Generation, OutError))
    {
        return false;
    }

    for (const FString& PackagePath : Packages)
    {
        const FString MountPoint = MountPointOf(PackagePath);
        if (MountPoint.IsEmpty())
        {
            continue;
        }
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

AUTORTFM_DISABLE void GodotVerse::ReleaseCookedContainers()
{
    if (GPackageStoreBackend.IsValid())
    {
        for (const TUniquePtr<FIoContainerHeader>& Header : GContainerHeaders)
        {
            GPackageStoreBackend->Unmount(Header.Get());
        }
    }
    GContainerHeaders.Empty();

    if (GIoBackend.IsValid())
    {
        for (const FString& TocPath : GMountedTocs)
        {
            GIoBackend->Unmount(*TocPath);
        }
    }
    GMountedTocs.Empty();

    GPackageStoreBackend.Reset();
    GIoBackend.Reset();
}

AUTORTFM_DISABLE bool GodotVerse::LoadCookedProject(const FString& CookedDir, FUtf8String& OutError)
{
    TSharedRef<ISolarisRuntime> Runtime = ISolarisModule::Get().GetRuntime();


    TArray<FString> Packages;
    int32 Generation = 0;
    if (!ReadCookedManifest(SidecarPathFor(CookedDir), Packages, Generation, OutError))
    {
        return false;
    }

    // A Verse package P is the UPackage /P/_Verse (VVMNames.cpp:342, 385). The VNI packages -- the
    // mirror, the standard library -- are not loaded here: Solaris loaded them itself during
    // module startup, out of the containers MountCookedContainers put down first.
    FString ScriptPackageName;
    for (const FString& PackagePath : Packages)
    {
        if (IsVniPackage(PackagePath))
        {
            continue;
        }

        UPackage* Package = LoadPackage(nullptr, *PackagePath, LOAD_None);
        if (!Package)
        {
            OutError = FUtf8String(FString::Printf(TEXT("could not load the cooked package %s"), *PackagePath));
            return false;
        }
        Package->FullyLoad();
        Runtime->AddCompiledUPackage(Package);

        // The generation the cook published, which the sidecar carries rather than this file
        // assuming: the cooker is a fresh process so it is always 1 today (D20), and a cook that
        // ever publishes a second one should not have to change this.
        const FString MountPoint = MountPointOf(PackagePath);
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

    AdoptCookedGeneration(FUtf8String(ScriptPackageName), Generation);

    FUtf8String SidecarError;
    if (!LoadClassSidecar(SidecarPathFor(CookedDir), SidecarError))
    {
        OutError = SidecarError;
        return false;
    }
    return true;
}
