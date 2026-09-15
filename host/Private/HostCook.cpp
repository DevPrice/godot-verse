// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostCook.h"

#if VH_HOST_KIND == VH_HOST_KIND_COOKER

#include "Algo/StableSort.h"
#include "VerseVM/VVMVerseClass.h"
#include "HAL/FileManager.h"
#include "HostScript.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/ArchiveCookData.h"
#include "HostCookWriter.h"
#include "Serialization/BasePackageWriter.h"
#include "UObject/ArchiveCookContext.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "VerseVM/VVMGlobalProgram.h"
#include "VerseVM/VVMPackage.h"
#include "VerseVM/VVMProgram.h"

namespace {

/// Where a UPackage's path lands on disk, under the data directory an exported game carries.
///
/// The mount point is the first segment, and `Engine` is the one that is not ours: the runtime
/// host's engine directory is the data directory (D7), so FPaths::EngineContentDir() resolves to
/// <data>/Engine/Content and JitVniPackages finds the mirror there with nothing registered
/// (SolarisModule.cpp:3372-3404). Every other mount point is a Verse package name, and the runtime
/// host registers one mount point per subdirectory of Cooked/.
///
/// Empty for a path this does not know how to place.
FString FileForUPackagePath(const FString& OutDir, const FString& PackagePath)
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
    if (MountPoint == TEXT("Engine"))
    {
        return FPaths::Combine(OutDir, TEXT("Engine"), TEXT("Content"), Rest + TEXT(".uasset"));
    }
    return FPaths::Combine(OutDir, TEXT("Cooked"), MountPoint, Rest + TEXT(".uasset"));
}

/// What a cook expects to be talking to, for the two things SavePackage asks of it.
///
/// UnrealEd's CookFunctionLibrary.cpp:152-183 is the worked example of a one-off cook and declares
/// this same class inline; the pieces that matter are that BeginCache says yes -- "saving will
/// fail if we don't say good things happened", in its own words -- and that WriteFileOnCookDirector
/// is what actually puts bytes on disk, through Core's own HashAndWrite.
class FVerseCookerInterface final : public UE::PackageWriter::Private::ICookerInterface
{
public:
    virtual EPackageWriterResult CookerBeginCacheForCookedPlatformData(
        UE::PackageWriter::Private::FBeginCacheForCookedPlatformDataInfo&) override
    {
        return EPackageWriterResult::Success;
    }
    virtual void RegisterDeterminismHelper(ICookedPackageWriter*, UObject*,
        const TRefCountPtr<UE::Cook::IDeterminismHelper>&) override
    {
    }
    virtual bool IsDeterminismDebug() const override { return false; }
    virtual void WriteFileOnCookDirector(const UE::PackageWriter::Private::FWriteFileData& FileData,
        FMD5& AccumulatedHash, const TRefCountPtr<FPackageHashes>& PackageHashes,
        IPackageWriter::EWriteOptions WriteOptions) override
    {
        UE::PackageWriter::Private::HashAndWrite(FileData, AccumulatedHash, PackageHashes, WriteOptions);
    }
    virtual UE::Cook::ICookSandbox* GetCookSandbox() override { return nullptr; }
    virtual bool InternalTryReadClassesOfExportsInUnloadedPackage(FName, TMap<FString, FString>&) const override
    {
        return false;
    }
};

/// Whether saving this package would take the process down.
///
/// SavePackage2.cpp:2076 does `check(Class != nullptr)` on every UVerseClass export's VM class, and
/// Epic's own native VNI packages -- /Solaris/_Verse/VNI/VerseNative and its siblings -- have
/// UVerseClass objects whose Verse::VClass is null in this process. There is no way to catch an
/// appError, so the only thing to do with a package like that is not to hand it over.
///
/// The three packages this cook actually has to produce all pass: the project's own, the attribute
/// package, and the mirror at /Engine/_Verse/VNI/VerseHost. What is skipped is the standard
/// library, whose native bindings are compiled into the runtime host anyway; a VNI package the
/// runtime host cannot find is a warning from JitVniPackages rather than a failure
/// (SolarisModule.cpp:3415-3430), which is what makes finding out affordable.
bool WouldAssertOnSave(UPackage* Package)
{
    TArray<UObject*> Objects;
    GetObjectsWithPackage(Package, Objects);
    for (UObject* Object : Objects)
    {
        if (const UVerseClass* VerseClass = Cast<UVerseClass>(Object))
        {
            if (!VerseClass->Class.Get())
            {
                return true;
            }
        }
    }
    return false;
}

/// Saves one UPackage as a cooked asset at Filename.
///
/// The shape is CookFunctionLibrary::CookPackage's, which is the engine's own one-off cook: the
/// caller brackets the save with BeginPackage and CommitPackage, because in a real cook that is
/// the cook server's job and SavePackage does neither. Without the bracket the writer asserts
/// "BeginPackage must be called before any other functions on IPackageWriter" -- which is where
/// the third cook stopped.
bool SaveCookedPackage(UPackage* Package, const FString& Filename, FUtf8String& OutError)
{
    ITargetPlatformManagerModule& TargetPlatformManager = GetTargetPlatformManagerRef();
    const ITargetPlatform* TargetPlatform = TargetPlatformManager.FindTargetPlatform(TEXT("Windows"));
    if (!TargetPlatform)
    {
        OutError = UTF8TEXT("no Windows target platform is available; the cooker was built without it");
        return false;
    }

    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), /*Tree*/ true);

    FVerseCookerInterface CookerInterface;
    // Naked new, and not owned here: ~FSavePackageContext deletes the writer it was given.
    // CookFunctionLibrary.cpp:201 says so in a comment, and a stack-allocated one segfaulted the
    // process the instant the first package finished -- after it had written both its files.
    FVerseCookedPackageWriter* Writer = new FVerseCookedPackageWriter(Filename);
    Writer->SetCooker(&CookerInterface);

    FSavePackageContext Context(TargetPlatform, Writer);
    FArchiveCookContext CookContext(Package, UE::Cook::ECookType::ByTheBook, UE::Cook::ECookingDLC::No,
                                    TargetPlatform, /*CookInfo*/ nullptr);
    FArchiveCookData CookData(*TargetPlatform, CookContext);

    // The target platform decides whether editor-only data is filtered, and a cooked package is
    // one that has none. Restored afterwards because the UPackage outlives this call.
    const bool bWasFilterEditorOnly = Package->HasAllPackagesFlags(PKG_FilterEditorOnly);
    if (!TargetPlatform->HasEditorOnlyData())
    {
        Package->SetPackageFlags(PKG_FilterEditorOnly);
    }

    FSavePackageArgs Args;
    Args.ArchiveCookData = &CookData;
    Args.TopLevelFlags = RF_Public;
    Args.SaveFlags = SAVE_AllowTimeout;
    Args.bForceByteSwapping = (!TargetPlatform->IsLittleEndian()) ^ (!PLATFORM_LITTLE_ENDIAN);
    Args.bWarnOfLongFilename = false;
    Args.bSlowTask = false;
    Args.SavePackageContext = &Context;

    ICookedPackageWriter::FBeginPackageInfo BeginInfo;
    BeginInfo.PackageName = Package->GetFName();
    BeginInfo.LooseFilePath = Filename;
    Writer->BeginPackage(BeginInfo);

    const FSavePackageResultStruct Result = UPackage::Save(Package, /*Asset*/ nullptr, *Filename, Args);

    if (Result.IsSuccessful())
    {
        ICookedPackageWriter::FCommitPackageInfo CommitInfo;
        CommitInfo.Status = IPackageWriter::ECommitStatus::Success;
        CommitInfo.PackageName = Package->GetFName();
        CommitInfo.WriteOptions = IPackageWriter::EWriteOptions::Write;
        Writer->CommitPackage(MoveTemp(CommitInfo));
    }

    if (!bWasFilterEditorOnly)
    {
        Package->ClearPackageFlags(PKG_FilterEditorOnly);
    }

    if (!Result.IsSuccessful())
    {
        OutError = FUtf8String(FString::Printf(TEXT("saving %s returned %d"), *Package->GetName(), (int32)Result.Result));
        return false;
    }
    return true;
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::CookProjectPackages(const FString& OutDir, TArray<FString>& OutWritten, FUtf8String& OutError)
{
    if (!Verse::GlobalProgram)
    {
        OutError = UTF8TEXT("there is no Verse program to cook; nothing was compiled");
        return false;
    }

    // Every package the program holds, rather than a list of the ones we expect. A compiler-less
    // Solaris loads Verse out of cooked UPackages and out of nothing else -- the standard library
    // and the mirror as much as the project's own code -- so "what this process has" is the only
    // list that cannot be short by one.
    //
    // Ordered with the project's own packages first, and the VNI packages after: a VNI package is
    // the risky half of this (their UVerseClass objects are the ones SavePackage2.cpp:2076 asserts
    // on), and a run that dies part way through has at least written the half that is this
    // project's.
    TArray<UPackage*> ToSave;
    const uint32 Count = Verse::GlobalProgram->NumPackages();
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        Verse::VPackage& Package = Verse::GlobalProgram->GetPackage(Index);
        UPackage* UPackageForVerse = Package.GetUPackage();
        UE_LOG(LogTemp, Display, TEXT("verse_cook: package -> %s"),
               UPackageForVerse ? *UPackageForVerse->GetName() : TEXT("(no UPackage)"));
        if (UPackageForVerse)
        {
            // A package the VM holds with nothing serialised behind it is skipped, not an error:
            // the runtime host asks for what it needs by name and says so if it is missing.
            ToSave.Add(UPackageForVerse);
        }
    }
    // The project's own packages first, then the mirror, then the rest of the VNI packages -- the
    // standard library and Epic's own. A VNI package is the risky half (SavePackage2.cpp:2076
    // asserts on a UVerseClass whose Verse::VClass is null, which VerseNative's are), so a run
    // that dies part way through has written the half this project cannot do without.
    Algo::StableSortBy(ToSave, [](UPackage* Package)
        {
            const FString Name = Package->GetName();
            if (!Name.Contains(TEXT("/_Verse/VNI/")))
            {
                return 0;
            }
            return Name == TEXT("/Engine/_Verse/VNI/VerseHost") ? 1 : 2;
        });

    for (UPackage* UPackageForVerse : ToSave)
    {
        const FString PackagePath = UPackageForVerse->GetName();
        const FString Filename = FileForUPackagePath(OutDir, PackagePath);
        if (Filename.IsEmpty())
        {
            OutError = FUtf8String(FString::Printf(TEXT("cannot place %s on disk"), *PackagePath));
            return false;
        }

        if (WouldAssertOnSave(UPackageForVerse))
        {
            UE_LOG(LogTemp, Warning, TEXT("verse_cook: skipping %s -- it holds a Verse class with no VM class"), *PackagePath);
            continue;
        }

        UE_LOG(LogTemp, Display, TEXT("verse_cook: saving %s -> %s"), *PackagePath, *Filename);
        if (!SaveCookedPackage(UPackageForVerse, Filename, OutError))
        {
            return false;
        }
        OutWritten.Add(Filename);
    }

    if (OutWritten.IsEmpty())
    {
        OutError = UTF8TEXT("no package had anything to save");
        return false;
    }
    return true;
}

#endif // VH_HOST_KIND == VH_HOST_KIND_COOKER
