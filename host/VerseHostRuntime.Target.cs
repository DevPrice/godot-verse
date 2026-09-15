// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

// The runtime host: the same sources with no Verse compiler, which is what ships beside an
// exported game. bBuildWithEditorOnlyData=false is the whole of it -- Solaris.Build.cs:47-82
// reads that flag and sets WITH_VERSE_COMPILER=0, which drops VerseCompiler, uLangUE's IDE half
// and everything that reads a .verse file, leaving a Solaris that loads Verse out of cooked
// UPackages and nothing else (SolarisModule.cpp:3372-3404).
//
// Nothing else changes: same module list, same ABI, same DLL shape. The eleven entry points that
// need a compiler answer VH_ERR_UNSUPPORTED (phase-7-design.md 6), and vh_host_kind() says which
// host this is before a consumer calls vh_init.
public class VerseHostRuntimeTarget : VerseHostTarget
{
	public VerseHostRuntimeTarget(TargetInfo Target) : base(Target)
	{
		Name = "verse_host_runtime";

		bBuildWithEditorOnlyData = false;

		GlobalDefinitions.Remove(EditorHostKindDefinition);
		GlobalDefinitions.Add("VH_HOST_KIND=2");
	}
}
