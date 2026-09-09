// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class VerseHostTarget : TargetRules
{
	public VerseHostTarget(TargetInfo Target) : base(Target)
	{
		Name = "verse_host";
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		Type = TargetType.Program;
		LinkType = TargetLinkType.Monolithic;

		SolutionDirectory = "Verse";
		LaunchModuleName = "VerseHost";

		// Godot loads us; we do not own main() or process lifetime.
		bShouldCompileAsDLL = true;
		bHasExports = true;

		// Lean and mean, matching VerseCmd - this module set is the known-good minimum for VerseVM.
		bBuildDeveloperTools = false;
		bCompileICU = false;
		bBuildWithEditorOnlyData = true;
		bCompileAgainstEngine = false;
		bCompileAgainstCoreUObject = true;
		bIsBuildingConsoleApplication = true;

		GlobalDefinitions.Add("ALLOW_LOG_FILE=0");
		GlobalDefinitions.Add("NOINITCRASHREPORTER=1");

		bUseLoggingInShipping = true;
		bUseAutoRTFMCompiler = true;
		bUseVerseBPVM = false;
	}
}
