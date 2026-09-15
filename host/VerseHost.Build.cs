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
			"Solaris",
			"uLangCore",
			"VerseVMSocketDebugger",
			"VerseNative",
			"Verse",
			"VerseSpatialMath",

			// The class sidecar an exported game ships is JSON (HostSidecar.cpp): the cooker
			// writes it and the runtime host reads it back, so both need this.
			"Json",

			// The cooked project is an IoStore container and HostCooked.cpp mounts it, which is
			// the iostore half of FPakPlatformFile::Mount with the pak half taken out: the two
			// backends that mount live in PakFile's Private folder and nowhere else.
			"PakFile",
		});

		// FFilePackageStoreBackend and the file I/O dispatcher backends are PakFile's own, and
		// UE::FPlatformIoDispatcher -- which decides which of the two to make -- is in Core's
		// Internal folder. Neither is on a Program's include path however the dependency is
		// declared, and a monolithic link is what makes reaching them legal at all.
		PrivateIncludePaths.AddRange(new string[]{
			System.IO.Path.Combine(EngineDirectory, "Source", "Runtime", "PakFile", "Private"),
			System.IO.Path.Combine(EngineDirectory, "Source", "Runtime", "PakFile", "Internal"),
			System.IO.Path.Combine(EngineDirectory, "Source", "Runtime", "Core", "Internal"),
		});

		// The compiler, and everything that only exists to serve it. A runtime host gets
		// WITH_VERSE_COMPILER=0 from bBuildWithEditorOnlyData (CoreUObject.Build.cs:46-53) and runs
		// Verse out of cooked packages, so it asks for none of these.
		//
		// **It gets them anyway, and this does not make the DLL one byte smaller.** Solaris lists
		// VerseCompiler and VerseVMCodeGen in its *public* dependencies unconditionally
		// (Solaris.Build.cs:14-29) and uLangUE in its private ones (:30-40), and Solaris is the
		// module that runs Verse -- there is no version of this host that links one and not the
		// other. VerseNative pulls uLangUE and VerseVMCodeGen too, VerseSpatialMath pulls
		// VerseSimulationMetadata, and SolarisBridge pulls VerseCompiler. Measured with
		// `Build.bat VerseHostRuntime Win64 Development -Mode=JsonExport`: all five are still in
		// the graph with this block in place, and the binary stayed at 112.4 MB.
		//
		// The block stays because it is what this host actually depends on, and because the next
		// person to ask "why does a game ship a Verse compiler" should find the answer here rather
		// than repeat the experiment. The one thing that *does* drop is ScriptDisassembler, in
		// Shipping only (Solaris.Build.cs:113-115) -- which is the configuration D12 ships.
		if (Target.bBuildWithEditorOnlyData)
		{
			PrivateDependencyModuleNames.AddRange(new string[]{
				"ScriptDisassembler",
				"uLangUE",
				"VerseCompiler",
				"VerseVMCodeGen",

				// Not for the inspector attributes -- those are godot-verse's own, declared at
				// runtime in a package the authorship injection lets us author. This is for the
				// scripts that import /Verse.org/Simulation anyway: its metadata attributes are
				// `@customattribhandler`, and a *build* where one is applied with no handler
				// registered fails outright rather than ignoring the attribute. A cooked package
				// has no attributes left to handle.
				"VerseSimulationMetadata",
			});
		}

		// The cooker (VerseHostCooker.Target.cs) compiles against the editor and Engine, and
		// RequiredProgramMainCPPInclude.h textually compiles LaunchEngineLoop.cpp into
		// VerseHost.cpp -- which under WITH_EDITOR && WITH_ENGINE includes UnrealEd's headers. An
		// editor Launch gets these from Launch.Build.cs's own bBuildEditor block; a Program has to
		// list them itself, which is what ChaosVisualDebugger.Build.cs does. The editor host and
		// the runtime host never enter this branch.
		if (Target.bCompileAgainstEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[]{
				"Engine",
				"InputCore",
				"InstallBundleManager",
				"MediaUtils",
				"Messaging",
				"MoviePlayer",
				"MoviePlayerProxy",
				"PreLoadScreen",
				"RenderCore",
				"RHI",
				"Slate",
				"SlateCore",
				"TraceLog",
				"ProfileVisualizer",
				"SourceControl",
				"EditorFramework",
				"UnrealEd",
				"DeveloperToolSettings",
				"DesktopPlatform",
				"DerivedDataCache",

				// The cook itself: FDefaultCookedFilePackageWriter is UnrealEd's, and
				// UPackage::Save needs a Windows ITargetPlatform to cook against.
				"TargetPlatform",
				"WindowsTargetPlatform",

				// The container step (phase-7b-design.md D1): CreateIoStoreContainerFiles is the
				// only code in the engine that carries a Verse::VCell from a legacy cooked header
				// into something a loader can read.
				"IoStoreUtilities",
			});

			// TPackageWriterToSharedBuffer, which HostCookWriter.h derives the cooker's package
			// writer from, is in CoreUObject's Internal folder -- and an Internal folder is not on
			// a Program's include path however the dependency is declared.
			PrivateIncludePaths.Add(System.IO.Path.Combine(EngineDirectory, "Source", "Runtime", "CoreUObject", "Internal"));

			// FPackageStoreOptimizer, whose CreateScriptObjectsBuffer is what feeds the container
			// step its -ScriptObjects file, is in IoStoreUtilities' Internal folder for the same
			// reason and needs the same line.
			PrivateIncludePaths.Add(System.IO.Path.Combine(EngineDirectory, "Source", "Developer", "IoStoreUtilities", "Internal"));
			PrivateIncludePathModuleNames.AddRange(new string[]{
				"AutomationWorker",
				"AutomationController",
				"AutomationTest",
				"HeadMountedDisplay",
				"MRMesh",
				"SlateRHIRenderer",
				"SlateNullRenderer",
				"MessagingCommon",
			});
		}
	}
}
