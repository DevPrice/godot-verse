// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

// The cooker: the editor host's sources with WITH_EDITOR=1, because FSaveContext refuses a
// cooking save without it (phase-0-spikes.md S-1). Every flag below was paid for by a spike
// build, and phase-7-design.md 2 S-1 lists all thirteen -- a Program target *may* opt into
// editor code (TargetRules.cs's own words, and four engine targets do), but not while staying
// lean, which is what OQ-10 was really asking. ChaosVisualDebugger.Target.cs is the nearest
// precedent; UnrealAssetStringify.Target.cs is the one that stays lean and cannot cook.
public class VerseHostCookerTarget : VerseHostTarget
{
	public VerseHostCookerTarget(TargetInfo Target) : base(Target)
	{
		Name = "verse_cook";

		// An executable, not a DLL: a monolithic editor-class DLL exports every module's API
		// symbols and lld-link refuses at 65535 (spike build 6 had 143570). CookMain.cpp is the
		// entry point.
		bShouldCompileAsDLL = false;
		bHasExports = false;

		// UBT sets this itself for a monolithic *Editor* target (UEBuildWindows.cs:1309-1311) and
		// not for a Program: at the default 4096 the 4.6 GB PDB fails to write (spike build 7).
		// UnrealConsole.Target.cs does the same by hand.
		WindowsPlatform.PdbPageSize = 16384;

		// Spike builds 8 and 8b: with the page size fixed, lld-link writing that PDB was killed
		// for memory twice on a 62 GB machine with 42 GB free. The cooker never needs a debugger
		// attached at export; a debuggable build is a by-hand step (phase-7-design.md §2 S-1),
		// and bUseFastPDBLinking is the first thing to try when one is wanted.
		bDisableDebugInfo = true;

		// The editor host turns developer tools off to stay lean. The cooker cannot: under
		// WITH_ENGINE, PreInit constructs FShaderCompilingManager whatever the command line says,
		// and its constructor loads ShaderPreprocessor unconditionally (ShaderCompiler.cpp:789-790).
		// Developer tools are also what carry the target-platform modules a cook needs.
		bBuildDeveloperTools = true;

		bBuildWithEditorOnlyData = true;
		bCompileAgainstEditor = true;
		// Spike build 4: with Engine merely dragged in (VerseSimulationMetadata -> SolarisEditor ->
		// UnrealEd) and WITH_ENGINE=0, UHT could not resolve UWorld or APlayerController for the
		// three headers that say `Within=`. ChaosVisualDebugger sets all three flags together.
		bCompileAgainstEngine = true;

		// Explicitly 1, as UnrealAssetStringify has it: Landscape.cpp and EditorEngine.cpp call
		// IConsoleVariable::GetPlatformValueVariable and UDeviceProfileManager::
		// GetPreviewDeviceProfileSelectorModule, which only exist under it (spike build 5).
		// AutoRTFMTestsWithEditor gets away with 0 because it links neither module.
		GlobalDefinitions.Add("ALLOW_OTHER_PLATFORM_CONFIG=1");
		GlobalDefinitions.Add("UE_CONFIG_ALLOW_ASYNC_LOADING=0");
	}
}
