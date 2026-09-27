// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Containers/StringView.h"
#include "Containers/UnrealString.h"
#include "Containers/Utf8String.h"
#include "HostTypeModel.h"

class UClass;
class UObject;

namespace uLang {
class CClass;
class CSemanticProgram;
}

/// What HostScript.cpp owns and the units split out of it read: the IDE's semantic program, the
/// project's source snippets, the trace switch, the published generation's classes, the recorded
/// declared types, the content scopes' outer and the binding roster. Each is a function rather than
/// an extern global, so a unit cannot write what it only has reason to read.
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

/// The rooted outer every content scope instantiates into, or null before EnterContentScope.
AUTORTFM_DISABLE const UObject* ContentScopeOuter();

/// The parameter and result types the analysis recorded for one method of a script class: out of
/// the snapshot in an editor host and out of the cook's table in a runtime host. False for a method
/// with no recorded signature.
///
/// What the author declared, which the live program stops being once IR generation has run -- see
/// InstanceCall.
AUTORTFM_DISABLE bool RecordedMethodTypes(FUtf8StringView ClassName,
                                          FUtf8StringView DecoratedName,
                                          TArray<FMemberType>& OutParams,
                                          FMemberType& OutResult);

/// The bindings package's class for a script's global class name, or for a ClassDB class name, or
/// null where no binding stands for it. Held for as long as the roster is.
AUTORTFM_DISABLE const FUtf8String* BindingForScriptClass(FUtf8StringView ScriptClass);
AUTORTFM_DISABLE const FUtf8String* BindingForGodotClass(FUtf8StringView GodotClass);

/// Whether any binding stands for a script class, which is what makes asking Godot for an object's
/// script worth a callback.
AUTORTFM_DISABLE bool HasScriptClassBindings();

/// The name InstantiateClass is handed to mint a binding class's peer, or null. Held for as long
/// as the roster is.
AUTORTFM_DISABLE const FUtf8String* MintNameForBinding(FUtf8StringView VerseClass);

} // namespace GodotVerse
