// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "VerseString.h"
#include "verse_host_abi.h"

namespace GodotVerse {

struct FScript
{
    FUtf8String Path;
    /// Verse path of the module this file's definitions live in. Every snippet in a project
    /// shares one flat scope and Verse forbids shadowing, so two files that both define a
    /// top-level Ready() are a compile error; each file wraps its definitions in a module named
    /// after its own stem instead.
    FUtf8String ModulePath;
};

/// Creates the placeholder outer and enters the content scope Verse allocations need.
AUTORTFM_DISABLE bool EnterContentScope();
AUTORTFM_DISABLE void LeaveContentScope();

AUTORTFM_DISABLE FScript* CompileFile(const FUtf8String& Path);

/// Adds every path as a data source and builds them as one program. Callable once per process:
/// a second BuildAll re-notifies already-loaded native Verse packages and aborts in the async
/// loader, so a rebuild is refused with a diagnostic rather than attempted.
AUTORTFM_DISABLE bool CompileProject(const TArray<FUtf8String>& Paths);
/// Re-runs semantic analysis over the whole project with one file's text replaced by the
/// editor's unsaved buffer, reporting fresh diagnostics through the init callback. Generates
/// nothing, so the running program is untouched and this may be called as often as the editor
/// asks -- it is the second BuildAll that CompileProject cannot do, minus the code generation
/// that is what actually cannot happen twice.
AUTORTFM_DISABLE bool CheckProject(const FUtf8String& Path, const FUtf8String& SourceText);

AUTORTFM_DISABLE FScript* OpenScript(const FUtf8String& Path);
AUTORTFM_DISABLE void ReleaseScript(FScript* Script);

/// One live Verse object: a script's `class(godot_node2d)` bound to one Godot instance id.
struct FInstance;

/// Instantiates the class ClassName defines at the top level of the compiled project and binds
/// it to a Godot object. ClassName is undecorated -- `player`, not `(/user@localhost:)player`.
///
/// The class must derive from `godot_object`, which is what gives the instance the UObject
/// representation everything below needs; a class that does not will fail to instantiate here
/// rather than at the first call.
/// Whether the compiled project defines such a class. Cheap enough to ask per script, and it is
/// how a class-shaped script is told from a module-shaped one.
AUTORTFM_DISABLE bool HasClass(FUtf8StringView ClassName);

AUTORTFM_DISABLE FInstance* Instantiate(FUtf8StringView ClassName, int64 Handle);
AUTORTFM_DISABLE void ReleaseInstance(FInstance* Instance);

AUTORTFM_DISABLE bool InstanceHasFunction(const FInstance* Instance, FUtf8StringView DecoratedName);

/// Calling into the instance seals it: see WriteInstanceField.
AUTORTFM_DISABLE int32 InstanceCallVoid(FInstance* Instance, FUtf8StringView DecoratedName);
AUTORTFM_DISABLE int32 InstanceCallVoidFloat(FInstance* Instance, FUtf8StringView DecoratedName, double Arg);

/// One `@godot_export` data member, harvested from the semantic program rather than the VM.
struct FExportDesc
{
    FUtf8String Name;
    vh_type Type;
    bool bIsVar;

    /// Inspector hints, empty when the member carries no such attribute. Strings because that is
    /// what the attributes hold: `@clamp_min("0.0")` takes a string, and SOL-972 means a string
    /// argument is the only attribute payload that can be read back at all.
    FUtf8String ClampMin;
    FUtf8String ClampMax;
    FUtf8String Category;
};

/// Fills OutExports with the `@godot_export` data members ClassName declares, reading the
/// semantic program the last analysis pass left behind.
///
/// This is deliberately not routed through the VM. Analysis re-runs on every keystroke while
/// code generation may happen once per process, so the semantic program is the only view of a
/// script's shape that can change without restarting the editor -- which is what lets Godot
/// refresh a script's exports live. The cost is that it describes source, not running state.
///
/// False means the class is not in the analysed program at all; a class with no exports is true
/// with an empty array.
AUTORTFM_DISABLE bool GetClassExports(FUtf8StringView ClassName, TArray<FExportDesc>& OutExports);

/// Reads one data member off a live instance into the ABI's value shape.
///
/// String bytes are copied into OutStorage rather than pointed at: VArray::AsStringView points
/// into a GC-managed cell, and the vh_value outlives the VM scope it was read in.
AUTORTFM_DISABLE bool ReadInstanceField(const FInstance* Instance, FUtf8StringView FieldName, vh_value& OutValue, FUtf8String& OutStorage);

/// The same read against the class default object, whose Verse constructor has already run.
/// That is where a member's declared default has to come from: the semantic program can say only
/// that an initializer exists (CDataDefinition::HasInitializer), not what it evaluates to.
AUTORTFM_DISABLE bool ReadClassDefaultField(FUtf8StringView ClassName, FUtf8StringView FieldName, vh_value& OutValue, FUtf8String& OutStorage);

/// Writes one data member on a live instance. Covers the same four types the read path does.
///
/// A `var` may be written at any time. A non-var may be written only while the instance is
/// *unsealed* -- between Instantiate and the first call into it -- which is the window Godot uses
/// to apply the values a scene stored. That keeps Verse's immutability honest: a non-var still
/// never changes once the script can observe it, so the inspector value reads as an initializer
/// rather than a mutation. Sealing is one-way and happens on the first InstanceCall*.
AUTORTFM_DISABLE bool WriteInstanceField(FInstance* Instance, FUtf8StringView FieldName, const vh_value& Value);

AUTORTFM_DISABLE bool HasFunction(const FScript* Script, FUtf8StringView DecoratedName);
AUTORTFM_DISABLE int32 RunMain(const TArray<verse::string>& Args, int64& OutExitCode);
AUTORTFM_DISABLE int32 CallVoid(const FScript* Script, FUtf8StringView DecoratedName);
AUTORTFM_DISABLE int32 CallVoidFloat(const FScript* Script, FUtf8StringView DecoratedName, double Arg);

AUTORTFM_DISABLE void TickScripts(double BudgetSeconds);

/// Releases the IDE, its data sources and the content scope. Must run before the engine tears
/// down: these objects free through GMalloc, which AppExit takes with it.
AUTORTFM_DISABLE void ResetScriptState();

} // namespace GodotVerse
