// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "HostLookup.h"
#include "HostResult.h"
#include "Templates/SharedPointer.h"
#include "VerseString.h"
#include "verse_host_abi.h"

// architecture-review.md item 1 step 1: wraps a switch meant to be exhaustive over an ABI enum, so
// a case missed for a new enumerator fails the build. Scoped to the switch itself rather than
// promoted for the module: -Wswitch is off by default here, and the module also links VNI's
// generated glue and reaches engine headers (Stats/StatsSystemTypes.h, PixelFormat.h,
// AutomationTest.h) whose own default-less switches this bridge does not own and cannot edit.
#if defined(__clang__)
#define VH_EXHAUSTIVE_SWITCH_BEGIN \
	_Pragma("clang diagnostic push") \
	_Pragma("clang diagnostic error \"-Wswitch\"")
#define VH_EXHAUSTIVE_SWITCH_END _Pragma("clang diagnostic pop")
#else
#define VH_EXHAUSTIVE_SWITCH_BEGIN
#define VH_EXHAUSTIVE_SWITCH_END
#endif

struct FVerseValue;

namespace Verse {
struct FRunningContext;
struct VValue;
}

namespace verse {
class vh_object;
struct variant;
}

class FJsonObject;

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

/// One generated binding: a Godot class the mirror does not carry, and the Verse class in the
/// bindings package that stands for it.
///
/// Exactly one key is set. GodotClass is what GetClassOf answers, for a ClassDB class;
/// ScriptClass is what GetScriptClassOf answers, for a class a script declares -- GetClassOf
/// answers the script's *native base* for one of those, so it cannot be the key.
struct FBindingClass
{
    FUtf8String GodotClass;
    FUtf8String ScriptClass;
    FUtf8String VerseClass;
};

/// Replaces the bindings package and the table that keys it.
///
/// Records and marks dirty; the package is put into the source project by the next build or
/// analysis, whichever comes first. Generational, so each *changed* source costs a package name --
/// publishing one name twice asserts inside the async loader (R-INT-8).
AUTORTFM_DISABLE void SetBindings(const FUtf8String& Source, TArray<FBindingClass>&& Classes);

/// The rows the last SetBindings was given, which is what the cook writes into the sidecar.
AUTORTFM_DISABLE const TArray<FBindingClass>& GetBindingClasses();

/// Installs a cooked game's binding table and names the package the cook published it as.
///
/// The half of SetBindings that an exported game can do: there is no source to compile -- the
/// bindings package is in the container beside the project's own -- so nothing is marked dirty and
/// no generation is prepared. Without it a handle whose class or script names a binding crosses as
/// its nearest *mirrored* ancestor, and every cast to the binding declines (R-INT-11).
AUTORTFM_DISABLE void AdoptCookedBindings(TArray<FBindingClass>&& Classes, FUtf8StringView PackageName);

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

/// Starts CheckProject on a thread we own. AnalysisInFlight when one is already running -- only
/// one runs at a time, because they share the IDE and the semantic program it rebuilds --
/// AnalysisNotReaped when the last one has finished and PollBackgroundCheck has not delivered it,
/// and NotBuilt before the first build.
///
/// Diagnostics are buffered rather than forwarded, so the Godot callback still only runs on the
/// game thread, out of PollBackgroundCheck.
AUTORTFM_DISABLE TResult<void> BeginBackgroundCheck(const FUtf8String& Path, const FUtf8String& SourceText);

/// Reaps a finished BeginBackgroundCheck and forwards its diagnostics. OutFinished is true only
/// on the call that reaps one; the return value is that analysis' result.
AUTORTFM_DISABLE bool PollBackgroundCheck(bool& OutFinished);

AUTORTFM_DISABLE bool IsBackgroundCheckRunning();

/// Blocks until any in-flight background check finishes, then reaps it.
///
/// Every entry point that *runs* Verse calls this first. VerseVM blocks execution for the length
/// of a build, so touching the VM while one runs trips `ensure(!bBlockAllExecution)` in
/// VVMExecutionContext and then kills the process. Completion and the argument hint call it too,
/// since both analyse a buffer of their own.
///
/// What no longer calls it is everything that only describes a class: see FAnalysisSnapshot, in
/// HostSnapshot.h.
AUTORTFM_DISABLE void WaitForBackgroundCheck();

/// Whether the program can answer a position question about SourceText as Path's text: nothing is
/// in flight, and the last analysis was of exactly this buffer.
///
/// Both halves are the caller's to act on rather than to wait out, which is what ABI v7 made of
/// completion and the argument hint: neither analyses any more, so this is the test that decides
/// between an answer and VH_ERR_STATE. Safe off the back of the atomic alone -- the worker clears
/// bRunning with a release store after RunCheck has recorded the buffer, so an acquire-load of
/// false is also the guarantee that the record is visible.
AUTORTFM_DISABLE bool ProgramDescribes(const FUtf8String& Path, const FUtf8String& SourceText);

/// Whether the program the IDE holds came from an analysis -- clean or not -- rather than from a
/// build, which may have generated code. Code generation hangs an IR package off every module and
/// the AST walk asserts rather than degrades when it meets one, so a position question has to be
/// declined until the next analysis puts an AST back -- which is a "not yet", not a "no such
/// symbol", and VH_ERR_NOT_ANALYSED is how the ABI spells that for these three entry points.
AUTORTFM_DISABLE bool ProgramIsAnalysisOnly();

/// What WaitForBackgroundCheck has cost the calling thread since this was last asked, and clears
/// it -- vh_tick reports it as the frame's figure, so anything else reading it would take a frame's
/// accounting away from the consumer.
///
/// Only the entry points that *run* Verse reach it now. Everything that merely describes a class
/// answers from the snapshot the last analysis left (HostSnapshot.h), so a figure that moves is a
/// frame in which something executed mid-analysis, which is what the monitor is for.
AUTORTFM_DISABLE void TakeAnalysisWaitStats(int32& OutCount, double& OutSeconds);

/// The class-describing reads below -- HasClass, GetClassMethods, GetClassSignals, GetClassStatics,
/// IsClassAbstract, GetClassExports, ReadClassDefaultField, ClassMembers, ResolveUnknownName --
/// answer out of a snapshot taken at the end of every analysis rather than by walking the semantic
/// program the caller happens to find. None of them waits for an analysis to finish.
///
/// The snapshot is extracted by whichever thread built the program and made current on the game
/// thread, so a reader never sees a half-built one and a reader is never the reason an editor
/// stalls. What it costs is that an answer describes the last analysis that *landed*: a class only
/// the buffer being typed declares appears when that analysis is reaped, a frame or so later, which
/// is when the diagnostics about it appear too.

/// One live Verse object: a script's `class(node2d)` bound to one Godot instance id.
struct FInstance;

/// Whether the compiled project defines a top-level class of that name deriving from `object`,
/// which is what makes a .verse file usable as a script at all. Cheap enough to ask per script.
AUTORTFM_DISABLE bool HasClass(FUtf8StringView ClassName);

/// The Godot class a script of this class attaches to: the nearest mirrored ancestor's own Godot
/// name, which is `Node2D` for a `class(node2d)` and `Resource` for a `class(resource)`. Empty for
/// a class the program does not carry.
///
/// The same walk R-NODE-3 mints a peer from, asked of a class name rather than of an object -- so
/// the two answers cannot drift apart, which they would if a script's declared base and its peer's
/// Godot class were computed separately.
AUTORTFM_DISABLE FUtf8String ClassBaseType(FUtf8StringView ClassName);

/// Instantiates the class ClassName defines at the top level of the compiled project and binds
/// it to a Godot object. ClassName is undecorated -- `player`, not `(/user@localhost:)player`.
///
/// The class must derive from `object`, which is what gives the instance the UObject
/// representation everything below needs; a class that does not will fail to instantiate here
/// rather than at the first call.
AUTORTFM_DISABLE FInstance* Instantiate(FUtf8StringView ClassName, int64 Handle);
AUTORTFM_DISABLE void ReleaseInstance(FInstance* Instance);

/// The Verse object a Godot handle crosses as (R-SCN-6). The script's own instance where the node
/// carries one -- which is what makes a downcast to a script class possible at all -- and a fresh
/// mirror wrapper of the handle's own Godot class otherwise. Never null.
///
/// Fallback is the class to wrap in when Godot will not say what the handle is: a caller that
/// already has a declared type to hold the value passes it, so a dead handle still produces
/// something the declaration accepts. The generated cast path passes null on purpose.
AUTORTFM_DISABLE UObject* ObjectForHandle(int64 Handle, UClass* Fallback = nullptr);

/// The mirrored class an object of Godot's GodotClassName crosses as, for a caller that knows the
/// class before it has a handle. Null for a Godot class this mirror does not carry.
AUTORTFM_DISABLE UClass* MirroredClassFor(FUtf8StringView GodotClassName);

/// R-NODE-3's half of a construction: the Godot peer for an object whose Verse constructor is
/// running. The handle the host is already holding for it where the host is the side doing the
/// constructing, and a fresh Godot object of the nearest mirrored ancestor of Self's class
/// otherwise. 0 for a class that has no peer to mint.
///
/// OutRefusedClass is set to the Godot class Godot declined to instantiate, and is the caller's
/// cue to raise. It is an out-parameter rather than a raise here because this runs inside an
/// AutoRTFM::Open and every other native in this bridge raises from outside one.
AUTORTFM_DISABLE int64 AdoptOrMintPeer(verse::vh_object* Self, const char*& OutRefusedClass);

/// Releases a peer AdoptOrMintPeer minted, if this object is the one that minted it. Called from
/// vh_object::BeginDestroy, so it runs on the collector's thread of control and must not touch the
/// VM -- and from the abort compensation, where nothing outside the transaction ever saw the
/// object, which is what bDiscard tells the consumer.
AUTORTFM_DISABLE void ReleaseMintedPeer(const UObject* Owner, int64 Handle, bool bDiscard = false);

/// Whether construction is running under a reading device -- the throwaway instance the export
/// defaults are read off, whose members initialize in full. AdoptOrMintPeer consults this itself;
/// it is exposed because a container's default mints through GodotBindings rather than through
/// that path, and the leak it guards against is the same one.
AUTORTFM_DISABLE bool IsMintSuppressed();

/// A Verse function value as a Godot Callable (R-TYPE-3's other direction, R-INT-4, and what
/// signals subscribe with). Answers the reference id of a Callable the GDExtension minted, or 0
/// for a value that is not a Verse function bound to a live script instance -- which is the only
/// shape 4a accepts, because it is the half of Godot's own design that does not leak.
AUTORTFM_DISABLE int64 MakeCallableFor(const FVerseValue& Callback);

/// Runs the Verse function behind a callback id, converting Args to its declared parameter types
/// exactly as a call from Godot's own dispatch would. Same answers as vh_instance_call.
AUTORTFM_DISABLE int32 InvokeCallback(int64 CallbackId,
                                      const vh_value* Args,
                                      int32 ArgCount,
                                      vh_value& OutResult,
                                      struct FFieldStorage& OutStorage);

AUTORTFM_DISABLE void ReleaseCallback(int64 CallbackId);


AUTORTFM_DISABLE bool InstanceHasFunction(const FInstance* Instance, FUtf8StringView DecoratedName);

/// One parameter of a script method, described from its declared type.
struct FParamDesc
{
    FUtf8String Name;
    vh_type Type{VH_TYPE_VOID};
    int32 VariantTag{VH_VARIANT_NIL};
    /// A `?Named:t = default` parameter, which a caller may omit.
    bool bHasDefault{false};
    /// The class an object-typed parameter names, as the consumer has to spell it to Godot,
    /// and which package declares it. Empty and VH_CLASS_NONE for any other type. See
    /// vh_param_desc::ClassUtf8 for what a script class that Godot has not registered
    /// reports instead.
    FUtf8String ClassName;
    int32 ClassKind{VH_CLASS_NONE};
};

/// One signal a script's class declares (R-SIG-1), as Godot's signal list wants it.
struct FSignalDesc
{
    /// The Verse spelling, verbatim -- `Hit`, `MobSpawned`. This is what Godot sees, following C#
    /// (which registers `Hit`, not `hit`) and what a Verse method already does in every scene
    /// connection.
    FUtf8String Name;
    /// One per Godot argument. A `tuple()` payload has none; a tuple of N has N; a struct has one
    /// per top-level field; anything else has one.
    TArray<FParamDesc> Args;
    int32 Line{0};
    int32 Column{0};
    /// vh_signal_reject. Anything but VH_SIGNAL_OK means this signal must not be registered with
    /// Godot -- it is reported so the editor can say why, at Line/Column.
    int32 Reject{0};
    /// The argument or field a payload rejection is about; empty for a rejection about the member.
    FUtf8String RejectDetail;

};

/// The signals ClassName declares, its base script classes' included (R-SIG-6.6: signals inherit,
/// and Phase 2 shipped that bug once already for @export). Read out of the semantic program, so it
/// refreshes per keystroke the way the method and export lists do.
AUTORTFM_DISABLE bool GetClassSignals(FUtf8StringView ClassName, TArray<FSignalDesc>& OutSignals);

/// One method's `@rpc` (R-EXP-9), as Godot's `rpc_config` Dictionary wants it.
///
/// The four values carry Godot's own numbering, so the consumer copies rather than translates,
/// and the defaults are filled in here -- authority, not call-local, reliable, channel 0 -- so
/// that the two sides cannot drift about what `@rpc("any_peer")` alone means. Those are
/// GDScript's defaults and SceneRPCInterface::_parse_rpc_config's both.
struct FRpcDesc
{
    /// The name Godot dispatches by: the Verse method's, or Godot's own name for the virtual it
    /// overrides, for the reason FMethodDesc carries one.
    FUtf8String Name;
    int32 RpcMode{2};
    bool bCallLocal{false};
    int32 TransferMode{2};
    int32 Channel{0};
    int32 Line{0};
    int32 Column{0};
    /// vh_rpc_reject. Anything but VH_RPC_OK means the method must not be registered as an RPC.
    int32 Reject{0};
    FUtf8String RejectDetail;
};

/// The methods of ClassName that carry an `@rpc`. A method with none is absent rather than
/// listed disabled, because absence is what Godot's own empty config means.
AUTORTFM_DISABLE bool GetClassRpcs(FUtf8StringView ClassName, TArray<FRpcDesc>& OutRpcs);

/// One member of a class's `@statics` module (R-NODE-4).
struct FStaticDesc
{
    FUtf8String Name;
    bool bIsFunction{false};
    int32 Line{0};
    int32 Column{0};
    /// Why a constant's wire value is empty: NotBuilt before the first build has published the
    /// package it is read from, or ValueToWire's reason for a value with no lane. Unset for a
    /// function and for a constant that crossed. Host-side only -- vh_static_desc has no field for
    /// it -- and not carried by the sidecar, which a cook writes after its own build.
    TOptional<EHostFailure> ValueFailure;
};

/// One class's statics, the three arrays running parallel: see FClassStatics below.
struct FClassStatics;

/// The members of the module that declares itself ClassName's statics. False when there is no such
/// class; an empty list when there is no such module, which is the ordinary case.
///
/// Answered out of the analysis snapshot, and OutStatics is what keeps that answer alive: a
/// vh_value in it points into the FFieldStorage beside it, so the block cannot be copied out and
/// the caller holds a share of it instead.
AUTORTFM_DISABLE bool GetClassStatics(FUtf8StringView ClassName, TSharedPtr<const FClassStatics>& OutStatics);

/// Whether the class is `class<abstract>` (R-NODE-5).
AUTORTFM_DISABLE bool IsClassAbstract(FUtf8StringView ClassName);

/// Emits the signal a binding id names, now. See the Verse declaration for why "now".
AUTORTFM_DISABLE void EmitSignal(int64 SignalId, const FVerseValue& Payload);

/// Connects Callback and answers a subscription id, or 0. Registers an
/// `AutoRTFM::OnAbort<SameAsClosed>` that disconnects: this mutates Godot *and* returns a value, so
/// it can be neither deferred to commit nor ignored, and an aborted transaction must not leave a
/// connection the script believes it never made.
AUTORTFM_DISABLE int64 SubscribeSignal(int64 SignalId, const FVerseValue& Callback);

/// Disconnects. Idempotent, as event_subscription::Cancel is in UEFN.
AUTORTFM_DISABLE void CancelSubscription(int64 SubscriptionId);

/// The same two, for an `@export_signal` member, which names its binding by the `event(t)` object
/// it holds rather than by an id: `event` is Verse's own class and cannot be reopened to carry an
/// `Id` field the way the native `vh_signal` does.
///
/// Both answer the binding the object was given at vh_instantiate and then do exactly what the
/// id-taking pair above does -- the emission still goes out to Godot and comes back through the
/// member's permanent connection, which is what makes a Verse handler and a GDScript handler see
/// one ordering (R-SIG-1).
AUTORTFM_DISABLE void EmitEventSignal(UObject* Event, const FVerseValue& Payload);
AUTORTFM_DISABLE int64 SubscribeEventSignal(UObject* Event, const FVerseValue& Callback);

/// Begins one wait on the signal a `signal` object names (R-SIG-5), answering a token.
///
/// The object rather than its id, because an engine-signal accessor mints a fresh `signal`
/// on every call and several of them share one binding -- only the object says which event a given
/// wait is suspended on. Held for the life of the token, so awaiting a temporary is safe.
///
/// 0 for a signal that names nothing or that Godot refused to register, having said why.
AUTORTFM_DISABLE int64 BeginSignalAwait(UObject* Signal);

/// The same for a Godot Signal value -- one a GDScript or C# script declared, or one made with
/// `add_user_signal`. Nothing declares its payload, so the emission's arguments reach Waiter's
/// event as one Godot Array.
AUTORTFM_DISABLE int64 BeginSignalRefAwait(int64 Ref, UObject* Waiter);

/// Ends one wait: disconnects, releases the Callable, and drops the host's claim on the object.
/// Idempotent, because the `defer` that calls it may run after a one-shot connection is already
/// gone.
AUTORTFM_DISABLE void EndSignalAwait(int64 Token);

/// Connects Callback to a Godot Signal value, compensated on abort the way SubscribeSignal is.
/// This is what closes `Object.Connect`'s rollback gap (R-SIG-6): the handler receives the
/// emission's arguments as one Godot Array, since nothing declares them.
AUTORTFM_DISABLE int64 SubscribeSignalRef(int64 Ref, const FVerseValue& Callback);

/// Binds one of Godot's own signals on one object, for the accessors the generator emits per class
/// (R-SIG-3's other half). Answers a binding id, deduplicated per (owner, signal) so an accessor
/// called in a loop does not grow the table.
///
/// The payload's declared type is read off the generated accessor's *return* type, which is a
/// `signal(t)` instantiation -- the same road a declared member's payload is read by, and the
/// reason nothing here has to be told what a Godot signal carries.
AUTORTFM_DISABLE int64 BindEngineSignal(int64 Handle,
                                        FUtf8StringView ClassName,
                                        FUtf8StringView AccessorName,
                                        FUtf8StringView SignalName);

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
    /// The class an object-typed result names, decided exactly as FParamDesc::ClassName is.
    FUtf8String ResultClassName;
    int32 ResultClassKind{VH_CLASS_NONE};

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

/// What a Verse `ToString` extension method answers for this instance -- R-NODE-10's first hook.
///
/// Godot asks a script for its own text through `to_string_func`, and Verse's spelling for that is
/// not a virtual on the native root but an extension method:
///
///     (X:my_class).ToString<public>()<transacts>:string = "..."
///
/// which is a module-level `operator'.ToString'(:my_class, :tuple())` -- receiver first, the call's
/// own arguments as a tuple second. A class *member* of that name cannot be written at all (glitch
/// 3532 against /Verse.org/Verse's own ToString), so InstanceCall can never reach one and this has
/// its own entry point. `tests/verse_probe/tostring_probe.verse` carries the five rounds that
/// settled it.
///
/// VH_ERR_NOT_FOUND when the class has no such method, which is the common case and means Godot
/// should keep its own representation rather than show an empty string.
AUTORTFM_DISABLE int32 InstanceToString(FInstance* Instance, vh_value& OutResult, FFieldStorage& OutStorage);

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

/// Verse's `variant` from a wire value, and back. Defined in GodotBindings.cpp, where the lane
/// rules are, because a second implementation of them is a second chance to disagree about which
/// lane a Rect2 puts its height in.
///
/// VariantToWire's result points into OutText and OutComponents, which the caller owns and must
/// keep alive for as long as the vh_value is read -- the same contract FFieldStorage has.
AUTORTFM_DISABLE verse::variant VariantFromWire(const vh_value& Value);
AUTORTFM_DISABLE vh_value VariantToWire(const verse::variant& Value,
                                        FUtf8String& OutText,
                                        TArray<vh_value>& OutComponents);

/// The statics of one class, as GetClassStatics answers them. Values and Storage run parallel to
/// Statics, with a wire value per *constant* and a function's slot left empty.
///
/// One block rather than three arrays a caller supplies, because a vh_value in Values points into
/// the FFieldStorage beside it: the three only mean anything together, and copying any of them
/// away from the others leaves a dangling Seq.
struct FClassStatics
{
    TArray<FStaticDesc> Statics;
    TArray<vh_value> Values;
    TArray<FFieldStorage> Storage;
};

/// One value and the bytes it points at, for the same reason FClassStatics is one block.
struct FFieldValue
{
    vh_value Value{};
    FFieldStorage Storage;
};

/// Makes a table read back from a sidecar the one BindEngineSignal consults. Game thread only.
AUTORTFM_DISABLE void SetRecordedEngineSignalTypes(TSharedPtr<struct FEngineSignalTypes> Types);

/// Reads one data member off a live instance into the ABI's value shape, or says why not: the
/// member is absent, holds no value yet, or holds one with no wire representation.
AUTORTFM_DISABLE TResult<void> ReadInstanceField(const FInstance* Instance,
                                                 FUtf8StringView FieldName,
                                                 vh_value& OutValue,
                                                 FFieldStorage& OutStorage);

/// The same read against the class default object, whose Verse constructor has already run.
/// That is where a member's declared default has to come from: the semantic program can say only
/// that an initializer exists (CDataDefinition::HasInitializer), not what it evaluates to.
///
/// Every `@export` of every class is read into the analysis snapshot, so this is a lookup. A member
/// that is not an export is read on the spot, and only while no analysis is in flight -- reading it
/// means entering the VM, which a running build forbids.
AUTORTFM_DISABLE bool ReadClassDefaultField(FUtf8StringView ClassName,
                                            FUtf8StringView FieldName,
                                            TSharedPtr<const FFieldValue>& OutValue);

/// Writes one data member on a live instance. Covers the same four types the read path does.
///
/// A `var` may be written at any time. A non-var may be written only while the instance is
/// *unsealed* -- between Instantiate and the first call into it -- which is the window Godot uses
/// to apply the values a scene stored. That keeps Verse's immutability honest: a non-var still
/// never changes once the script can observe it, so the inspector value reads as an initializer
/// rather than a mutation. Sealing is one-way and happens on the first InstanceCall*.
///
/// NotAssignable is a non-var on a sealed instance or a shape constant; the converters' reasons are
/// a value the declared type cannot take.
AUTORTFM_DISABLE TResult<void> WriteInstanceField(FInstance* Instance, FUtf8StringView FieldName, const vh_value& Value);

/// Writes a reference member with another instance rather than with a handle.
///
/// A handle would not do. The object a member typed as one of the project's own classes should hold
/// is the one that node's own instance already is, and building a second from the handle would give
/// one node two Verse objects -- two sets of members, and no way for the author to tell which of
/// them they are looking at. A mirrored member takes one too, for the same reason read the other
/// way round: where the object exists, holding it beats copying its handle.
///
/// Value may be null, which clears the member. TypeMismatch for a member that is not a reference or
/// one whose declared class Value is not an instance of, and WriteInstanceField's reasons for one
/// that cannot be written.
AUTORTFM_DISABLE TResult<void> WriteInstanceFieldInstance(FInstance* Instance,
                                                          FUtf8StringView FieldName,
                                                          const FInstance* Value);

AUTORTFM_DISABLE int32 RunMain(const TArray<verse::string>& Args, int64& OutExitCode);

/// Runs queued Verse work: `Sleep` resumptions, `Main`, and anything else with no Godot event
/// behind it. A task waiting on a Godot signal is not here -- it resumes inside the emission
/// (phase-5-design.md D3/D4), which is where GDScript resumes its coroutines too.
AUTORTFM_DISABLE void TickScripts(double BudgetSeconds, vh_tick_stats* OutStats);

/// Reports what a raise is about to cancel.
///
/// Called from the OnVerseRuntimeError delegate, which UE broadcasts immediately *before* it
/// terminates the content scope -- the last moment the task group can be asked what is about to
/// be thrown away. Since Phase 5 the scope being terminated is the raising *instance's*, so what
/// is lost is that node's suspended work rather than the project's.
AUTORTFM_DISABLE void NoteRuntimeErrorRaised();

/// Releases the IDE, its data sources and the content scope. Must run before the engine tears
/// down: these objects free through GMalloc, which AppExit takes with it.
AUTORTFM_DISABLE void ResetScriptState();

/// Adopts a generation a *cook* published, for a host that has no compiler to publish one of its
/// own. Everything downstream -- which package a class is looked up in, whether the project is
/// built at all -- reads the same two pieces of state a CompileProject would have set.
AUTORTFM_DISABLE void AdoptCookedGeneration(FUtf8StringView PackageName, int32 Generation);

#if VH_HOST_KIND == VH_HOST_KIND_COOKER
/// vh_init with the engine boot left to the caller. Defined in VerseHost.cpp beside vh_init, which
/// is the same function with the PreInit put back.
AUTORTFM_DISABLE int32_t InitCookerAfterEngineBoot(const vh_init_desc& Desc);
#endif

} // namespace GodotVerse
