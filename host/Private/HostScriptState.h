// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/StringView.h"
#include "Containers/UnrealString.h"
#include "Containers/Utf8String.h"

class UClass;

namespace uLang {
class CClass;
class CSemanticProgram;
}

/// What HostScript.cpp owns and the units split out of it read: the IDE's semantic program, the
/// project's source snippets, the trace switch and the published generation's classes. Each is a
/// function rather than an extern global, so a unit cannot write what it only has reason to read.
namespace GodotVerse {

/// The semantic program the IDE currently holds, or null before the IDE exists or once it is
/// released. Whichever thread built it owns it while an analysis runs, so a caller on the game
/// thread asks IsBackgroundCheckRunning first.
AUTORTFM_DISABLE uLang::CSemanticProgram* CurrentSemanticProgram();

/// The text the last build or analysis read for the project file at Path, the editor's unsaved
/// buffer included. False for a path the project does not have.
AUTORTFM_DISABLE bool ScriptSnippetText(FUtf8StringView Path, FUtf8String& OutText);

/// One of the script package's own classes in the current program, by the module-qualified name
/// every ClassNameUtf8 in the ABI carries, or null.
AUTORTFM_DISABLE const uLang::CClass* FindScriptClassLive(FUtf8StringView ClassName);

/// Whether VH_TRACE_ANALYSIS asked for a trace to stderr.
AUTORTFM_DISABLE bool AnalysisTraceEnabled();

/// The UClass of a mirrored Godot class -- `node2d` -- in the published program, or null.
AUTORTFM_DISABLE UClass* FindMirroredClass(FUtf8StringView ClassName);

/// The project's own class of this name, in the generation currently published.
AUTORTFM_DISABLE UClass* FindGodotClass(FUtf8StringView ClassName);

/// The binding class of this name. Never module-qualified: the bindings package is generated as one
/// snippet with no modules in it.
AUTORTFM_DISABLE UClass* FindBindingClass(FUtf8StringView ClassName);

} // namespace GodotVerse
