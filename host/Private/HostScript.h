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

/// One source file of the project, and the module its definitions go into.
struct FScriptSource
{
    FUtf8String Path;
    /// '/'-separated, relative to the package's root module. Empty is the root module itself.
    FUtf8String ModulePath;
};

/// Builds every source as one program and publishes it as a new generation, writing that
/// generation's number -- counting from 1 -- through OutGeneration.
///
/// Callable as often as the caller likes. Each generation gets a package name no publish has
/// used, because publishing marks a package's exports LoaderImport and republishing the same
/// package asserts on the flag; the previous generation's package is retired from the source
/// project but stays live in the VM, so its instances go on running against it.
///
/// A failed build publishes nothing and leaves OutGeneration alone.
AUTORTFM_DISABLE bool CompileProject(const TArray<FScriptSource>& Sources, int32& OutGeneration);
/// Re-runs semantic analysis over the whole project with one file's text replaced by the
/// editor's unsaved buffer, reporting fresh diagnostics through the init callback. Generates
/// nothing, so the running program is untouched and this may be called as often as the editor
/// asks -- it is a CompileProject with the code generation taken out, which is both the
/// expensive half and the half that publishes.
AUTORTFM_DISABLE bool CheckProject(const FUtf8String& Path, const FUtf8String& SourceText);

/// Which of the project's modules declare a top-level Name, by module path relative to the
/// package root. The root module is never one of them: it is in scope everywhere, so a name it
/// declares never needs an import. Answers out of the program the last analysis left behind.
AUTORTFM_DISABLE bool ResolveUnknownName(FUtf8StringView Name, TArray<FUtf8String>& OutModules);

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

/// One parameter of a script method, described from its declared type.
struct FParamDesc
{
    FUtf8String Name;
    vh_type Type{VH_TYPE_VOID};
    int32 VariantTag{VH_VARIANT_NIL};
    /// A `?Named:t = default` parameter, which a caller may omit.
    bool bHasDefault{false};
};

/// One method a script's class declares.
struct FMethodDesc
{
    /// The Verse name, undecorated. This is the name Godot sees, verbatim.
    FUtf8String Name;
    /// What InstanceCall takes. The VM keys an override under the *declaring* class' decorated
    /// name, which is what CFunction::GetDecoratedName produces and why it is asked rather than
    /// assembled here.
    FUtf8String DecoratedName;

    TArray<FParamDesc> Params;
    /// How many leading parameters have no default, and so must be supplied.
    int32 RequiredParamCount{0};

    vh_type ResultType{VH_TYPE_VOID};
    int32 ResultVariantTag{VH_VARIANT_NIL};

    /// Declared <decides>: the call may run and decline, which is not the same as being absent.
    bool bCanFail{false};
    /// Declared <suspends>: calling it starts a task rather than running it to completion.
    bool bSuspends{false};

    /// Godot's own name for the virtual this overrides -- `_ready` -- or empty when it overrides
    /// nothing of Godot's. Derived rather than tabulated: a method whose base definition lives in
    /// the mirrored package is a Godot virtual, and Godot's name for it is the snake_case of the
    /// Verse one with a leading underscore, which is the exact inverse of the rule the generator
    /// used to name it. So a virtual added by a future Godot version arrives by regenerating the
    /// mirror, with no change here (R-NODE-7).
    FUtf8String GodotVirtual;

    int32 Line{-1};
    int32 Column{-1};
};

/// Every method ClassName declares, read off the semantic program the last analysis left -- the
/// same source, and for the same reason, as GetClassExports.
///
/// False means the class is not in the analysed program at all; a class with no methods of its own
/// is true with an empty array.
AUTORTFM_DISABLE bool GetClassMethods(FUtf8StringView ClassName, TArray<FMethodDesc>& OutMethods);

/// Calls any method the script declares and answers its result.
///
/// Arguments are converted against the parameter types GetClassMethods reported, so this needs no
/// per-shape entry point -- which is what replaced v1's two hardcoded call shapes.
///
/// Answers a vh_status: VH_OK with OutResult written, VH_ERR_NOT_FOUND for an unknown method,
/// VH_ERR_ARGUMENT for the wrong arity or an argument the declared type cannot accept (neither
/// having run anything), VH_ERR_FAILED for a <decides> method that ran and declined, and
/// VH_ERR_RUNTIME for one that raised.
///
/// Calling into the instance seals it: see WriteInstanceField.
AUTORTFM_DISABLE int32 InstanceCall(FInstance* Instance,
                                    FUtf8StringView DecoratedName,
                                    const vh_value* Args,
                                    int32 ArgCount,
                                    vh_value& OutResult,
                                    struct FFieldStorage& OutStorage);

/// One `@export` data member, harvested from the semantic program rather than the VM.
struct FExportDesc
{
    FUtf8String Name;
    vh_type Type{VH_TYPE_VOID};
    /// vh_variant_tag: which Godot type to rebuild the value as, where the layout alone cannot
    /// say. VH_VARIANT_NIL leaves the consumer to infer it from Type.
    int32 VariantTag{VH_VARIANT_NIL};
    /// vh_variant_tag: what an element of a plain Array is. A packed array carries that in its own
    /// tag; an Array does not, and an inspector told only "Array" offers an editor that can add
    /// anything. VH_VARIANT_NIL where there is nothing to say.
    int32 ElementVariantTag{VH_VARIANT_NIL};
    bool bIsVar{false};

    /// The inspector hint the *declaration* implies -- a range off a constrained int or float,
    /// the enumerators of an enum, the class of a Godot reference. vh_export_hint.
    int32 Hint{VH_EXPORT_HINT_NONE};
    /// The enumerators or the class name; a range speaks through the bounds below instead.
    FUtf8String HintString;
    /// The mirrored class whose Godot counterpart says whether the slot picks a node or a resource:
    /// the referenced class itself where that is one of the mirrors, and its nearest mirrored ancestor
    /// where it is one of the project's own -- ClassDB cannot place a name a script registered.
    FUtf8String NativeClass;

    /// The bounds of a constrained number as the type carries them, absent rather than infinite
    /// at an end the type leaves open. A strict inequality arrives already normalised to the
    /// adjacent value, which is all the consumer needs.
    double RangeMin{0.0};
    double RangeMax{0.0};
    bool bHasRangeMin{false};
    bool bHasRangeMax{false};

    /// The inspector section this member opens, and vh_export_group saying at which of Godot's
    /// three nesting depths. Every member after it joins that section until one opens another.
    int32 GroupKind{VH_EXPORT_GROUP_NONE};
    FUtf8String GroupName;

    /// Where the member is declared, for a consumer that wants to say something about it. Zero
    /// based row, utf8 byte column, both -1 when the definition has no source location.
    int32 Line{-1};
    int32 Column{-1};

    /// vh_export_reject. A member that cannot be exported is still harvested, so that whoever
    /// asked can say why rather than leave the author staring at an inspector missing a property.
    int32 Reject{VH_EXPORT_OK};
};

/// Fills OutExports with the `@export` data members ClassName declares, reading the
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
    /// A parameter of the function that declares it. It has no documentation of its own: its
    /// source line is the line the whole function is declared on.
    bool bIsParameter{false};
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

/// One name completion could offer, described the way FLookupDesc describes a definition minus
/// the location -- completion says what a name is, not where it was written.
struct FCompleteItem
{
    FUtf8String Name;
    FUtf8String Type;
    FUtf8String Owner;
    /// Where it was declared, empty and -1 for a definition with no source behind it. Carried so
    /// a consumer can find the comment block above a name it is offering or documenting.
    FUtf8String Path;
    int32 Line{-1};
    vh_lookup_kind Kind{VH_LOOKUP_UNKNOWN};
    bool bIsVar{false};
    /// Parameters declared, or -1 for anything that is not a function.
    int32 ParamCount{-1};
    /// A function's declaration after its name, as Verse source: "(Delta:float)<transacts>:void".
    /// The parameter names live on the signature rather than the function type, so Type cannot
    /// carry them and an editor writing a declaration has nowhere else to get them.
    FUtf8String Signature;
    /// Whether a subclass could declare this with <override>.
    bool bIsOverridable{false};
};

/// The parameters of one function, for an editor's argument hint.
struct FSignatureDesc
{
    FUtf8String Name;
    FUtf8String Result;
    TArray<FCompleteItem> Params;
};

/// Lists what could be written at Line/Column of Path, with that file's text replaced by
/// SourceText -- the members of the expression there, or everything its scope admits.
///
/// Runs its own analysis rather than reading whatever the last one left, because the buffer
/// completion is asked about is mid-edit by definition and no analysis of it exists yet. That
/// analysis' diagnostics are discarded: they describe a half-written line. It does not have to
/// succeed -- uLang keeps the analysed children of an expression it could not analyse, which is
/// what lets `Position.` still name vector2 as the receiver.
///
/// Leaves the IDE holding SourceText as that file's text, so any analysis a caller was relying on
/// for LookupSymbol is spent once this has run.
/// Every member ClassName declares itself, read off the semantic program the last analysis left.
/// Broader than GetClassExports -- methods included, `@editable` not required -- because this
/// exists to become documentation rather than an inspector.
///
/// False means the class is not in the analysed program at all.
AUTORTFM_DISABLE bool ClassMembers(FUtf8StringView ClassName, TArray<FCompleteItem>& OutItems);

/// The function called at Line/Column of Path, with that file's text replaced by SourceText, and
/// its parameters. Line/Column name the callee's last byte rather than the cursor: the argument
/// list being typed does not analyse, so there is nothing at the cursor to resolve.
///
/// Analyses the buffer and spends the caller's analysis exactly as Complete does.
AUTORTFM_DISABLE bool SignatureAt(FUtf8StringView Path,
                                  const FUtf8String& SourceText,
                                  int32 Line,
                                  int32 Column,
                                  FSignatureDesc& OutDesc);

AUTORTFM_DISABLE bool Complete(FUtf8StringView Path,
                               const FUtf8String& SourceText,
                               int32 Line,
                               int32 Column,
                               vh_complete_mode Mode,
                               TArray<FCompleteItem>& OutItems);

/// Backing store for the parts of a value a vh_value points at rather than holds: a string's bytes
/// and a tuple's items.
///
/// It has to outlive the VM scope the value was read in. VArray::AsStringView points into a
/// GC-managed cell, and a struct's fields are read one at a time into storage of our own, so neither
/// can be pointed at where it lies.
struct FFieldStorage
{
    FUtf8String Text;
    /// One block per vh_value Seq: an array's own items, and one more for each struct inside it.
    /// Blocks is reserved to its final length before any block is filled and no block is resized
    /// afterwards, because a vh_value holds a bare pointer into one -- growing either would leave
    /// that pointing at freed memory. (Growing Blocks would move only the TArray headers, whose
    /// allocations follow them, but an outstanding reference *into* Blocks would still dangle, so
    /// blocks are addressed by index.)
    TArray<TArray<vh_value>> Blocks;
    /// The bytes of each string element, for the reason Text exists.
    TArray<FUtf8String> Strings;
};

/// Reads one data member off a live instance into the ABI's value shape.
AUTORTFM_DISABLE bool ReadInstanceField(const FInstance* Instance, FUtf8StringView FieldName, vh_value& OutValue, FFieldStorage& OutStorage);

/// The same read against the class default object, whose Verse constructor has already run.
/// That is where a member's declared default has to come from: the semantic program can say only
/// that an initializer exists (CDataDefinition::HasInitializer), not what it evaluates to.
AUTORTFM_DISABLE bool ReadClassDefaultField(FUtf8StringView ClassName, FUtf8StringView FieldName, vh_value& OutValue, FFieldStorage& OutStorage);

/// Writes one data member on a live instance. Covers the same four types the read path does.
///
/// A `var` may be written at any time. A non-var may be written only while the instance is
/// *unsealed* -- between Instantiate and the first call into it -- which is the window Godot uses
/// to apply the values a scene stored. That keeps Verse's immutability honest: a non-var still
/// never changes once the script can observe it, so the inspector value reads as an initializer
/// rather than a mutation. Sealing is one-way and happens on the first InstanceCall*.
AUTORTFM_DISABLE bool WriteInstanceField(FInstance* Instance, FUtf8StringView FieldName, const vh_value& Value);

/// Writes a reference member with another instance rather than with a handle.
///
/// A handle would not do. The object a member typed as one of the project's own classes should hold
/// is the one that node's own instance already is, and building a second from the handle would give
/// one node two Verse objects -- two sets of members, and no way for the author to tell which of
/// them they are looking at. A mirrored member takes one too, for the same reason read the other
/// way round: where the object exists, holding it beats copying its handle.
///
/// Value may be null, which clears the member. False for a member that is not a reference, one that
/// cannot be written, or one whose declared class Value is not an instance of.
AUTORTFM_DISABLE bool WriteInstanceFieldInstance(FInstance* Instance, FUtf8StringView FieldName, const FInstance* Value);

AUTORTFM_DISABLE int32 RunMain(const TArray<verse::string>& Args, int64& OutExitCode);

AUTORTFM_DISABLE void TickScripts(double BudgetSeconds);

/// Releases the IDE, its data sources and the content scope. Must run before the engine tears
/// down: these objects free through GMalloc, which AppExit takes with it.
AUTORTFM_DISABLE void ResetScriptState();

} // namespace GodotVerse
