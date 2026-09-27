// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/StringView.h"
#include "Containers/UnrealString.h"
#include "Containers/Utf8String.h"
#include "HostTypeModel.h"

class UClass;
class UObject;

namespace Verse {
struct VFunction;
}

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

/// What a script class declares one of its members as, up the script chain: out of the current
/// program in a host that has one, and out of the cook's recorded tables in a runtime host.
AUTORTFM_DISABLE FMemberType DescribeMemberType(FUtf8StringView ClassName, FUtf8StringView FieldName);

/// The payload shapes the cook recorded for a script class's signal members, by member name, or
/// null for a class with no recorded tables. What a runtime host has instead of a program.
AUTORTFM_DISABLE const TMap<FUtf8String, FPayloadShape>* RecordedSignalShapes(FUtf8StringView ClassName);

/// The UObject a class-typed member holds, or null.
///
/// The read half of WriteFieldOf, narrowed to the one case signal binding needs: a `signal`
/// member's own object, so the host can write the id into it.
AUTORTFM_DISABLE UObject* PeekFieldObject(UObject* Object, FUtf8StringView FieldName);

/// The decorated name of a bound Verse method, found by asking the object for each method its
/// class declares and comparing the function that comes back.
///
/// There is no reading the semantic program's spelling back off a VFunction -- the bytecode has
/// erased it -- so the comparison is the lookup, which is the same thing InstanceHasFunction does
/// to tell an override from an inherited body. Once per Subscribe, never per emission.
AUTORTFM_DISABLE bool DescribeBoundFunction(Verse::VFunction* Function, int64& OutHandle, FUtf8String& OutDecorated);

} // namespace GodotVerse
