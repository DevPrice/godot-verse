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
#include "IVerseNativeModule.h"
#include "Misc/AES.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/PackageStore.h"
#include "UObject/Package.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UObjectGlobals.h"
#include "VerseVM/VVMGlobalProgram.h"
#include "VerseVM/VVMPackage.h"
#include "VerseVM/VVMPackageName.h"
#include "VerseVM/VVMProgram.h"
#include "VerseVM/VVMVerseModuleClass.h"

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

/// Every module a loaded package holds a definition in, as the asset name the VNI registrations are
/// keyed by.
///
/// A definition's key is its decorated path -- `(/Verse.org/Verse:)Print`, and for the native half
/// of one `(/Verse.org/Verse/(/Verse.org/Verse:)Print:)Native` -- so the module is the leading scope
/// with anything from its first `(` onwards cut off. Relative to the package's root path, with `/`
/// spelled `_` and the root module spelled `_Root`, that is exactly the `MangledVerseName` the VNI
/// generator writes into each registration's FVniTypeDesc
/// (VerseNativeInterfaceGen/Private/DefinitionInfo.cpp:214-222) -- and the package's own definitions
/// are the only place a host with no semantic program can read the list from. `/Verse.org` is a
/// package whose root module holds nothing: `Print` is in the *submodule* `Verse`, so `_Root` alone
/// finds nothing there.
AUTORTFM_DISABLE TSet<FString> ModuleAssetNamesOf(const Verse::VPackage& VersePackage)
{
    const FString RootPath(FUtf8String(VersePackage.GetRootPath().AsStringView()));
    TSet<FString> Names;
    Names.Add(ANSI_TO_TCHAR(Verse::FPackageName::RootModuleClassName));

    const uint32 DefinitionCount = VersePackage.NumDefinitions();
    for (uint32 Index = 0; Index < DefinitionCount; ++Index)
    {
        const FString Decorated(FUtf8String(VersePackage.GetDefinitionName(Index).AsStringView()));
        int32 ScopeEnd = INDEX_NONE;
        if (!Decorated.EndsWith(TEXT(":)Native"), ESearchCase::CaseSensitive)
            || !Decorated.StartsWith(TEXT("(")) || !Decorated.FindChar(TEXT(':'), ScopeEnd))
        {
            continue;
        }
        FString Scope = Decorated.Mid(1, ScopeEnd - 1);
        int32 NestedScope = INDEX_NONE;
        if (Scope.FindChar(TEXT('('), NestedScope))
        {
            Scope = Scope.Left(NestedScope);
        }
        while (Scope.EndsWith(TEXT("/")))
        {
            Scope.LeftChopInline(1);
        }
        if (!Scope.StartsWith(RootPath))
        {
            continue;
        }
        FString Relative = Scope.Mid(RootPath.Len());
        Relative.RemoveFromStart(TEXT("/"));
        Names.Add(Relative.IsEmpty() ? FString(ANSI_TO_TCHAR(Verse::FPackageName::RootModuleClassName))
                                     : Relative.Replace(TEXT("/"), TEXT("_")));
    }
    return Names;
}

/// Puts the C++ thunks back on the module-level `<native>` functions of every loaded VNI package.
///
/// A VNativeProcedure's Thunk is a C++ function pointer, so it is not serialised: a cooked package
/// comes back with `Thunk = nullptr` on every one of them (VVMNativeProcedure.cpp:37-42), and the
/// interpreter calls it without checking -- `(*NativeProcedure->Thunk)(...)`,
/// VVMInterpreter.cpp:2666 -- which is a jump to address 0 and no diagnostic of any kind.
///
/// The engine rebinds the *class*-scoped half at load, and the module-scoped half only at build
/// time, from the assembler walking the semantic program's module parts
/// (VerseVMCodeGen/Private/VVMAssembler.cpp:296-322). `FVerseNativeModule::TryBindVniModule` carries
/// the TODO that says so in as many words -- "Call at load time when we start using VerseVM cooked
/// framework packages" (VerseNativeModule.cpp:340-341) -- so a host that loads cooked VNI packages
/// has to do the walk itself. That is this: same public entry point, with the module list read off
/// the loaded package instead of off an AST there is no compiler to build.
///
/// Until this ran, every mirrored Godot call and every stdlib call from an exported game was a raw
/// access violation inside VFunction::Invoke (phase-7b-design.md 13.8): `VhCallValue`, `Print` and
/// `Sqrt` are all module-level natives.
AUTORTFM_DISABLE void RebindVniModuleNatives()
{
    if (!Verse::GlobalProgram)
    {
        return;
    }

    const uint32 PackageCount = Verse::GlobalProgram->NumPackages();
    for (uint32 Index = 0; Index < PackageCount; ++Index)
    {
        Verse::VPackage& VersePackage = Verse::GlobalProgram->GetPackage(Index);
        UPackage* UPackageForVerse = VersePackage.GetUPackage();
        if (!UPackageForVerse || !IsVniPackage(UPackageForVerse->GetName()))
        {
            continue;
        }
        for (const FString& ModuleName : ModuleAssetNamesOf(VersePackage))
        {
            // A parametric class writes its scope plainly, with no `(...)` decoration, so cutting at
            // the first one leaves the *class* rather than the module it is in -- and asking
            // TryBindVniModule for a type's key trips its own ensure that the scope name is empty
            // (VerseNativeModule.cpp:349). The UObject beside the name is what tells them apart:
            // a module's is a UVerseModuleClass, a class's a UVerseClass. A module with no UObject
            // at all is still a module and is still asked.
            const UObject* Beside = StaticFindObject(UStruct::StaticClass(), UPackageForVerse, *ModuleName);
            if (Beside && !Beside->IsA<UVerseModuleClass>())
            {
                continue;
            }
            IVerseNativeModule::Get().TryBindVniModule(
                VersePackage, FTopLevelAssetPath(UPackageForVerse->GetFName(), FName(*ModuleName)));
        }
    }
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

    RebindVniModuleNatives();

    FUtf8String SidecarError;
    if (!LoadClassSidecar(SidecarPathFor(CookedDir), SidecarError))
    {
        OutError = SidecarError;
        return false;
    }
    return true;
}
