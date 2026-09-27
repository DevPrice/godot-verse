// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

// The white-box seam (docs/architecture-review.md item 3 step 4): the editor host's sources as an
// executable, with tests/host_unit's main staged beside them by build_host.py, so the type model,
// the converters and the sidecar can be asked directly about types the host compiled.
//
// Nothing else changes from the editor host, which is the point: VH_HOST_KIND stays the editor's,
// so the code under test is the code verse_host.dll runs. An executable rather than a DLL only
// because a test needs a main -- none of the cooker's editor-class flags apply, since this target
// never cooks (phase-7-design.md 2 S-1 is what those cost).
public class VerseHostUnitTarget : VerseHostTarget
{
	public VerseHostUnitTarget(TargetInfo Target) : base(Target)
	{
		Name = "verse_host_unit";

		bShouldCompileAsDLL = false;
		bHasExports = false;

		GlobalDefinitions.Add("VH_HOST_UNIT=1");
	}
}
