// Copyright Epic Games, Inc. All Rights Reserved.

// The cooker's entry point. Each target sets VH_HOST_KIND, and this file is the cooker's alone:
// the two DLL hosts have no main and are entered through the ABI. The S-1 spike (phase-7-design.md
// §2) established that this target builds, links and boots at all -- it has to be an executable,
// because a monolithic editor-class DLL exports every module's API symbols and lld-link stops at
// 65535 of them -- and this is the body §5 asks for.

#include "verse_host_abi.h"

#if VH_HOST_KIND == VH_HOST_KIND_COOKER

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "HostCook.h"
#include "HostScript.h"
#include "HostSidecar.h"
#include "LaunchEngineLoop.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include <cstdio>

DEFINE_LOG_CATEGORY_STATIC(LogVerseCook, Log, All);

namespace {

/// The manifest's absolute path -> the res:// path it came from, so a diagnostic reads the way the
/// author's editor showed it rather than naming a directory on the build machine.
TMap<FString, FString> GResPathByAbsolute;

/// Straight to stdout, because that is what the export plugin reads back through OS::execute.
/// FPlatformMisc::LocalPrint is OutputDebugString on Windows and reaches a debugger and nothing
/// else -- the first cook printed not one line anywhere the plugin could see it.
void Say(const FString& Line)
{
    const FTCHARToUTF8 Utf8(*(Line + TEXT("\n")));
    fwrite(Utf8.Get(), 1, Utf8.Length(), stdout);
    fflush(stdout);
}

/// `<res path>:<line>:<col>: <severity>: <message>`, which is what the export plugin relays.
///
/// vh_diagnostic's Line and Column are already 1-based, and 0 when there is no location -- unlike
/// the zero-based rows the class-describing descriptors carry.
void OnDiagnostic(void* /*Ctx*/, const vh_diagnostic* Diagnostic)
{
    if (!Diagnostic)
    {
        return;
    }

    const FString Message(FUtf8StringView(
        reinterpret_cast<const UTF8CHAR*>(Diagnostic->MessageUtf8), Diagnostic->MessageLen));

    FString Where;
    if (Diagnostic->FilePathUtf8 && Diagnostic->FilePathLen > 0)
    {
        const FString Absolute(FUtf8StringView(
            reinterpret_cast<const UTF8CHAR*>(Diagnostic->FilePathUtf8), Diagnostic->FilePathLen));
        const FString* Res = GResPathByAbsolute.Find(Absolute);
        Where = FString::Printf(TEXT("%s:%d:%d: "), Res ? **Res : *Absolute,
                                Diagnostic->Line, Diagnostic->Column);
    }

    const TCHAR* Severity = TEXT("error");
    switch (Diagnostic->Severity)
    {
    case VH_SEVERITY_WARNING:
        Severity = TEXT("warning");
        break;
    case VH_SEVERITY_INFO:
        Severity = TEXT("info");
        break;
    default:
        break;
    }

    Say(FString::Printf(TEXT("%s%s: %s"), *Where, Severity, *Message));
}

void OnRuntimeError(void* /*Ctx*/, const vh_runtime_error* Error)
{
    if (!Error)
    {
        return;
    }
    const FString Message(FUtf8StringView(
        reinterpret_cast<const UTF8CHAR*>(Error->MessageUtf8), Error->MessageLen));
    Say(FString::Printf(TEXT("error: %s"), *Message));
}

/// One source per line: `<absolute path>\t<module path>\t<res:// path>`. The first two are what
/// vh_compile_project takes; the third is the cooker's alone, for the diagnostics above.
bool ReadManifest(const FString& Path, TArray<GodotVerse::FScriptSource>& OutSources)
{
    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *Path))
    {
        Say(FString::Printf(TEXT("error: could not read the manifest at %s"), *Path));
        return false;
    }

    TArray<FString> Lines;
    Text.ParseIntoArrayLines(Lines);
    for (const FString& Line : Lines)
    {
        if (Line.IsEmpty())
        {
            continue;
        }
        TArray<FString> Fields;
        Line.ParseIntoArray(Fields, TEXT("\t"), /*CullEmpty*/ false);
        if (Fields.Num() < 3)
        {
            Say(FString::Printf(TEXT("error: manifest line is not three tab-separated fields: %s"), *Line));
            return false;
        }
        GodotVerse::FScriptSource& Source = OutSources.AddDefaulted_GetRef();
        Source.Path = FUtf8String(Fields[0]);
        Source.ModulePath = FUtf8String(Fields[1]);
        GResPathByAbsolute.Add(Fields[0], Fields[2]);
    }

    if (OutSources.IsEmpty())
    {
        Say(TEXT("error: the manifest names no .verse files"));
        return false;
    }
    return true;
}

/// How this program ends, and the answer to the problem §5 left open for stage 1.
///
/// The S-1 spike's binary segfaulted at the very end of teardown on every run, past everything a
/// cook would have written, which made its exit code meaningless -- and the export plugin's whole
/// failure path reads that code. Rather than chase an engine shutdown this program does not need,
/// it flushes and leaves: GEngineLoop.Exit() is not called at all, and RequestExitWithStatus with
/// Force set goes straight to the platform's exit with the code we chose. That is what a tool
/// whose work is finished ordinarily does, and it is the one of §5's three options that makes the
/// exit code mean something on the first try.
///
/// Everything this program writes is already closed when this is reached: FFileHelper and the
/// package writer each close their handle inside the call that wrote it.
[[noreturn]] void Leave(int32 Code)
{
    if (GLog)
    {
        GLog->Flush();
    }
    FPlatformMisc::RequestExitWithStatus(/*Force*/ true, (uint8)Code);
    // Does not return, but the compiler has no way to know that.
    for (;;)
    {
    }
}

} // namespace

// AUTORTFM_DISABLE on the entry point itself, the way AutoRTFMTests.cpp:188 writes its `main`:
// this target is built by the AutoRTFM clang (bUseAutoRTFMCompiler), and instrumented code may not
// call an uninstrumented function -- so the whole chain from the entry point down has to be
// disabled, exactly as the ABI entry points in VerseHost.cpp are. On Windows the macro is
// `int32 wmain(...)` and the attribute lands on it; a non-Windows cooker expands to a `tchar_main`
// forward declaration plus a `main`, and would need the attribute moved onto the former.
AUTORTFM_DISABLE INT32_MAIN_INT32_ARGC_TCHAR_ARGV()
{
	FTaskTagScope Scope(ETaskTag::EGameThread);

	// The engine's own log goes to stdout only when asked for. Without this the export plugin
	// relays six hundred lines of engine boot into the export dialog; with it, a failed cook can
	// be re-run by hand and made to say why.
	FString BootArgs(TEXT(" EDITOR -unattended -nullrhi -NoShaderCompile -AssetGatherAll=0 -NoPreviewPlatforms"));
	bool bKeepLoose = false;
	for (int32 Index = 1; Index < ArgC; ++Index)
	{
		if (FCString::Stricmp(ArgV[Index], TEXT("--verbose")) == 0)
		{
			BootArgs += TEXT(" -stdout -FullStdOutLogOutput");
		}
		if (FCString::Stricmp(ArgV[Index], TEXT("--keep-loose")) == 0)
		{
			bKeepLoose = true;
		}
	}
	if (!BootArgs.Contains(TEXT("-stdout")))
	{
		BootArgs += TEXT(" -NOCONSOLE");
	}

	// The editor host's own boot line (VerseHost.cpp), plus four things an engine-class boot
	// needs that a DLL host never met. -NoPreviewPlatforms: WITH_EDITOR would otherwise load
	// every platform's config (UnrealAssetStringify). The EDITOR token: under WITH_EDITOR &&
	// WITH_ENGINE, PreInit refuses to run without a project file unless it is running as the
	// editor or as a commandlet (LaunchEngineLoop.cpp:2715-2725); ChaosVisualDebugger appends
	// the same word. -NoShaderCompile: PreInit constructs FShaderCompilingManager either way,
	// and without this it launches ShaderCompileWorker.exe, which nothing here builds.
	// -nullrhi: there is nothing to draw. -stdout -FullStdOutLogOutput because ALLOW_LOG_FILE=0
	// leaves no log to read afterwards, and what this program has to say when it fails is the
	// engine's own message rather than its exit code.
	// This program *is* a cook, and one piece of the engine only behaves that way when told so by
	// this global. `VIntrinsics::Initialize` gives the `$BuiltIn` package an associated UPackage
	// -- /Script/CoreUObject -- only under IsRunningCookCommandlet (VVMIntrinsics.cpp:41-45), and
	// without one the harvester's import branch writes a **null** package for every reference to
	// an intrinsic: `VersePackage->GetUPackage()` is what it names the import by
	// (PackageHarvester.cpp:1065-1082). The cooked package then loads with a null cell where
	// `Abs`, `Floor`, `BitOr` and the rest should be, and calling one is a jump to address 0 --
	// the same shape of fault as an unbound native thunk, and found the same way.
	//
	// Set before PreInit because the VM is initialised inside it and the flag is read there.
	PRIVATE_GIsRunningCookCommandlet = true;

	if (const int32 Result = GEngineLoop.PreInit(ArgC, ArgV, *BootArgs))
	{
		return Result;
	}

	// Parsed after PreInit, which is what sorts the engine's own switches into Switches and leaves
	// this program's two arguments as the tokens.
	TArray<FString> Tokens;
	TArray<FString> Switches;
	FCommandLine::Parse(FCommandLine::Get(), Tokens, Switches);
	if (Tokens.Num() < 2)
	{
		Say(TEXT("usage: verse_cook <manifest> <out_dir> [--keep-loose] [--verbose]"));
		Leave(2);
	}

	const FString ManifestPath = FPaths::ConvertRelativePathToFull(Tokens[0]);
	const FString OutDir = FPaths::ConvertRelativePathToFull(Tokens[1]);

	TArray<GodotVerse::FScriptSource> Sources;
	if (!ReadManifest(ManifestPath, Sources))
	{
		Leave(2);
	}

	vh_init_desc Desc{};
	Desc.StructSize = sizeof(vh_init_desc);
	Desc.AbiVersion = VH_ABI_VERSION;
	Desc.OnDiagnostic = &OnDiagnostic;
	Desc.OnRuntimeError = &OnRuntimeError;
	if (const int32_t Status = GodotVerse::InitCookerAfterEngineBoot(Desc))
	{
		Say(FString::Printf(TEXT("error: the Verse host would not start (status %d)"), Status));
		Leave(2);
	}

	Say(TEXT("verse_cook: compiling"));
	int32 Generation = 0;
	if (!GodotVerse::CompileProject(Sources, Generation))
	{
		Say(TEXT("error: the project did not compile; nothing was cooked"));
		Leave(2);
	}

	// The loose cook is the *input* to the container step and is not what ships (D9): a `.uasset`
	// holding a Verse cell cannot be loaded at all, so shipping both would double a 68 MB payload
	// with a copy nothing can read. The container step's own scratch files go inside it, so
	// deleting it takes them too.
	const FString LooseDir = FPaths::Combine(OutDir, TEXT("_loose"));
	const FString ContainerDir = FPaths::Combine(OutDir, TEXT("Cooked"));

	// What a previous cook left, removed before this one writes: the export plugin reuses one
	// directory under the user's cache across every export of a project, and the plugin ships that
	// directory whole. A 7a cook's loose `Cooked/GodotAttributes/_Verse.uasset` sitting beside this
	// cook's container is not a leftover -- it is the file the runtime host finds *first*, and the
	// exported game dies on the VCell wall this phase exists to get past. Found by hand, on the
	// first export of dodge-the-creeps that should have worked.
	//
	// Bounded to what this program writes rather than a wipe of OutDir: the argument is a path
	// handed in from outside, and "delete the directory you were pointed at" is not something a
	// tool should do on a typo.
	//
	// `sources.txt` is on the list because it is a name this program's *caller* used to write here
	// and no longer does. The plugin writes the manifest as a sibling now (§13.6), but a machine
	// that exported before that fix still has the old one sitting in its cache directory -- and
	// **everything in this directory ships**, so it went on leaking the author's absolute paths
	// into every export for as long as the file existed. Found by hand, in an export made after
	// §13.6 was written and believed closed.
	for (const TCHAR* Owned : {TEXT("Cooked"), TEXT("Engine"), TEXT("_loose")})
	{
		IFileManager::Get().DeleteDirectory(*FPaths::Combine(OutDir, Owned),
		                                    /*RequireExists*/ false, /*Tree*/ true);
	}
	for (const TCHAR* Owned : {TEXT("verse_classes.json"), TEXT("sources.txt")})
	{
		IFileManager::Get().Delete(*FPaths::Combine(OutDir, Owned), /*RequireExists*/ false);
	}

	Say(FString::Printf(TEXT("verse_cook: compiled generation %d; cooking"), Generation));
	FUtf8String CookError;
	TArray<GodotVerse::FCookedPackageFile> Written;
	if (!GodotVerse::CookProjectPackages(LooseDir, Written, CookError))
	{
		Say(FString::Printf(TEXT("error: %s"), *FString(CookError)));
		Leave(2);
	}

	Say(FString::Printf(TEXT("verse_cook: cooked %d package(s); building the container"), Written.Num()));
	const double ContainerStart = FPlatformTime::Seconds();
	if (!GodotVerse::BuildCookedContainer(LooseDir, ContainerDir, Written, CookError))
	{
		Say(FString::Printf(TEXT("error: %s"), *FString(CookError)));
		Leave(2);
	}
	Say(FString::Printf(TEXT("verse_cook: container built in %.2f s"), FPlatformTime::Seconds() - ContainerStart));

	if (!bKeepLoose)
	{
		IFileManager::Get().DeleteDirectory(*LooseDir, /*RequireExists*/ false, /*Tree*/ true);
	}

	Say(TEXT("verse_cook: writing the class sidecar"));
	TArray<FString> CookedPackages;
	CookedPackages.Reserve(Written.Num());
	for (const GodotVerse::FCookedPackageFile& Package : Written)
	{
		CookedPackages.Add(Package.PackageName);
	}
	const FString SidecarPath = FPaths::Combine(OutDir, TEXT("verse_classes.json"));
	FUtf8String SidecarError;
	if (!GodotVerse::WriteClassSidecar(SidecarPath, CookedPackages, Generation, SidecarError))
	{
		Say(FString::Printf(TEXT("error: %s"), *FString(SidecarError)));
		Leave(2);
	}

	// The engine directory an exported game boots against is the data directory itself (D7), and
	// UE takes any directory with a Binaries/ child as GForeignEngineDir
	// (GenericPlatformMisc.cpp:1408-1415). Nothing goes in it; it is the marker.
	IFileManager::Get().MakeDirectory(*FPaths::Combine(OutDir, TEXT("Engine"), TEXT("Binaries")), /*Tree*/ true);

	Say(FString::Printf(TEXT("verse_cook: generation %d, %d package(s), %d source(s) -> %s"),
	                    Generation, Written.Num(), Sources.Num(), *OutDir));
	Leave(0);
}

#endif // VH_HOST_KIND == VH_HOST_KIND_COOKER
