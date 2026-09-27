// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/UnrealString.h"
#include "Containers/Utf8String.h"
#include "uLang/Toolchain/ProgramBuildManager.h"

/// The build and generation lifecycle: the IDE and its caching parser, the three packages a build
/// reads (the attributes, the bindings, the generation's own), the analysis thread, and what the
/// last publish left. The public half -- CompileProject, CheckProject, the background check,
/// SetBindings, ResetScriptState -- is declared in HostScript.h; this is what the units reading that
/// state need, as functions, so none of them can write it.
namespace GodotVerse {

/// Whether the IDE exists, which is whether this host has a compiler at all: a runtime host never
/// makes one and answers everything from a cooked sidecar instead.
AUTORTFM_DISABLE bool HasIde();

/// The IDE's build manager, or null without an IDE. Whichever thread is analysing owns the program
/// it holds, so a caller on the game thread asks IsBackgroundCheckRunning first.
AUTORTFM_DISABLE uLang::TSPtr<uLang::CProgramBuildManager> IdeBuildManager();

/// The package the newest published generation lives in. Empty before the first build, which is
/// the state an editor is in until its first Play.
AUTORTFM_DISABLE const FUtf8String& PublishedScriptPackageName();

/// Whether the VM carries that package. FVerseFunction's package constructor dereferences the
/// result of LookupPackage without checking it, so asking for a function when the build failed
/// crashes rather than returning invalid.
AUTORTFM_DISABLE bool ScriptPackageLoaded();

} // namespace GodotVerse
