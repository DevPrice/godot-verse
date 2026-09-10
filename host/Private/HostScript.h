// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "VerseString.h"
#include "verse_host_abi.h"

namespace GodotVerse {

/// Creates the placeholder outer and enters the content scope Verse allocations need.
AUTORTFM_DISABLE bool EnterContentScope();
AUTORTFM_DISABLE void LeaveContentScope();

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

/// Starts CheckProject on a thread we own. False when one is already in flight -- only one runs
/// at a time, because they share the IDE and the semantic program it rebuilds.
///
/// Diagnostics are buffered rather than forwarded, so the Godot callback still only runs on the
/// game thread, out of PollBackgroundCheck.
AUTORTFM_DISABLE bool BeginBackgroundCheck(const FUtf8String& Path, const FUtf8String& SourceText);

/// Reaps a finished BeginBackgroundCheck and forwards its diagnostics. OutFinished is true only
/// on the call that reaps one; the return value is that analysis' result.
AUTORTFM_DISABLE bool PollBackgroundCheck(bool& OutFinished);

AUTORTFM_DISABLE bool IsBackgroundCheckRunning();

/// Blocks until any in-flight background check finishes, then reaps it.
///
/// Every entry point that runs Verse or reads the semantic program calls this first. VerseVM
/// blocks execution for the length of a build, so touching the VM while one runs trips
/// `ensure(!bBlockAllExecution)` in VVMExecutionContext and then kills the process.
AUTORTFM_DISABLE void WaitForBackgroundCheck();

/// One live Verse object: a script's `class(node2d)` bound to one Godot instance id.
struct FInstance;

/// Whether the compiled project defines a top-level class of that name deriving from `object`,
/// which is what makes a .verse file usable as a script at all. Cheap enough to ask per script.
AUTORTFM_DISABLE bool HasClass(FUtf8StringView ClassName);

/// Instantiates the class ClassName defines at the top level of the compiled project and binds
/// it to a Godot object. ClassName is undecorated -- `player`, not `(/user@localhost:)player`.
///
/// The class must derive from `object`, which is what gives the instance the UObject
/// representation everything below needs; a class that does not will fail to instantiate here
/// rather than at the first call.
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

/// One definition an identifier resolved to, with the source location to jump to.
struct FLookupDesc
{
    FUtf8String Name;
    /// Empty for a definition the project has no file for -- the generated Godot API, the Verse
    /// standard library -- which can be described but not jumped to.
    FUtf8String Path;
    /// Zero-based row; the column is a byte offset into it, which is how uLang counts and is not
    /// how Godot counts.
    int32 Line{-1};
    int32 Column{-1};
    FUtf8String Type;
    /// Name of the declaring scope: for a method, the class that declares it.
    FUtf8String Owner;
    vh_lookup_kind Kind{VH_LOOKUP_UNKNOWN};
    bool bIsVar{false};
    /// The cursor was on the definition itself, not on a reference to it.
    bool bIsDefinition{false};
    /// The definition this one immediately overrides, if any. An override cannot rename, so its
    /// name is Name above.
    FUtf8String OverriddenOwner;
    FUtf8String OverriddenPath;
    int32 OverriddenLine{-1};
    int32 OverriddenColumn{-1};
};

/// Resolves the identifier at Line/Column of Path against the analysed program.
///
/// Positions are the compiler's: zero-based rows, byte-offset columns. The caller is responsible
/// for only asking about text the last analysis actually saw -- nothing here can detect an edit
/// since, and a stale locus is a confident jump to the wrong line.
///
/// Only ever valid on an analysis-only program. Code generation replaces a definition's AST node
/// with an IR node, and the accessors this walks assert rather than fall back.
AUTORTFM_DISABLE bool LookupSymbol(FUtf8StringView Path, int32 Line, int32 Column, FLookupDesc& OutDesc);

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

AUTORTFM_DISABLE int32 RunMain(const TArray<verse::string>& Args, int64& OutExitCode);

AUTORTFM_DISABLE void TickScripts(double BudgetSeconds);

/// Releases the IDE, its data sources and the content scope. Must run before the engine tears
/// down: these objects free through GMalloc, which AppExit takes with it.
AUTORTFM_DISABLE void ResetScriptState();

} // namespace GodotVerse
