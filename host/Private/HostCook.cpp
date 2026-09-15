// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostCook.h"

#if VH_HOST_KIND == VH_HOST_KIND_COOKER

#include "Algo/StableSort.h"
#include "IO/IoChunkId.h"
#include "IoStoreUtilities.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "PackageStoreOptimizer.h"
#include "Serialization/CompactBinaryWriter.h"
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

/// Takes the one kind of export SavePackage cannot write out of the export set, for the length of
/// one save.
///
/// SavePackage2.cpp:2076 does `check(Class != nullptr)` on every UVerseClass export's VM class, and
/// a UVerseClass that stands for a Verse *module* has none -- a module is not a class. There are
/// exactly two in this program, `/Solaris/_Verse/VNI/VerseNative.Persona` and
/// `/Solaris/_Verse/VNI/VersePredicts.Predicts`, both epic_internal modules nothing on this bridge
/// can name, and `IsVerseModule()` is what tells them from a class whose VClass is genuinely
/// missing -- which would be a defect and is still refused.
///
/// This used to skip the whole package, and that was the wall of 7b 13.8: VerseNative is where
/// `/Verse.org/Concurrency`'s `task` and `awaitable` and the `/Verse.org/Native` attributes live, so
/// leaving it out of the container left every import into it null in three other packages -- the
/// standard library, the mirror and the project's own -- and the first call that landed on one was
/// an access violation inside VFunction::Invoke.
class FScopedModuleExportSuppression
{
public:
    explicit FScopedModuleExportSuppression(UPackage* Package)
    {
        TArray<UObject*> Objects;
        GetObjectsWithPackage(Package, Objects);
        for (UObject* Object : Objects)
        {
            UVerseClass* VerseClass = Cast<UVerseClass>(Object);
            if (VerseClass && VerseClass->IsVerseModule() && !VerseClass->Class.Get())
            {
                Suppress(VerseClass);
                if (UObject* DefaultObject = VerseClass->GetDefaultObject(/*bCreateIfNeeded*/ false))
                {
                    Suppress(DefaultObject);
                }
            }
        }
    }

    ~FScopedModuleExportSuppression()
    {
        for (UObject* Object : Suppressed)
        {
            Object->ClearFlags(RF_Transient);
            Object->SetInternalFlags(EInternalObjectFlags::Native);
        }
    }

private:
    /// RF_Transient alone is not enough: FSaveContext::GetSaveableStatusNoOuter reads it only for a
    /// non-native object (SaveContext.cpp:232-243), and a VNI-generated UVerseClass carries
    /// EInternalObjectFlags::Native -- which is what UObject::IsNative() answers from, not
    /// RF_MarkAsNative. Both go back on in the destructor; the class is live in this process.
    void Suppress(UObject* Object)
    {
        Object->SetFlags(RF_Transient);
        Object->ClearInternalFlags(EInternalObjectFlags::Native);
        Suppressed.Add(Object);
    }

    TArray<UObject*> Suppressed;
};

/// Whether saving this package would take the process down anyway, once the modules above are out
/// of the way. There is no way to catch an appError, so the only thing to do with a package like
/// that is not to hand it over -- and to say which class it was, because unlike a module this is a
/// defect rather than a shape the engine cannot serialise.
bool WouldAssertOnSave(UPackage* Package)
{
    TArray<UObject*> Objects;
    GetObjectsWithPackage(Package, Objects);
    for (UObject* Object : Objects)
    {
        const UVerseClass* VerseClass = Cast<UVerseClass>(Object);
        if (VerseClass && !VerseClass->Class.Get() && !VerseClass->IsVerseModule())
        {
            UE_LOG(LogTemp, Warning, TEXT("verse_cook: %s has no VM class"), *VerseClass->GetPathName());
            return true;
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

    const FScopedModuleExportSuppression ModuleSuppression(Package);

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

AUTORTFM_DISABLE bool GodotVerse::CookProjectPackages(const FString& OutDir, TArray<FCookedPackageFile>& OutWritten, FUtf8String& OutError)
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
    // project's. Every one of them has to reach the container: a VNI package the runtime host
    // cannot find is only a warning from JitVniPackages (SolarisModule.cpp:3415-3430), and then
    // every import into it in every other package silently resolves to null.
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
    // asserts on a UVerseClass whose Verse::VClass is null, which a module's is), so a run that
    // dies part way through has written the half this project cannot do without.
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
        OutWritten.Add(FCookedPackageFile{PackagePath, Filename});
    }

    if (OutWritten.IsEmpty())
    {
        OutError = UTF8TEXT("no package had anything to save");
        return false;
    }
    return true;
}


// --- the container step (phase-7b-design.md 4) -------------------------------------------------

namespace {

/// The script-objects chunk IoStoreUtilities refuses to run without.
///
/// A real cook writes this file out of the same sweep of loaded /Script/ packages; here the sweep
/// is FPackageStoreOptimizer::Initialize() against this process, which is the same set. The cell
/// half of that sweep (ScriptCellsMap, from $BuiltIn) is *not* serialised by
/// CreateScriptObjectsBuffer, so the conversion re-reads this file and finds no script cells --
/// which costs one "referencing missing script import" warning per built-in cell and nothing else:
/// ProcessImports assigns the FPackageObjectIndex from the verse path either way
/// (PackageStoreOptimizer.cpp:470-481), and the runtime resolves it against the registrations
/// FAsyncLoadingThread2::NotifyScriptVersePackage makes in memory.
bool WriteScriptObjects(const FString& Path, FUtf8String& OutError)
{
    FPackageStoreOptimizer Optimizer;
    Optimizer.Initialize();
    const FIoBuffer Buffer = Optimizer.CreateScriptObjectsBuffer();
    const TArrayView<const uint8> Bytes(Buffer.GetData(), static_cast<int32>(Buffer.DataSize()));
    if (!FFileHelper::SaveArrayToFile(Bytes, *Path))
    {
        OutError = FUtf8String(FString::Printf(TEXT("could not write the script objects to %s"), *Path));
        return false;
    }
    return true;
}

/// The oplog manifest, which is the one thing the conversion cannot derive from the files.
///
/// A legacy cooked `.uasset` carries no package name, so FindOrAddLegacyPackage asks the package
/// store for one by filename (IoStoreUtilities.cpp:1742) and drops any file it cannot name. Every
/// other field an oplog entry can hold is unused on this path -- imports, shader maps and chunk
/// hashes are all rebuilt from the cooked header by FPackageStoreOptimizer -- so the entry is the
/// package's name and the chunk id its export-bundle data will be written under.
bool WriteManifest(const FString& Path, const FString& LooseDir,
                   const TArray<GodotVerse::FCookedPackageFile>& Packages, FUtf8String& OutError)
{
    FCbWriter Writer;
    Writer.BeginObject();
    Writer.BeginObject(UTF8TEXT("oplog"));
    Writer.BeginArray(UTF8TEXT("entries"));
    for (const GodotVerse::FCookedPackageFile& Package : Packages)
    {
        const FName PackageName(*Package.PackageName);
        const FPackageId PackageId = FPackageId::FromName(PackageName);
        const FIoChunkId ChunkId = CreateIoChunkId(PackageId.Value(), 0, EIoChunkType::ExportBundleData);

        FString Relative = Package.Filename;
        Relative.RemoveFromStart(LooseDir);
        Relative.RemoveFromStart(TEXT("/"));

        Writer.BeginObject();
        Writer.BeginObject(UTF8TEXT("packagestoreentry"));
        Writer.AddString(UTF8TEXT("packagename"), Package.PackageName);
        Writer.EndObject();
        Writer.BeginArray(UTF8TEXT("packagedata"));
        Writer.BeginObject();
        Writer.AddObjectId(UTF8TEXT("id"), FCbObjectId(MakeMemoryView(ChunkId.GetData(), ChunkId.GetSize())));
        Writer.AddString(UTF8TEXT("filename"), Relative);
        Writer.EndObject();
        Writer.EndArray();
        Writer.EndObject();
    }
    Writer.EndArray();
    Writer.EndObject();
    Writer.EndObject();

    TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileWriter(*Path));
    if (!Ar)
    {
        OutError = FUtf8String(FString::Printf(TEXT("could not write the package store manifest to %s"), *Path));
        return false;
    }
    Writer.Save(*Ar);
    return Ar->Close();
}

/// `"<source>" "<destination>" -compress` per line, the shape UnrealPak's response files have.
///
/// Only the `.uasset` is listed: CreateTargetFileFromCookedFile reads a PackageHeader's `.uexp`
/// itself (IoStoreUtilities.cpp:1973-1987), and listing it separately would name the same chunk
/// twice. `-compress` is per file and is what the `-compressionformats=` on the command line
/// applies to; without it the container is larger than the loose cook it came from (§13).
bool WriteResponseFile(const FString& Path, const FString& LooseDir,
                       const TArray<GodotVerse::FCookedPackageFile>& Packages, FUtf8String& OutError)
{
    FString Text;
    for (const GodotVerse::FCookedPackageFile& Package : Packages)
    {
        FString Relative = Package.Filename;
        Relative.RemoveFromStart(LooseDir);
        Relative.RemoveFromStart(TEXT("/"));
        Text += FString::Printf(TEXT("\"%s\" \"../../../%s\" -compress\n"), *Package.Filename, *Relative);
    }
    if (!FFileHelper::SaveStringToFile(Text, *Path))
    {
        OutError = FUtf8String(FString::Printf(TEXT("could not write the response file to %s"), *Path));
        return false;
    }
    return true;
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::BuildCookedContainer(const FString& LooseDir, const FString& OutDir,
    const TArray<FCookedPackageFile>& Packages, FUtf8String& OutError)
{
    FString NormalizedLooseDir = FPaths::ConvertRelativePathToFull(LooseDir);
    FPaths::NormalizeDirectoryName(NormalizedLooseDir);

    const FString WorkDir = FPaths::Combine(NormalizedLooseDir, TEXT("_container_work"));
    IFileManager::Get().MakeDirectory(*OutDir, /*Tree*/ true);
    IFileManager::Get().MakeDirectory(*WorkDir, /*Tree*/ true);

    const FString ScriptObjectsPath = FPaths::Combine(WorkDir, TEXT("scriptobjects.bin"));
    const FString ManifestPath = FPaths::Combine(WorkDir, TEXT("packagestore.manifest"));
    const FString ResponsePath = FPaths::Combine(WorkDir, TEXT("response.txt"));

    TArray<FCookedPackageFile> Normalized;
    Normalized.Reserve(Packages.Num());
    for (const FCookedPackageFile& Package : Packages)
    {
        FString Filename = FPaths::ConvertRelativePathToFull(Package.Filename);
        FPaths::NormalizeFilename(Filename);
        Normalized.Add(FCookedPackageFile{Package.PackageName, MoveTemp(Filename)});
    }

    if (!WriteScriptObjects(ScriptObjectsPath, OutError)
        || !WriteManifest(ManifestPath, NormalizedLooseDir, Normalized, OutError)
        || !WriteResponseFile(ResponsePath, NormalizedLooseDir, Normalized, OutError))
    {
        return false;
    }

    const FString CommandsPath = FPaths::Combine(WorkDir, TEXT("commands.txt"));
    const FString Command = FString::Printf(
        TEXT("-Output=\"%s\" -ContainerName=verse_scripts -ResponseFile=\"%s\""),
        *FPaths::Combine(OutDir, TEXT("verse_scripts")), *ResponsePath);
    if (!FFileHelper::SaveStringToFile(Command + TEXT("\n"), *CommandsPath))
    {
        OutError = FUtf8String(FString::Printf(TEXT("could not write the container command list to %s"), *CommandsPath));
        return false;
    }

    // CreateIoStoreContainerFiles takes a command line and then reads FCommandLine::Get() for all
    // but the first two switches (IoStoreUtilities.cpp:10338-10360), so the process's own line is
    // what it actually parses. Appended rather than replaced: the engine's boot switches are still
    // live underneath, and this runs once, at the end, with nothing after it but the exit.
    const FString OriginalCommandLine = FCommandLine::Get();
    const FString Arguments = FString::Printf(
        TEXT("%s -CreateGlobalContainer=\"%s\" -CookedDirectory=\"%s\" -PackageStoreManifest=\"%s\"")
        TEXT(" -ScriptObjects=\"%s\" -Commands=\"%s\" -compressionformats=Oodle"),
        *OriginalCommandLine,
        *FPaths::Combine(WorkDir, TEXT("global")),
        *NormalizedLooseDir,
        *ManifestPath,
        *ScriptObjectsPath,
        *CommandsPath);
    FCommandLine::Set(*Arguments);
    const int32 Result = CreateIoStoreContainerFiles(*Arguments);
    FCommandLine::Set(*OriginalCommandLine);

    if (Result != 0)
    {
        OutError = FUtf8String(FString::Printf(
            TEXT("building the IoStore container failed (CreateIoStoreContainerFiles returned %d)"), Result));
        return false;
    }
    return true;
}


#endif // VH_HOST_KIND == VH_HOST_KIND_COOKER
