// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class VerseHost : ModuleRules
{
	public VerseHost(ReadOnlyTargetRules Target) : base(Target)
	{
		// InternalUser, not PublicAPI: `<native>` is epic_internal, and outside Epic's own verse
		// path domains InternalUser is the only scope permitted to reach it.
		SetupVerse("/Godot.org/Godot", VerseScope.InternalUser);

		PublicIncludePathModuleNames.Add("Launch");

		// Selects the dllexport side of the shared C ABI header, which build_host.py stages into Public/.
		PrivateDefinitions.Add("VERSE_HOST_IMPLEMENTATION=1");

		PrivateDependencyModuleNames.AddRange(new string[]{
			"VerseProgramHostServices",

			"ApplicationCore",
			"Core",
			"CoreUObject",
			"Projects",
			"SolarisBridge",
			"SolarisTestUtils",
			"ScriptDisassembler",
			"Solaris",
			"uLangUE",
			"uLangCore",
			"VerseCompiler",
			"VerseVMCodeGen",
			"VerseVMSocketDebugger",
			"VerseNative",
			"Verse",
			"VerseSpatialMath",
		});
	}
}
