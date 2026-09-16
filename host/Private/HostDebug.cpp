// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostDebug.h"

#include "AutoRTFM.h"

#include "Containers/Map.h"
#include "Containers/Utf8String.h"
#include "GodotClasses.h"
#include "HAL/PlatformTime.h"
#include "HostRuntime.h"
#include "String/Find.h"
#include "VerseVM/Inline/VVMValueInline.h"
#include "VerseVM/Inline/VVMVerseClassInline.h"
#include "VerseVM/VVMArrayBase.h"
#include "VerseVM/VVMContext.h"
#include "VerseVM/VVMContextImpl.h"
#include "VerseVM/VVMDebugger.h"
#include "VerseVM/VVMFloat.h"
#include "VerseVM/VVMFrame.h"
#include "VerseVM/VVMFunction.h"
#include "VerseVM/VVMGlobalHeapPtr.h"
#include "VerseVM/VVMInt.h"
#include "VerseVM/VVMLocation.h"
#include "VerseVM/VVMNativeProcedure.h"
#include "VerseVM/VVMNativeRef.h"
#include "VerseVM/VVMProcedure.h"
#include "VerseVM/VVMProfilingLibrary.h"
#include "VerseVM/VVMRef.h"
#include "VerseVM/VVMShape.h"
#include "VerseVM/VVMUniqueString.h"
#include "VerseVM/VVMValuePrinting.h"
#include "VerseVM/VVMVerseClass.h"

namespace GodotVerse {
namespace {

/// The stop the VM is currently sitting in. Notify's four arguments are valid only for the
/// duration of that call, so this lives for exactly as long as the consumer's DebugBreak is on
/// the stack and is cleared on the way out. Every vh_debug_* read is defined only while it is set.
struct FStash
{
    const Verse::FOp* PC{nullptr};
    Verse::VFrame* Frame{nullptr};
    Verse::VTask* Task{nullptr};
    const Verse::FNativeFrame* NativeFrame{nullptr};
    bool bValid{false};
};

FStash GStash;

/// The running context, rebuilt from its promise rather than stored: FRunningContext is not
/// assignable, and every read below runs inside the Notify that set the stash -- so the context it
/// would have copied is the one already current on this thread.
AUTORTFM_DISABLE Verse::FRunningContext StashContext()
{
    return Verse::FRunningContext{Verse::FRunningContextPromise{}};
}

/// Read by the reads rather than rebuilt per question: Godot asks for the count, then each frame,
/// then each frame's locals, and walking the stack once per question would be three walks of the
/// same stopped stack.
///
/// Holds no VValue -- only what has already been turned into bytes or a wire value -- because it
/// outlives nothing: it is rebuilt at every stop and dropped when the stop ends.
struct FStoppedFrame
{
    FUtf8String Path;
    FUtf8String Name;
    int32 Line{0};
    /// Innermost-first, matching what ForEachStackFrame walks.
    TArray<FDebugValue> Locals;
    TArray<FDebugValue> Members;
    bool bValuesRead{false};
};

TArray<FStoppedFrame> GStoppedFrames;

/// Whether the consumer's DebugBreak is on the stack. Distinct from GStash.bValid only in that it
/// is what the *other* entry points test: a nested vh_instance_call arriving from inside Godot's
/// debug loop must be refused, and the stash is what it would have read.
bool GStopped = false;

/// ------------------------------------------------------------------------ the debugger --

/// The interpreter state at the previous Notify, and the frame the last stop happened in.
///
/// Global heap pointers rather than raw ones, which is Epic's own shape (VVMSocketDebugger.cpp
/// holds the same three in write barriers on a VCell). The identity comparison below never
/// dereferences a stale pointer, but a collected frame reallocated at the same address would read
/// as "unchanged" and skip a stop, and keeping them reachable is what rules that out.
Verse::TGlobalHeapPtr<Verse::VFrame> GPrevFrame;
Verse::TGlobalHeapPtr<Verse::VUniqueString> GPrevFilePath;
Verse::FLocation GPrevLocation{0};
Verse::TGlobalHeapPtr<Verse::VFrame> GLastStoppedFrame;

/// Epic's UpdatePrevLocation, verbatim in effect: same frame, same file, same location means the
/// consumer has already been asked about this spot.
///
/// **Not optional.** Notify fires per bytecode op, not per line, so without this the consumer is
/// asked millions of times a second and every one of those crosses the ABI.
AUTORTFM_DISABLE bool UpdatePrevLocation(Verse::FAccessContext Context,
                                         Verse::VFrame& Frame,
                                         Verse::VUniqueString& FilePath,
                                         const Verse::FLocation& Location)
{
    if (GPrevFrame.Get() == &Frame && GPrevFilePath.Get() == &FilePath && GPrevLocation == Location)
    {
        return false;
    }
    GPrevFrame.Set(Context, &Frame);
    GPrevFilePath.Set(Context, &FilePath);
    GPrevLocation = Location;
    return true;
}

AUTORTFM_DISABLE bool IsAncestorOf(const Verse::VFrame& Left, const Verse::VFrame* I)
{
    for (; I; I = I->CallerFrame.Get())
    {
        if (I == &Left)
        {
            return true;
        }
    }
    return false;
}

/// How Frame relates to the frame the last stop was in, in the three terms
/// vh_debug_frame_relation names. Epic's IsAncestorOf/IsProperAncestorOf, asked once and answered
/// as a value rather than as two predicates, because the consumer is the side that decides which
/// of them a pending step wants.
AUTORTFM_DISABLE int32 FrameRelation(Verse::VFrame& Frame)
{
    Verse::VFrame* const Last = GLastStoppedFrame.Get();
    if (!Last)
    {
        // Only reachable when a step was asked for while nothing was stopped, which Epic's own
        // comment calls out as the one case its LastStoppedFrame can be null in.
        return VH_DEBUG_FRAME_OTHER;
    }
    if (Last == &Frame)
    {
        return VH_DEBUG_FRAME_SAME;
    }
    return IsAncestorOf(*Last, Frame.CallerFrame.Get()) ? VH_DEBUG_FRAME_DEEPER : VH_DEBUG_FRAME_OTHER;
}

/// Turns one register into either a wire value or a rendering (D7).
///
/// The rule is what the value *alone* identifies, because a stopped frame carries no declaration
/// to consult -- and reaching for one would mean asking the semantic program from inside the
/// interpreter. An int, a float, a string, a Godot object and a mirrored math struct cross typed;
/// everything else renders -- a tuple, an option, a map, a class instance of the author's own and
/// every container wrapper, none of which vh_value describes without being told what it is.
///
/// The math structs are in the first list rather than the second because they are the one shape a
/// value *can* name itself: `vector2` says so through its class, where an empty array cannot say
/// what it holds and a `false` cannot say whether it is a logic or an empty option.
AUTORTFM_DISABLE void ReadDebugValue(Verse::FRunningContext Context,
                                     Verse::VValue Value,
                                     int32 MaxDepth,
                                     FDebugValue& Out)
{
    if (Value.IsUninitialized())
    {
        // A register outside its live range at this bytecode offset. Reported rather than dropped:
        // the name is in scope in the source the author is looking at, and its absence from the
        // list would read as a bug in the debugger. Epic's DAP client says the same thing.
        Out.Rendered = UTF8TEXT("<not yet in scope>");
        return;
    }

    if (Verse::VRef* Ref = Value.DynamicCast<Verse::VRef>())
    {
        Value = Ref->Get(Context);
        if (Value.IsUninitialized())
        {
            Out.Rendered = UTF8TEXT("<not yet in scope>");
            return;
        }
    }

    // Shared with `Variant(Value:any)`, which asks the identical question from the other side: what
    // does this value say about itself when nothing has declared its type? A `vector2` in the
    // inspector must be a Vector2 rather than the text of one, which is what the math-struct arm is
    // there for -- the rendering below is honest and unusable.
    if (GodotVerse::ReadSelfDescribingValue(Context, Value, Out.Storage, Out.Value))
    {
        Out.bHasValue = true;
        return;
    }

    Out.Rendered = Verse::ToString(Value, Context, Verse::EValueStringFormat::VerseSyntax,
                                   static_cast<uint32>(FMath::Max(MaxDepth, 0)));
}

/// The bare Verse spelling of a decorated name: `(/user@localhost/player:)Health` is `Health`.
///
/// Every name the VM hands back is decorated -- a procedure's Name, a shape key, both. The first
/// `:)` is the end of the scope and the *first* is load-bearing: a method's key carries its
/// parameter list, so `(/Godot.org/Godot/object:)Connect(:[]char,:callable)` has a closing paren
/// that is not the scope's.
FUtf8StringView Undecorate(FUtf8StringView Decorated)
{
    const int32 Index = UE::String::FindFirst(Decorated, FUtf8StringView(UTF8TEXT(":)")));
    return Index == INDEX_NONE ? Decorated : Decorated.RightChop(Index + 2);
}

/// Self and its fields, which is the only place a script instance's state can appear: Godot calls
/// a C++ virtual on whatever _debug_get_stack_level_instance answers, and a GDExtension script
/// instance is not a ScriptInstance, so that virtual answers null forever (D8).
AUTORTFM_DISABLE void ReadMembers(Verse::FRunningContext Context,
                                  Verse::VValue Self,
                                  int32 MaxDepth,
                                  TArray<FDebugValue>& Out)
{
    FDebugValue& SelfEntry = Out.AddDefaulted_GetRef();
    SelfEntry.Name = UTF8TEXT("Self");
    ReadDebugValue(Context, Self, MaxDepth, SelfEntry);

    UObject* const Object = Self.ExtractUObject();
    if (!Object)
    {
        return;
    }

    Verse::VShape& Shape = UVerseClass::GetShapeForLoadField(Context, Object->GetClass());
    for (Verse::VShape::FieldsMap::TIterator It = Shape.CreateFieldsIterator(); It; ++It)
    {
        Verse::VUniqueString* const Key = It.Key().Get();
        if (!Key)
        {
            continue;
        }
        const Verse::VShape::VEntry& Entry = It.Value();

        // A shape carries methods and accessors as well as data, and a script class inherits ~52
        // of Godot's own -- which would bury the three the author wrote under a list of function
        // values. Godot's members panel means instance state, and so does this.
        if (Entry.IsMethod() || Entry.IsAccessor())
        {
            continue;
        }

        // PeekField over LoadField, for the reason ReadFieldOf gives: an unset member reads as
        // uninitialized here where LoadField would raise, and a debugger asking for a value it may
        // not get is not an error condition.
        Verse::VValue Value = Entry.Type == Verse::EFieldType::FPropertyVar
            ? Verse::VNativeRef::Peek(Context, Object, Entry.UProperty)
            : UVerseClass::PeekField(Context, Object, &Entry);

        FDebugValue& Field = Out.AddDefaulted_GetRef();
        Field.Name = FUtf8String(Undecorate(Key->AsStringView()));
        ReadDebugValue(Context, Value, MaxDepth, Field);
    }
}

/// Walks the stopped stack once and turns it into bytes. Called lazily: Godot always asks for the
/// frame count first, and a stop the user continues straight past never pays for a walk.
AUTORTFM_DISABLE void EnsureStackWalked()
{
    if (!GStash.bValid || !GStoppedFrames.IsEmpty())
    {
        return;
    }

    Verse::Debugger::ForEachStackFrame(
        StashContext(), GStash.PC, GStash.Frame, GStash.Task, GStash.NativeFrame,
        [](const Verse::FLocation* Location, Verse::Debugger::FFrame Frame) {
            FStoppedFrame& Out = GStoppedFrames.AddDefaulted_GetRef();
            if (Verse::VUniqueString* const Name = Frame.Name.Get())
            {
                Out.Name = FUtf8String(Undecorate(Name->AsStringView()));
            }
            if (Verse::VUniqueString* const Path = Frame.FilePath.Get())
            {
                Out.Path = FUtf8String(Path->AsStringView());
            }
            Out.Line = Location ? static_cast<int32>(Location->Line) : 0;

            // The registers are read here rather than on demand, because FFrame owns them and it
            // dies with this callback -- there is no later moment at which they could be asked
            // for. Self is separated out as it goes past: ForEachStackFrame prepends it under that
            // name when the frame has one, and it belongs in the members list rather than the
            // locals one.
            Verse::FRunningContext Context = StashContext();
            for (const TTuple<Verse::TWriteBarrier<Verse::VUniqueString>, Verse::VValue>& Register : Frame.Registers)
            {
                Verse::VUniqueString* const Name = Register.Key.Get();
                const bool bIsSelf = Name && Name->AsStringView() == FUtf8StringView(UTF8TEXT("Self"));
                if (bIsSelf)
                {
                    ReadMembers(Context, Register.Value, 2, Out.Members);
                    continue;
                }
                FDebugValue& Local = Out.Locals.AddDefaulted_GetRef();
                Local.Name = Name ? FUtf8String(Name->AsStringView()) : FUtf8String();
                ReadDebugValue(Context, Register.Value, 2, Local);
            }
            Out.bValuesRead = true;
        });
}

AUTORTFM_DISABLE void ClearStash()
{
    GStash.PC = nullptr;
    GStash.Frame = nullptr;
    GStash.Task = nullptr;
    GStash.NativeFrame = nullptr;
    GStash.bValid = false;
    GStoppedFrames.Empty();
}

/// The whole of Notify, off the vtable.
///
/// `FDebugger`'s four virtuals are AUTORTFM_ENABLE and an override may not narrow that, so the
/// overrides below are plain forwarders into this -- which is Epic's own shape, where an
/// AUTORTFM_DISABLE VCell holds an inner FDebugger that forwards to it.
AUTORTFM_DISABLE void NotifyImpl(Verse::FRunningContext Context,
                                 const Verse::FOp& PC,
                                 Verse::VFrame& Frame,
                                 Verse::VTask& Task)
{
    const FHostState& Host = GetHost();
    if (!Host.Godot.DebugShouldBreak || !Host.Godot.DebugBreak)
    {
        return;
    }

    // Godot's debug loop keeps servicing the editor while stopped, and flush_output can reach
    // _frame -- which ticks. A tick that resumed a slept task inside a VM stopped mid-op is
    // not a thing any of this is designed for, and this is the guard that makes it impossible
    // rather than merely unlikely.
    if (GStopped)
    {
        return;
    }

    // Epic's order, and each step is why the one below it is affordable.
    Verse::VProcedure& Procedure = *Frame.Procedure;
    Verse::VUniqueString* const FilePath = Procedure.FilePath.Get();
    if (!FilePath || FilePath->Num() == 0)
    {
        return;
    }
    const Verse::FLocation* const Location = Procedure.GetLocation(PC);
    if (!Location)
    {
        return;
    }
    if (!UpdatePrevLocation(Context, Frame, *FilePath, *Location))
    {
        return;
    }

    const FUtf8StringView PathView = FilePath->AsStringView();
    const int32 Relation = FrameRelation(Frame);
    if (!Host.Godot.DebugShouldBreak(Host.Godot.Ctx,
                                     reinterpret_cast<const char*>(PathView.GetData()),
                                     PathView.Len(),
                                     static_cast<int32_t>(Location->Line),
                                     Relation))
    {
        return;
    }

    GLastStoppedFrame.Set(Context, &Frame);

    GStash.PC = &PC;
    GStash.Frame = &Frame;
    GStash.Task = &Task;
    GStash.NativeFrame = Verse::FContext::NativeFrame();
    GStash.bValid = GStash.NativeFrame != nullptr;
    GStoppedFrames.Empty();

    // The consumer is told to stop and nothing else: it is the side that decided *why*, so it
    // is the side that can answer _debug_get_error. The design had the host stash a reason
    // string and hand it over, which would have meant inventing one from a decision made on
    // the other side of the ABI (phase-6-design.md 13.2).
    GStopped = true;
    Host.Godot.DebugBreak(Host.Godot.Ctx);
    GStopped = false;

    ClearStash();
}

struct FGodotDebugger final : Verse::FDebugger
{
    void Notify(Verse::FRunningContext Context,
                const Verse::FOp& PC,
                Verse::VFrame& Frame,
                Verse::VTask& Task) override
    {
        // Already open -- the interpreter that calls this is AUTORTFM_DISABLE, and a handshake is
        // annotated UnreachableIfClosed besides. The wrapper is what lets an AUTORTFM_ENABLE
        // virtual reach a disabled body at all.
        AutoRTFM::Open([&] { NotifyImpl(Context, PC, Frame, Task); });
    }

    void AddLocation(Verse::FAllocationContext, Verse::VUniqueString&, const Verse::FLocation&) override
    {
        // Godot owns the breakpoint list and answers is_breakpoint(line, source) per stop, so
        // there is nothing for the host to record. Epic's socket debugger uses this to arm a
        // breakpoint a client set before the code carrying that line existed; here the question is
        // asked the other way round and always against the live list.
    }

    void AddTask(Verse::FAccessContext, Verse::VTask&) override
    {
        // Godot's debugger models OS threads, and a Verse task is not one. R-ASYNC-7 / OQ-6.
    }

    bool HasConnectedClient() override { return true; }
};

FGodotDebugger GDebugger;
bool GDebuggerInstalled = false;

/// ------------------------------------------------------------------------ the profiler --

/// Relaxed rather than atomic-with-ordering: the only writer is the thread that calls
/// vh_profiling_set_enabled, which is the vh_init thread, and so is every reader.
bool GProfilingEnabled = false;

TMap<FUtf8String, FProfileRow> GAccumulated;
TMap<FUtf8String, FProfileRow> GThisFrame;
TArray<FProfileRow> GReadRows;

/// Built once per procedure. Keyed on the address rather than on the name because a generation is
/// a fresh package: two generations of `player._Process` are two procedures with one name, and
/// their rows should not merge.
TMap<const void*, FUtf8String> GSignatureCache;

/// The boundary crossing currently being timed, so a nested one can subtract itself from it.
FProfileScope* GActiveScope = nullptr;

AUTORTFM_DISABLE void AccumulateRow(FUtf8StringView Signature, double TotalSeconds, double SelfSeconds)
{
    const FUtf8String Key(Signature);
    for (TMap<FUtf8String, FProfileRow>* Table : {&GAccumulated, &GThisFrame})
    {
        FProfileRow& Row = Table->FindOrAdd(Key);
        if (Row.Signature.IsEmpty())
        {
            Row.Signature = Key;
        }
        ++Row.CallCount;
        Row.TotalSeconds += TotalSeconds;
        Row.SelfSeconds += SelfSeconds;
    }
}

/// `profile("tag"){...}` in a script (D13's second half, spike S-5). Exact counts and exact times,
/// opt-in, and separate from the boundary rows so a block nested inside a crossing does not
/// disturb that crossing's own self time.
AUTORTFM_DISABLE void OnEndProfilingEvent(const char* UserTag, double TimeInMs, const FProfileLocus& Locus)
{
    if (!GProfilingEnabled)
    {
        return;
    }
    FUtf8String Signature(FUtf8StringView(GetData(Locus.SnippetPath), Locus.SnippetPath.Len()));
    Signature += UTF8TEXT("::");
    Signature += FUtf8String::Printf(UTF8TEXT("%llu"), (unsigned long long)Locus.BeginRow);
    Signature += UTF8TEXT("::");
    Signature += FUtf8String(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(UserTag ? UserTag : "")));

    const double Seconds = TimeInMs / 1000.0;
    AccumulateRow(Signature, Seconds, Seconds);
}

FDelegateHandle GProfilingHandle;

} // namespace

/// ------------------------------------------------------------------------------ the API --

AUTORTFM_DISABLE bool SetDebugEnabled(bool bEnabled)
{
    if (bEnabled == GDebuggerInstalled)
    {
        return true;
    }

    if (bEnabled)
    {
        // Epic's socket debugger is what vh_init_desc::EnableDebugger installs, and SetDebugger is
        // one global pointer. Refusing rather than overwriting: the one asked for at vh_init is
        // the one the author deliberately chose.
        if (Verse::GetDebugger() != nullptr)
        {
            return false;
        }
        Verse::SetDebugger(&GDebugger);
        // Without this the bit is only sampled when a context is claimed, so a debugger attached
        // mid-run would notify nothing until the next claim.
        Verse::FContext::AttachedDebugger();
        GDebuggerInstalled = true;
        return true;
    }

    if (Verse::GetDebugger() == &GDebugger)
    {
        Verse::SetDebugger(nullptr);
        Verse::FContext::DetachedDebugger();
    }
    GDebuggerInstalled = false;
    ClearStash();
    GStopped = false;
    return true;
}

AUTORTFM_DISABLE bool IsDebugEnabled()
{
    return GDebuggerInstalled;
}

AUTORTFM_DISABLE bool IsDebugStopped()
{
    return GStopped;
}

AUTORTFM_DISABLE bool DebugStackCount(int32& OutCount)
{
    if (!GStash.bValid)
    {
        return false;
    }
    EnsureStackWalked();
    OutCount = GStoppedFrames.Num();
    return true;
}

AUTORTFM_DISABLE bool DebugStackFrame(int32 Level, FUtf8String& OutPath, FUtf8String& OutName, int32& OutLine)
{
    if (!GStash.bValid)
    {
        return false;
    }
    EnsureStackWalked();
    if (!GStoppedFrames.IsValidIndex(Level))
    {
        return false;
    }
    const FStoppedFrame& Frame = GStoppedFrames[Level];
    OutPath = Frame.Path;
    OutName = Frame.Name;
    OutLine = Frame.Line;
    return true;
}

AUTORTFM_DISABLE const TArray<FDebugValue>* DebugStackValues(int32 Level, bool bMembers)
{
    if (!GStash.bValid)
    {
        return nullptr;
    }
    EnsureStackWalked();
    if (!GStoppedFrames.IsValidIndex(Level))
    {
        return nullptr;
    }
    const FStoppedFrame& Frame = GStoppedFrames[Level];
    return bMembers ? &Frame.Members : &Frame.Locals;
}

AUTORTFM_DISABLE void SetProfilingEnabled(bool bEnabled)
{
    if (bEnabled == GProfilingEnabled)
    {
        return;
    }
    GProfilingEnabled = bEnabled;
    if (bEnabled)
    {
        if (!GProfilingHandle.IsValid())
        {
            GProfilingHandle = FVerseProfilingDelegates::OnEndProfilingEvent.AddStatic(&OnEndProfilingEvent);
        }
    }
    else if (GProfilingHandle.IsValid())
    {
        FVerseProfilingDelegates::OnEndProfilingEvent.Remove(GProfilingHandle);
        GProfilingHandle.Reset();
    }
}

AUTORTFM_DISABLE bool IsProfilingEnabled()
{
    return GProfilingEnabled;
}

AUTORTFM_DISABLE void ResetProfile()
{
    GAccumulated.Empty();
    GThisFrame.Empty();
    GReadRows.Empty();
    GSignatureCache.Empty();
}

AUTORTFM_DISABLE void ReadProfile(bool bFrameOnly, TArray<FProfileRow>& OutRows)
{
    TMap<FUtf8String, FProfileRow>& Table = bFrameOnly ? GThisFrame : GAccumulated;
    OutRows.Empty(Table.Num());
    for (const TPair<FUtf8String, FProfileRow>& Pair : Table)
    {
        OutRows.Add(Pair.Value);
    }
    if (bFrameOnly)
    {
        GThisFrame.Empty();
    }
}

AUTORTFM_DISABLE FUtf8StringView ProfileSignature(Verse::VFunction* Function, FUtf8StringView Owner)
{
    if (!GProfilingEnabled || !Function || !Function->HasProcedure())
    {
        return FUtf8StringView();
    }
    Verse::VProcedure* const Procedure = Function->Procedure.Get().DynamicCast<Verse::VProcedure>();
    if (!Procedure)
    {
        return FUtf8StringView();
    }

    if (const FUtf8String* const Cached = GSignatureCache.Find(Procedure))
    {
        return FUtf8StringView(*Cached);
    }

    Verse::VUniqueString* const Path = Procedure->FilePath.Get();
    if (!Path || Path->Num() == 0)
    {
        // A mirror function. The codegen only fills FilePath for a package with source behind it,
        // so there is nothing to name a row after -- and a row named after a Godot method would be
        // claiming a time the bridge does not measure.
        return FUtf8StringView();
    }

    const Verse::FLocation* const First = Procedure->GetLocation(0);
    FUtf8String Signature(Path->AsStringView());
    Signature += UTF8TEXT("::");
    Signature += FUtf8String::Printf(UTF8TEXT("%d"), First ? static_cast<int32>(First->Line) : 0);
    Signature += UTF8TEXT("::");
    if (!Owner.IsEmpty())
    {
        Signature += FUtf8String(Owner);
        Signature += UTF8TEXT(".");
    }
    if (Verse::VUniqueString* const Name = Procedure->Name.Get())
    {
        Signature += FUtf8String(Undecorate(Name->AsStringView()));
    }

    return FUtf8StringView(GSignatureCache.Add(Procedure, MoveTemp(Signature)));
}

AUTORTFM_DISABLE FProfileScope::FProfileScope(FUtf8StringView InSignature)
{
    if (!GProfilingEnabled || InSignature.IsEmpty())
    {
        return;
    }
    bActive = true;
    Signature = InSignature;
    Started = FPlatformTime::Seconds();
    Parent = GActiveScope;
    GActiveScope = this;
}

AUTORTFM_DISABLE FProfileScope::~FProfileScope()
{
    if (!bActive)
    {
        return;
    }
    const double Total = FPlatformTime::Seconds() - Started;
    GActiveScope = Parent;
    if (Parent)
    {
        Parent->ChildSeconds += Total;
    }
    AccumulateRow(Signature, Total, FMath::Max(Total - ChildSeconds, 0.0));
}

} // namespace GodotVerse
