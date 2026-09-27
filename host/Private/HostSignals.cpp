// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostSignals.h"

#include "GodotClasses.h"
#include "HostCallbacks.h"
#include "HostMarshal.h"
#include "HostRuntime.h"
#include "HostScript.h"
#include "HostScriptState.h"
#include "HostTypeModel.h"
#include "HostVerseEntry.h"
#include "ULangUEUtils.h"
#include "UObject/StrongObjectPtr.h"
#include "VerseContentScope.h"
#include "VerseEvent.h"
#include "VerseValue.h"
#include "VerseVM/Inline/VVMRefInline.h"
#include "VerseVM/Inline/VVMValueInline.h"
#include "VerseVM/Inline/VVMValueObjectInline.h"
#include "VerseVM/Inline/VVMVerseClassInline.h"
#include "VerseVM/VVMArray.h"
#include "VerseVM/VVMContext.h"
#include "VerseVM/VVMFunction.h"
#include "VerseVM/VVMNativeRef.h"
#include "VerseVM/VVMOpResult.h"
#include "VerseVM/VVMRef.h"
#include "VerseVM/VVMShape.h"
#include "VerseVM/VVMUniqueString.h"
#include "VerseVM/VVMValueObject.h"
#include "VerseVM/VVMVerseClass.h"
#include "uLang/Semantics/SemanticClass.h"
#include "uLang/Semantics/SemanticFunction.h"
#include "uLang/Semantics/SemanticProgram.h"
#include "uLang/Semantics/SemanticTypes.h"
#include "verse_diagnostics.h"

#include <string>

using GodotVerse::EnterVerse;
using GodotVerse::EPayloadShape;
using GodotVerse::FCallbackTarget;
using GodotVerse::FindReferenceClass;
using GodotVerse::FMemberType;
using GodotVerse::FPayloadArg;
using GodotVerse::FPayloadShape;
using GodotVerse::NewReferenceWrapper;
using GodotVerse::ValueToWire;
using GodotVerse::WireToValue;

namespace {

/// One `signal` member of one live instance: everything the member's *type* and *name* said,
/// resolved once at construction so neither has to be spelled again.
struct FSignalBinding
{
    int64 OwnerHandle = 0;
    FUtf8String Name;
    /// What the payload decomposes into -- the same shape the signal descriptor reported to Godot,
    /// so the arguments a handler was generated for and the arguments an emission carries cannot
    /// disagree.
    FPayloadShape Payload;
    /// vh_signal_reject, copied off the descriptor. A rejected signal is still bound, so that
    /// emitting it can say the reason the editor said rather than the generic "names nothing" --
    /// which is what a script running outside the editor gets, and it was the whole complaint.
    int32 Reject = VH_SIGNAL_OK;
    FUtf8String RejectDetail;

    /// The `event(t)` an `@export_signal` member holds, and the connection that feeds it.
    ///
    /// Null for a `signal(t)`, whose event is reached through the signal object at each await and
    /// whose connection lives exactly as long as that wait. An `event(t)` has no such hook -- its
    /// `Await` is Verse's own native -- so the connection is made once here and held for the
    /// instance's life, which is the one place R-SIG-5's connect-while-awaiting is traded away.
    ///
    /// Held strongly because it is what an emission is delivered into, and dropped at
    /// ReleaseInstance: a strong pointer kept past the node would be a GC root per scripted node,
    /// which is the shape of leak that looks entirely normal in a working scene.
    TStrongObjectPtr<UObject> Event;
    int64 CallableRef = 0;
    int64 CallbackId = 0;
};

TMap<int64, FSignalBinding> GSignalBindings;
int64 GNextSignalId = 1;

/// The `event(t)` an `@export_signal` member holds -> its binding id.
///
/// A `signal(t)` needs no such table: the row's id is written into the object's own `Id` field at
/// bind time, which is what the native class exists for. `event(t)` is Verse's own and cannot be
/// reopened to carry one, so the object *is* the key -- which is also why the binding holds it
/// strongly. The raw pointer is safe for exactly as long as that strong pointer is, and both are
/// dropped together at ReleaseInstance.
TMap<const UObject*, int64> GEventBindingIds;

/// One live connection, which is what a `connection` names.
struct FSubscription
{
    int64 OwnerHandle = 0;
    FUtf8String Name;
    /// The reference id of the Callable Godot holds. Released when the subscription is cancelled,
    /// which is also what drops the host's claim on the callback behind it.
    int64 CallableRef = 0;
};

TMap<int64, FSubscription> GSubscriptions;
int64 GNextSubscriptionId = 1;

/// One task waiting on one Godot signal (R-SIG-5).
///
/// A wait is a connection the host owns for exactly as long as the wait lasts. What it feeds is a
/// `/Verse.org/Verse` `event(t)` living on the `signal` object the script awaited -- which is
/// why the object is held here rather than only its binding id: an engine-signal accessor mints a
/// *fresh* `signal` on every call, several of them share one binding, and only the object
/// says which event a given wait is suspended on.
struct FAwaiter
{
    /// Held strongly: `Timer.Timeout().Await()` awaits a temporary, and nothing else on the Verse
    /// side has to outlive the statement that made it.
    TStrongObjectPtr<UObject> Waiter;
    /// The binding whose payload shape says how to put the emission's arguments back together.
    /// Zero for a foreign signal, whose arguments become one Godot Array instead.
    int64 SignalId = 0;
    /// What to disconnect from, and what to disconnect with.
    int64 OwnerHandle = 0;
    FUtf8String Name;
    int64 CallableRef = 0;
    /// The callback id the Callable carries, so ending the wait drops its row too.
    int64 CallbackId = 0;

    /// The scope the wait was started in, and the cleanup it registered there.
    ///
    /// This is what ends a wait that never resumes *and* never runs its `defer`: terminating a
    /// task group does not unwind the tasks in it, so a node freed while awaiting would otherwise
    /// leave a live Godot connection, a held object and a callback row behind. Epic's own rule, and
    /// their own hook -- `event::SubscribeInternal` drops a subscription the same way
    /// (`VerseEvent.cpp:139-178`). `phase-4-gaps.md` G9 is this.
    TWeakPtr<verse::FContentScope> Scope;
    FDelegateHandle Cleanup;
};

TMap<int64, FAwaiter> GAwaiters;
int64 GNextAwaitToken = 1;

/// "<handle>:<signal>" -> the binding id, so an accessor called twice on one object answers the
/// same row. A row is never dropped: the ids are small, and nothing tells the host that Godot has
/// freed an object.
TMap<FUtf8String, int64> GEngineSignalIds;

/// "<class>.<accessor>" -> a binding holding only the payload shape, which is the expensive half:
/// every Timer's `timeout` carries the same nothing, and the lookup walks the semantic program.
TMap<FUtf8String, FSignalBinding> GEngineSignalShapes;

/// The same table, recorded by the cook and read back out of the sidecar. What a runtime host has
/// instead of a semantic program to walk.
TSharedPtr<GodotVerse::FEngineSignalTypes> GRecordedEngineSignals;

std::string_view DiagArg(const FUtf8String& Text)
{
    return std::string_view(reinterpret_cast<const char*>(*Text), (size_t)Text.Len());
}

/// Reports one of the registry's sentences (include/verse_diagnostics.def), the one vm/ and src/
/// print for the same thing.
AUTORTFM_DISABLE void ReportVerseDiag(verse_diag Id, std::initializer_list<verse_diag_arg> Args = {})
{
    const std::string Text = verse_diag_text(Id, Args);
    GodotVerse::ReportError(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text.data()), (int32)Text.size()));
}

/// Why a signal was refused: the VG20xx sentence the editor shows at the member's own line, which
/// is what a game running outside the editor has instead of that line.
AUTORTFM_DISABLE std::string SignalRejectReason(const FUtf8String& Name, int32 Reject, const FUtf8String& Detail)
{
    return verse_signal_rejection(DiagArg(Name), Reject, DiagArg(Detail));
}

/// Defined below, beside the await it was factored out of. Declared here because an
/// `@export_signal` member's connection is made at bind time rather than at a wait.
AUTORTFM_DISABLE GodotVerse::TResult<int64> ConnectDelivery(int64 OwnerHandle,
                                                            const FUtf8String& Name,
                                                            FCallbackTarget Target,
                                                            int32 ConnectFlags,
                                                            int64& OutCallableRef);

/// Whether a reason is worth VH_UNREPORTED where a native drops it for a bare 0. CallbackMissing
/// is not: a consumer without Godot's callbacks -- tests/verse_probe, host_smoke -- takes these
/// paths on purpose, and the line would land in every probe transcript the contract layer compares.
bool WorthReporting(GodotVerse::EHostFailure Failure)
{
    return Failure != GodotVerse::EHostFailure::CallbackMissing;
}

} // namespace

AUTORTFM_DISABLE void GodotVerse::BindSignals(UObject* Instance,
                                              FUtf8StringView ClassName,
                                              int64 Handle,
                                              TArray<int64>& OutEventBindings)
{
    TArray<GodotVerse::FSignalDesc> Signals;
    if (!GodotVerse::GetClassSignals(ClassName, Signals))
    {
        VH_UNREPORTED("BindSignals: the class being instantiated has no snapshot row");
        return;
    }
    if (Signals.IsEmpty())
    {
        return;
    }

    const uLang::CSemanticProgram* const Program = CurrentSemanticProgram();
    const TMap<FUtf8String, FPayloadShape>* const Recorded = Program ? nullptr : RecordedSignalShapes(ClassName);
    if (!Program && !Recorded)
    {
        VH_UNREPORTED("BindSignals: no semantic program and no recorded payloads for the class");
        return;
    }

    for (const GodotVerse::FSignalDesc& Signal : Signals)
    {
        UObject* const Held = PeekFieldObject(Instance, FUtf8StringView(Signal.Name));
        verse::vh_signal* const Shadow = Cast<verse::vh_signal>(Held);
        verse::event* const Event = Shadow ? nullptr : Cast<verse::event>(Held);
        if (!Shadow && !Event)
        {
            // A declared signal whose member holds nothing. Saying so beats emitting into the void
            // later, which is what an unbound id does.
            GodotVerse::ReportError(FUtf8String(UTF8TEXT("The signal `")) + Signal.Name
                + UTF8TEXT("` on ") + FUtf8String(ClassName)
                + UTF8TEXT(" could not be bound: its member holds no signal object."));
            continue;
        }

        const FMemberType Declared = DescribeMemberType(ClassName, FUtf8StringView(Signal.Name));
        FSignalBinding Binding;
        Binding.OwnerHandle = Handle;
        Binding.Name = Signal.Name;
        Binding.Reject = Signal.Reject;
        Binding.RejectDetail = Signal.RejectDetail;
        if (Program && Declared.ReferenceClass)
        {
            DescribePayload(SignalPayloadType(*Declared.ReferenceClass), *Program, Binding.Payload);
        }
        else if (Recorded)
        {
            if (const FPayloadShape* const Shape = Recorded->Find(Signal.Name))
            {
                Binding.Payload = *Shape;
            }
        }

        const int64 Id = GNextSignalId++;
        if (Shadow)
        {
            GSignalBindings.Add(Id, MoveTemp(Binding));
            Shadow->Id.Init(Id, Shadow);
            continue;
        }

        // An `@export_signal` event member. There is no `Id` field to write -- `event(t)` is
        // Verse's own class and cannot be reopened -- so the binding holds the event instead, and
        // the emit and subscribe natives find the row by the object they are handed.
        Binding.Event = TStrongObjectPtr<UObject>(Held);
        const bool bRegistered = Binding.Reject == VH_SIGNAL_OK;
        GSignalBindings.Add(Id, MoveTemp(Binding));
        GEventBindingIds.Add(Held, Id);
        OutEventBindings.Add(Id);

        // The connection is *not* made here. This runs inside vh_instantiate, which the consumer
        // calls before it installs the script instance on the object, so Godot does not yet know
        // the script has a signal of this name and answers "Attempt to connect nonexistent signal".
        // AttachInstance below is the hook that runs once it does.
    }
}

AUTORTFM_DISABLE int64 GodotVerse::BindEngineSignal(int64 Handle,
                                                    FUtf8StringView ClassName,
                                                    FUtf8StringView AccessorName,
                                                    FUtf8StringView SignalName)
{
    // One row per (owner, signal). An accessor is a method, so it runs on every `Timer.Timeout()`
    // -- minting per call would grow the table for as long as the game does.
    const FUtf8String Key = FUtf8String::FromInt(Handle) + UTF8TEXT(":") + FUtf8String(SignalName);
    if (const int64* Existing = GEngineSignalIds.Find(Key))
    {
        return *Existing;
    }

    FSignalBinding Binding;
    Binding.OwnerHandle = Handle;
    Binding.Name = FUtf8String(SignalName);

    // The payload, off the accessor's own return type, cached per accessor rather than per object:
    // every Timer's `timeout` carries the same nothing.
    const FUtf8String Shape = FUtf8String(ClassName) + UTF8TEXT(".") + FUtf8String(AccessorName);
    if (const FSignalBinding* Cached = GEngineSignalShapes.Find(Shape))
    {
        Binding.Payload = Cached->Payload;
    }
    else if (const uLang::CSemanticProgram* const Program = CurrentSemanticProgram())
    {
        const FUtf8String Path = FUtf8String(GodotVersePath) + UTF8TEXT("/") + FUtf8String(ClassName);
        if (const uLang::CClass* const Mirrored = Program->FindDefinitionByVersePath<uLang::CClass>(
                FULangConversionUtils::FUtf8StringViewToULangStringView(Path)))
        {
            for (const uLang::TSRef<uLang::CFunction>& Function : Mirrored->GetDefinitionsOfKind<uLang::CFunction>())
            {
                if (!FUtf8StringView(Function->AsNameCString()).Equals(AccessorName))
                {
                    continue;
                }
                const uLang::CFunctionType* const Type = Function->_Signature.GetFunctionType();
                bool bIsOption = false;
                const uLang::CNormalType* const Returned =
                    Type ? &UnwrapDeclaredType(Type->GetReturnType(), bIsOption) : nullptr;
                if (const uLang::CClass* const Signal = Returned ? Returned->AsNullable<uLang::CClass>() : nullptr)
                {
                    DescribePayload(SignalPayloadType(*Signal), *Program, Binding.Payload);
                }
                break;
            }
        }
        GEngineSignalShapes.Add(Shape, Binding);
    }
    else if (GRecordedEngineSignals)
    {
        if (const FPayloadShape* const Recorded = GRecordedEngineSignals->Shapes.Find(Shape))
        {
            Binding.Payload = *Recorded;
        }
        GEngineSignalShapes.Add(Shape, Binding);
    }

    const int64 Id = GNextSignalId++;
    GSignalBindings.Add(Id, MoveTemp(Binding));
    GEngineSignalIds.Add(Key, Id);
    return Id;
}

AUTORTFM_DISABLE void GodotVerse::EmitSignal(int64 SignalId, const FVerseValue& Payload)
{
    const FSignalBinding* const Binding = GSignalBindings.Find(SignalId);
    if (!Binding)
    {
        ReportVerseDiag(verse_diag::VG2105);
        return;
    }

    // A signal the editor already refused. Saying so again here is not redundant: the editor
    // warning is the only report a *tools* build makes, and a game running outside it would
    // otherwise get the generic "names nothing" for a member that was declared perfectly visibly.
    if (Binding->Reject != VH_SIGNAL_OK)
    {
        const std::string Reason = SignalRejectReason(Binding->Name, Binding->Reject, Binding->RejectDetail);
        ReportVerseDiag(verse_diag::VG2101, {{"signal", DiagArg(Binding->Name)}, {"reason", Reason}});
        return;
    }

    FHostState& Host = GetHost();
    if (!Host.Godot.EmitSignal)
    {
        return;
    }

    // One storage per argument: FFieldStorage carries a single Text, so two string arguments
    // sharing one would clobber each other.
    const FPayloadShape& Shape = Binding->Payload;
    const int32 Count = Shape.Args.Num();
    TArray<FFieldStorage> Storages;
    Storages.SetNum(Count);
    TArray<vh_value> Args;
    Args.SetNum(Count);

    bool bConverted = true;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    EnterVerse(Context, [&] {
        const Verse::VValue Value = Payload.GetValue();
        const Verse::VArrayBase* const Tuple = Shape.Kind == EPayloadShape::Tuple
            ? Value.DynamicCast<Verse::VArrayBase>()
            : nullptr;
        Verse::VValueObject* const Struct = Shape.Kind == EPayloadShape::Struct
            ? Value.DynamicCast<Verse::VValueObject>()
            : nullptr;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const FPayloadArg& Arg = Shape.Args[Index];
            Verse::VValue Element;
            switch (Shape.Kind)
            {
            case EPayloadShape::Tuple:
                // A Verse tuple is an array at runtime, and its elements are the arguments Godot sees.
                Element = Tuple && Index < (int32)Tuple->Num() ? Tuple->GetValue((uint32)Index) : Verse::VValue();
                break;
            case EPayloadShape::Struct:
            {
                // By name rather than by position, the way ReadStructComponents reads a mirrored
                // math type: a struct value carries its fields under decorated keys and nothing in
                // it says what order the declaration wrote them in.
                if (!Struct)
                {
                    bConverted = false;
                    return;
                }
                Verse::VUniqueString& Key = Verse::VUniqueString::New(Context, FUtf8StringView(Arg.FieldKey));
                const Verse::FOpResult Read = Struct->LoadField(Context, Key);
                if (!Read.IsReturn())
                {
                    bConverted = false;
                    return;
                }
                Element = Read.Value;
                break;
            }
            case EPayloadShape::Bare:
                Element = Value;
                break;
            }

            if (!ValueToWire(Context, Element, Arg.Type, Storages[Index], Args[Index]))
            {
                bConverted = false;
                return;
            }
        }
    });

    if (!bConverted)
    {
        ReportVerseDiag(verse_diag::VG2104, {{"signal", DiagArg(Binding->Name)}});
        return;
    }

    Host.Godot.EmitSignal(Host.Godot.Ctx,
                          Binding->OwnerHandle,
                          reinterpret_cast<const char*>(*Binding->Name),
                          Binding->Name.Len(),
                          Args.GetData(),
                          Args.Num());
}

AUTORTFM_DISABLE int64 GodotVerse::SubscribeSignal(int64 SignalId, const FVerseValue& Callback)
{
    const FSignalBinding* const Binding = GSignalBindings.Find(SignalId);
    if (!Binding)
    {
        ReportVerseDiag(verse_diag::VG2106);
        return 0;
    }

    // Same reason as the emission half: Godot was never told this signal exists, so `connect` would
    // refuse the name, and "connect failed" is a worse sentence than the one that says why.
    if (Binding->Reject != VH_SIGNAL_OK)
    {
        const std::string Reason = SignalRejectReason(Binding->Name, Binding->Reject, Binding->RejectDetail);
        ReportVerseDiag(verse_diag::VG2102, {{"signal", DiagArg(Binding->Name)}, {"reason", Reason}});
        return 0;
    }

    FHostState& Host = GetHost();
    if (!Host.Godot.ConnectSignal || !Host.Godot.ReleaseRef)
    {
        return 0;
    }

    const int64 CallableRef = MakeCallableFor(Callback);
    if (CallableRef == 0)
    {
        VH_UNREPORTED("SubscribeSignal: MakeCallableFor answered no Callable");
        return 0;
    }

    vh_value Target{};
    Target.Type = VH_TYPE_REF;
    Target.VariantTag = VH_VARIANT_CALLABLE;
    Target.Ref = CallableRef;

    const int64 OwnerHandle = Binding->OwnerHandle;
    const FUtf8String Name = Binding->Name;
    const int32 Status = Host.Godot.ConnectSignal(
        Host.Godot.Ctx, OwnerHandle, reinterpret_cast<const char*>(*Name), Name.Len(), &Target, 0);
    if (Status != VH_CALL_OK)
    {
        Host.Godot.ReleaseRef(Host.Godot.Ctx, CallableRef);
        VH_UNREPORTED("SubscribeSignal: Godot refused the connection");
        return 0;
    }

    const int64 Id = GNextSubscriptionId++;
    GSubscriptions.Add(Id, FSubscription{OwnerHandle, Name, CallableRef});

    // Compensated rather than deferred. This mutates Godot *and* returns a value, so it can be
    // neither queued for commit nor ignored -- and without the compensation a failed transaction
    // leaves a live connection the script believes it never made. The host's only rollback
    // compensation, and the shape to copy for anything later that mutates Godot and cannot defer.
    //
    // **Not `Verse::Stm::OnRollback`**, which is what Phase 4 wrote and what Phase 4.5's S-3
    // measured as doing nothing: that is the Solaris *interpreter's* STM, and `VerseStm.h` says of
    // it "Noop if StmActive() returns false" -- StmActive being "true if in a failure context",
    // which is a BPVM-era notion VerseVM never sets from here. The connection survived all three
    // kinds of failure.
    //
    // `SameAsClosed` is the load-bearing half. Every Godot callback reaches C++ through
    // `AutoRTFM::Open` (see VhSignalSubscribe), and a plain `OnAbort` from open code is documented
    // to be *ignored*; `SameAsClosed` registers it against the active transaction as if the call
    // had been closed, which is the one spelling that survives the Open this call is inside.
    AutoRTFM::OnAbort<AutoRTFM::EOpenBehavior::SameAsClosed>(
        [Id] { GodotVerse::CancelSubscription(Id); });
    return Id;
}

namespace {
/// The binding an `@export_signal` member's event names, or 0.
///
/// 0 means the member was never bound: a `signal(t)` a script built for itself answers the same
/// way, and both reach the "names nothing" sentence rather than silently emitting into the void.
AUTORTFM_DISABLE int64 EventBindingFor(UObject* Event)
{
    if (!Event)
    {
        return 0;
    }
    const int64* const Found = GEventBindingIds.Find(Event);
    return Found ? *Found : 0;
}
}

AUTORTFM_DISABLE void GodotVerse::EmitEventSignal(UObject* Event, const FVerseValue& Payload)
{
    const int64 SignalId = EventBindingFor(Event);
    if (SignalId == 0)
    {
        ReportVerseDiag(verse_diag::VG2107);
        return;
    }
    EmitSignal(SignalId, Payload);
}

AUTORTFM_DISABLE int64 GodotVerse::SubscribeEventSignal(UObject* Event, const FVerseValue& Callback)
{
    const int64 SignalId = EventBindingFor(Event);
    if (SignalId == 0)
    {
        ReportVerseDiag(verse_diag::VG2108);
        return 0;
    }
    return SubscribeSignal(SignalId, Callback);
}

AUTORTFM_DISABLE void GodotVerse::CancelSubscription(int64 SubscriptionId)
{
    const FSubscription* const Found = GSubscriptions.Find(SubscriptionId);
    if (!Found)
    {
        // Idempotent, as event_subscription::Cancel is in UEFN: a second Cancel does nothing and
        // says nothing.
        return;
    }
    const FSubscription Subscription = *Found;
    GSubscriptions.Remove(SubscriptionId);

    GodotVerse::FHostState& Host = GodotVerse::GetHost();
    if (Host.Godot.DisconnectSignal)
    {
        vh_value Target{};
        Target.Type = VH_TYPE_REF;
        Target.VariantTag = VH_VARIANT_CALLABLE;
        Target.Ref = Subscription.CallableRef;
        Host.Godot.DisconnectSignal(Host.Godot.Ctx,
                                    Subscription.OwnerHandle,
                                    reinterpret_cast<const char*>(*Subscription.Name),
                                    Subscription.Name.Len(),
                                    &Target);
    }
    if (Host.Godot.ReleaseRef)
    {
        Host.Godot.ReleaseRef(Host.Godot.Ctx, Subscription.CallableRef);
    }
}

namespace {

/// The `/Verse.org/Verse` event a `signal` holds, read off the object rather than named.
///
/// Found by walking the shape rather than by building the field's decorated key. The key of a data
/// member is `(<declaring class' scope path>:)<name>`, and for a member of a *parametric* class
/// there is more than one plausible spelling of that path -- so the walk asks the only question
/// that cannot be got wrong: which field holds an event.
AUTORTFM_DISABLE verse::event* FindEventField(Verse::FRunningContext Context, UObject* Object)
{
    if (!Object)
    {
        return nullptr;
    }
    Verse::VShape& Shape = UVerseClass::GetShapeForLoadField(Context, Object->GetClass());
    for (Verse::VShape::FieldsMap::TIterator It = Shape.CreateFieldsIterator(); It; ++It)
    {
        const Verse::VShape::VEntry& Entry = It.Value();
        Verse::VValue Value = Entry.Type == Verse::EFieldType::FPropertyVar
            ? Verse::VNativeRef::Peek(Context, Object, Entry.UProperty)
            : UVerseClass::PeekField(Context, Object, &Entry);
        if (Verse::VRef* Ref = Value.DynamicCast<Verse::VRef>())
        {
            Value = Ref->Get(Context);
        }
        if (verse::event* const Event = Cast<verse::event>(Value.ExtractUObject()))
        {
            return Event;
        }
    }
    return nullptr;
}

/// A Godot Array holding an emission's arguments, as the reference wrapper a `godot_array` is.
///
/// The one payload a foreign signal can carry: nothing declares its arguments, so there is no
/// per-argument type to convert against and the whole list crosses as the container Godot itself
/// would have put them in. Ownership of the fresh reference passes to the wrapper, whose
/// BeginDestroy releases it when Verse drops the value.
AUTORTFM_DISABLE GodotVerse::TResult<void> ArgumentArrayValue(Verse::FRunningContext Context,
                                                              const vh_value* Args,
                                                              int32 ArgCount,
                                                              Verse::VValue& OutValue)
{
    GodotVerse::FHostState& Host = GodotVerse::GetHost();
    if (!Host.Godot.NewRef || !Host.Godot.RefSet)
    {
        return GodotVerse::EHostFailure::CallbackMissing;
    }
    const int64 Ref = Host.Godot.NewRef(Host.Godot.Ctx, VH_VARIANT_ARRAY);
    if (Ref == 0)
    {
        return GodotVerse::EHostFailure::CallbackFailed;
    }
    for (int32 Index = 0; Index < ArgCount; ++Index)
    {
        vh_value Key{};
        Key.Type = VH_TYPE_INT;
        Key.Int = Index;
        Host.Godot.RefSet(Host.Godot.Ctx, Ref, &Key, &Args[Index]);
    }
    const GodotVerse::TResult<UObject*> Wrapper = NewReferenceWrapper(FindReferenceClass(VH_VARIANT_ARRAY), Ref);
    if (!Wrapper)
    {
        if (Host.Godot.ReleaseRef)
        {
            Host.Godot.ReleaseRef(Host.Godot.Ctx, Ref);
        }
        return Wrapper.GetFailure();
    }
    OutValue = Verse::VValue(Wrapper.GetValue());
    return GodotVerse::TResult<void>::Ok();
}

/// Puts an emission's arguments back together as the one value the payload's type names.
///
/// The exact inverse of what DescribePayload took apart, and it has to be: `Await` answers `t`,
/// and `t` is what the declaration said rather than the argument list Godot carried.
AUTORTFM_DISABLE GodotVerse::TResult<void> PayloadValue(Verse::FRunningContext Context,
                                                        const FPayloadShape& Shape,
                                                        const vh_value* Args,
                                                        int32 ArgCount,
                                                        Verse::VValue& OutValue)
{
    switch (Shape.Kind)
    {
    case EPayloadShape::Bare:
        if (Shape.Args.Num() != 1 || ArgCount != 1)
        {
            return GodotVerse::EHostFailure::TypeMismatch;
        }
        return WireToValue(Context, Args[0], Shape.Args[0].Type, OutValue);

    case EPayloadShape::Tuple:
    {
        // A Verse tuple is an array at runtime, and a `tuple()` payload is an empty one -- which is
        // what an engine signal carrying nothing answers, and the commonest case there is.
        if (ArgCount != Shape.Args.Num())
        {
            return GodotVerse::EHostFailure::TypeMismatch;
        }
        TArray<Verse::VValue> Elements;
        Elements.Reserve(ArgCount);
        for (int32 Index = 0; Index < ArgCount; ++Index)
        {
            Verse::VValue Element;
            const GodotVerse::TResult<void> Converted = WireToValue(Context, Args[Index], Shape.Args[Index].Type, Element);
            if (!Converted)
            {
                return Converted;
            }
            Elements.Add(Element);
        }
        const auto Init = [&Elements](uint32 Index) { return Elements[(int32)Index]; };
        OutValue = Verse::VValue(Verse::VArray::New(Context, (uint32)Elements.Num(), Init));
        return GodotVerse::TResult<void>::Ok();
    }

    case EPayloadShape::Struct:
    {
        // The same rule InstanceCall applies to a struct parameter: N Godot arguments satisfy one
        // struct of N fields, and WireToValue is what builds it. Borrowed for the call, like every
        // other pointer on this wire.
        vh_value Packed{};
        Packed.Type = VH_TYPE_TUPLE;
        Packed.Seq.Items = Args;
        Packed.Seq.Count = ArgCount;
        return WireToValue(Context, Packed, Shape.Whole, OutValue);
    }
    }
    return GodotVerse::EHostFailure::Unconvertible;
}

} // namespace

AUTORTFM_DISABLE GodotVerse::TResult<void> GodotVerse::DeliverToAwaiter(int64 Token, const vh_value* Args, int32 ArgCount)
{
    const FAwaiter* const Found = GAwaiters.Find(Token);
    if (!Found)
    {
        // The wait ended between Godot queueing the emission and delivering it. Not an error: a
        // cancelled task is exactly a wait that stopped waiting.
        return TResult<void>::Ok();
    }
    UObject* const Waiter = Found->Waiter.Get();
    const int64 SignalId = Found->SignalId;
    const FPayloadShape* const Shape = SignalId != 0
        ? (GSignalBindings.Contains(SignalId) ? &GSignalBindings[SignalId].Payload : nullptr)
        : nullptr;
    if (!Waiter)
    {
        return EHostFailure::NotASignal;
    }
    if (SignalId != 0 && !Shape)
    {
        return EHostFailure::UnknownId;
    }

    TResult<void> Result = TResult<void>::Ok();
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    const AutoRTFM::ETransactionResult TransactionResult = AutoRTFM::Transact([&] {
        AutoRTFM::Open([&] {
            EnterVerse(Context, [&] {
                verse::event* const Event = FindEventField(Context, Waiter);
                if (!Event)
                {
                    Result = EHostFailure::NotASignal;
                    return;
                }
                Verse::VValue Payload;
                const TResult<void> Built = Shape ? PayloadValue(Context, *Shape, Args, ArgCount, Payload)
                                                  : ArgumentArrayValue(Context, Args, ArgCount, Payload);
                if (!Built)
                {
                    Result = Built;
                    return;
                }
                // event::Signal resumes the suspended awaits in FIFO order, under each task's own
                // content scope, skipping any whose scope was terminated -- Epic's code, and the
                // reason `Await` needed no scheduler of its own.
                Event->Signal(FVerseValue(Payload));
            });
        });
    });
    if (TransactionResult != AutoRTFM::ETransactionResult::Committed)
    {
        return EHostFailure::Aborted;
    }
    return Result;
}

AUTORTFM_DISABLE GodotVerse::TResult<void> GodotVerse::DeliverToEvent(int64 SignalId, const vh_value* Args, int32 ArgCount)
{
    const FSignalBinding* const Binding = GSignalBindings.Find(SignalId);
    if (!Binding)
    {
        return EHostFailure::UnknownId;
    }
    UObject* const Held = Binding->Event.Get();
    if (!Held)
    {
        // The instance was released between Godot queueing the emission and delivering it, which
        // is the event-member analogue of a wait that stopped waiting.
        return TResult<void>::Ok();
    }
    const FPayloadShape Shape = Binding->Payload;

    TResult<void> Result = TResult<void>::Ok();
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    const AutoRTFM::ETransactionResult TransactionResult = AutoRTFM::Transact([&] {
        AutoRTFM::Open([&] {
            EnterVerse(Context, [&] {
                verse::event* const Event = Cast<verse::event>(Held);
                if (!Event)
                {
                    Result = EHostFailure::NotASignal;
                    return;
                }
                Verse::VValue Payload;
                const TResult<void> Built = PayloadValue(Context, Shape, Args, ArgCount, Payload);
                if (!Built)
                {
                    Result = Built;
                    return;
                }
                Event->Signal(FVerseValue(Payload));
            });
        });
    });
    if (TransactionResult != AutoRTFM::ETransactionResult::Committed)
    {
        return EHostFailure::Aborted;
    }
    return Result;
}

namespace {

/// Mints the Callable an await or a foreign subscription is delivered through, and connects it.
///
/// Answers the callback id, with OutCallableRef holding the reference Godot keeps. Fails for a
/// connection Godot refused, having released whatever it had minted.
AUTORTFM_DISABLE GodotVerse::TResult<int64> ConnectDelivery(int64 OwnerHandle,
                                                            const FUtf8String& Name,
                                                            FCallbackTarget Target,
                                                            int32 ConnectFlags,
                                                            int64& OutCallableRef)
{
    OutCallableRef = 0;
    GodotVerse::FHostState& Host = GodotVerse::GetHost();
    if (!Host.Godot.MakeCallable || !Host.Godot.ConnectSignal || !Host.Godot.ReleaseRef)
    {
        return GodotVerse::EHostFailure::CallbackMissing;
    }
    if (OwnerHandle == 0)
    {
        return GodotVerse::EHostFailure::UnknownId;
    }

    const int64 CallbackId = GodotVerse::AddCallback(MoveTemp(Target));
    const int64 CallableRef = Host.Godot.MakeCallable(Host.Godot.Ctx, CallbackId, OwnerHandle);
    if (CallableRef == 0)
    {
        GodotVerse::RemoveCallback(CallbackId);
        return GodotVerse::EHostFailure::CallbackFailed;
    }

    vh_value Callable{};
    Callable.Type = VH_TYPE_REF;
    Callable.VariantTag = VH_VARIANT_CALLABLE;
    Callable.Ref = CallableRef;
    const int32 Status = Host.Godot.ConnectSignal(Host.Godot.Ctx,
                                                  OwnerHandle,
                                                  reinterpret_cast<const char*>(*Name),
                                                  Name.Len(),
                                                  &Callable,
                                                  ConnectFlags);
    if (Status != VH_CALL_OK)
    {
        Host.Godot.ReleaseRef(Host.Godot.Ctx, CallableRef);
        GodotVerse::RemoveCallback(CallbackId);
        return GodotVerse::EHostFailure::CallbackFailed;
    }
    OutCallableRef = CallableRef;
    return CallbackId;
}

/// The object and signal name a Godot Signal *value* stands for. Fails for a reference that is not
/// a Signal, or for a consumer built before v6.0 declared the callback.
AUTORTFM_DISABLE GodotVerse::TResult<void> ResolveSignalRef(int64 Ref, int64& OutHandle, FUtf8String& OutName)
{
    GodotVerse::FHostState& Host = GodotVerse::GetHost();
    const char* NameUtf8 = nullptr;
    if (!Host.Godot.SignalTarget)
    {
        return GodotVerse::EHostFailure::CallbackMissing;
    }
    if (Host.Godot.SignalTarget(Host.Godot.Ctx, Ref, &OutHandle, &NameUtf8) != VH_CALL_OK || !NameUtf8)
    {
        return GodotVerse::EHostFailure::CallbackFailed;
    }
    // Copied now: the consumer owns those bytes only until its next call, and the name outlives
    // this in an awaiter row.
    OutName = FUtf8String(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(NameUtf8)));
    if (OutName.IsEmpty())
    {
        return GodotVerse::EHostFailure::CallbackFailed;
    }
    return GodotVerse::TResult<void>::Ok();
}

/// Registers one wait and connects what feeds it. OwnerHandle/Name say what to connect to.
AUTORTFM_DISABLE GodotVerse::TResult<int64> BeginAwait(UObject* Waiter, int64 SignalId, int64 OwnerHandle, const FUtf8String& Name)
{
    if (!Waiter)
    {
        return GodotVerse::EHostFailure::NotASignal;
    }
    if (OwnerHandle == 0 || Name.IsEmpty())
    {
        return GodotVerse::EHostFailure::UnknownId;
    }
    const int64 Token = GNextAwaitToken++;

    FCallbackTarget Target;
    Target.OwnerHandle = OwnerHandle;
    Target.AwaitToken = Token;

    int64 CallableRef = 0;
    // One-shot: a single `Await()` resumes once, so Godot dropping the connection as it fires is
    // exactly right and saves the disconnect. `loop { X.Await() }` reconnects per iteration, which
    // is what the source says it does.
    const GodotVerse::TResult<int64> CallbackId =
        ConnectDelivery(OwnerHandle, Name, MoveTemp(Target), VH_CONNECT_ONE_SHOT, CallableRef);
    if (!CallbackId)
    {
        return CallbackId.GetFailure();
    }

    FAwaiter Awaiter;
    Awaiter.Waiter = TStrongObjectPtr<UObject>(Waiter);
    Awaiter.SignalId = SignalId;
    Awaiter.OwnerHandle = OwnerHandle;
    Awaiter.Name = Name;
    Awaiter.CallableRef = CallableRef;
    Awaiter.CallbackId = CallbackId.GetValue();

    // The active scope is the awaiting task's own: an InstanceCall pushed the instance's before the
    // spawn, and a resumption pushes the task's again (TVerseCall::Return does it itself). So the
    // wait is anchored to exactly the thing whose death should end it.
    if (verse::FContentScopeGuard::IsActive())
    {
        const TSharedRef<verse::FContentScope>& Scope = verse::FContentScopeGuard::GetActiveScope();
        Awaiter.Scope = Scope;
        Awaiter.Cleanup = Scope->OnContentScopeCleanup.AddLambda(
            [Token](bool) { GodotVerse::EndSignalAwait(Token); });
    }

    GAwaiters.Add(Token, MoveTemp(Awaiter));
    return Token;
}
}

AUTORTFM_DISABLE int64 GodotVerse::BeginSignalAwait(UObject* Signal)
{
    verse::vh_signal* const Shadow = Cast<verse::vh_signal>(Signal);
    if (!Shadow)
    {
        VH_UNREPORTED("BeginSignalAwait: the object awaited is not a signal");
        return 0;
    }
    const int64 SignalId = Shadow->Id.Get();
    const FSignalBinding* const Binding = GSignalBindings.Find(SignalId);
    if (!Binding)
    {
        ReportVerseDiag(verse_diag::VG2109);
        return 0;
    }
    if (Binding->Reject != VH_SIGNAL_OK)
    {
        const std::string Reason = SignalRejectReason(Binding->Name, Binding->Reject, Binding->RejectDetail);
        ReportVerseDiag(verse_diag::VG2103, {{"signal", DiagArg(Binding->Name)}, {"reason", Reason}});
        return 0;
    }
    const TResult<int64> Token = BeginAwait(Signal, SignalId, Binding->OwnerHandle, Binding->Name);
    if (!Token)
    {
        if (WorthReporting(Token.GetFailure()))
        {
            VH_UNREPORTED("BeginSignalAwait: the wait could not be connected");
        }
        return 0;
    }
    return Token.GetValue();
}

AUTORTFM_DISABLE int64 GodotVerse::BeginSignalRefAwait(int64 Ref, UObject* Waiter)
{
    int64 OwnerHandle = 0;
    FUtf8String Name;
    if (!ResolveSignalRef(Ref, OwnerHandle, Name))
    {
        ReportVerseDiag(verse_diag::VG2110);
        return 0;
    }
    const TResult<int64> Token = BeginAwait(Waiter, 0, OwnerHandle, Name);
    if (!Token)
    {
        if (WorthReporting(Token.GetFailure()))
        {
            VH_UNREPORTED("BeginSignalRefAwait: the wait could not be connected");
        }
        return 0;
    }
    return Token.GetValue();
}

AUTORTFM_DISABLE void GodotVerse::EndSignalAwait(int64 Token)
{
    const FAwaiter* const Found = GAwaiters.Find(Token);
    if (!Found)
    {
        // Idempotent. A one-shot connection has already gone by the time a resumed wait ends, and
        // a `defer` that runs twice -- once on cancel, once on scope teardown -- must not say so.
        return;
    }
    const FAwaiter Awaiter = *Found;
    GAwaiters.Remove(Token);

    // Removed before anything else, so a wait that ended normally does not leave the scope holding
    // a lambda for the rest of its life. Harmless if this *is* the cleanup running -- the broadcast
    // clears its own list, and removing a handle that is already gone does nothing.
    if (const TSharedPtr<verse::FContentScope> Scope = Awaiter.Scope.Pin(); Scope.IsValid() && Awaiter.Cleanup.IsValid())
    {
        Scope->OnContentScopeCleanup.Remove(Awaiter.Cleanup);
    }

    GodotVerse::RemoveCallback(Awaiter.CallbackId);

    GodotVerse::FHostState& Host = GodotVerse::GetHost();
    if (Host.Godot.DisconnectSignal)
    {
        vh_value Callable{};
        Callable.Type = VH_TYPE_REF;
        Callable.VariantTag = VH_VARIANT_CALLABLE;
        Callable.Ref = Awaiter.CallableRef;
        // Refuses quietly for the one-shot Godot has already dropped, which is the resumed case.
        Host.Godot.DisconnectSignal(Host.Godot.Ctx,
                                    Awaiter.OwnerHandle,
                                    reinterpret_cast<const char*>(*Awaiter.Name),
                                    Awaiter.Name.Len(),
                                    &Callable);
    }
    if (Host.Godot.ReleaseRef)
    {
        Host.Godot.ReleaseRef(Host.Godot.Ctx, Awaiter.CallableRef);
    }
}

AUTORTFM_DISABLE int64 GodotVerse::SubscribeSignalRef(int64 Ref, const FVerseValue& Callback)
{
    int64 OwnerHandle = 0;
    FUtf8String Name;
    if (!ResolveSignalRef(Ref, OwnerHandle, Name))
    {
        ReportVerseDiag(verse_diag::VG2111);
        return 0;
    }

    int64 OwnerOfCallback = 0;
    FUtf8String Decorated;
    if (!DescribeBoundFunction(Callback.GetValue().DynamicCast<Verse::VFunction>(), OwnerOfCallback, Decorated))
    {
        ReportVerseDiag(verse_diag::VG2112);
        return 0;
    }

    FCallbackTarget Target;
    Target.OwnerHandle = OwnerOfCallback;
    Target.DecoratedName = Decorated;
    // Nothing declares a foreign signal's payload, so the handler takes the arguments as one Godot
    // Array and the packing happens on the way in.
    Target.bArgsAsArray = true;

    int64 CallableRef = 0;
    const TResult<int64> CallbackId = ConnectDelivery(OwnerHandle, Name, MoveTemp(Target), 0, CallableRef);
    if (!CallbackId)
    {
        if (WorthReporting(CallbackId.GetFailure()))
        {
            VH_UNREPORTED("SubscribeSignalRef: the subscription could not be connected");
        }
        return 0;
    }

    const int64 Id = GNextSubscriptionId++;
    GSubscriptions.Add(Id, FSubscription{OwnerHandle, Name, CallableRef});

    // Compensated exactly as signal.Subscribe is, and for the same reason: this mutates Godot
    // and answers a value, so it can be neither deferred to commit nor ignored. It is what closes
    // `Object.Connect`'s rollback gap -- that one stays an unforgiving direct call, and this is the
    // spelling a script reaches first.
    AutoRTFM::OnAbort<AutoRTFM::EOpenBehavior::SameAsClosed>(
        [Id] { GodotVerse::CancelSubscription(Id); });
    return Id;
}

AUTORTFM_DISABLE void GodotVerse::EnsureEventConnections(const TArray<int64>& EventBindings)
{
    for (const int64 Id : EventBindings)
    {
        FSignalBinding* const Binding = GSignalBindings.Find(Id);
        // Idempotent, and a refused signal is skipped: Godot was never told about one, so `connect`
        // would fail on the name and "connect failed" is a worse sentence than the one the editor
        // already gave at the member's line.
        if (!Binding || Binding->CallbackId != 0 || Binding->Reject != VH_SIGNAL_OK)
        {
            continue;
        }

        FCallbackTarget Target;
        Target.OwnerHandle = Binding->OwnerHandle;
        Target.EventSignalId = Id;
        int64 CallableRef = 0;
        const TResult<int64> CallbackId =
            ConnectDelivery(Binding->OwnerHandle, Binding->Name, MoveTemp(Target), 0, CallableRef);
        if (CallbackId)
        {
            Binding->CallableRef = CallableRef;
            Binding->CallbackId = CallbackId.GetValue();
            continue;
        }

        // Silence here would be the worst answer available: the member registers, emissions still
        // reach Godot, and only delivery *back into the event* is missing -- so every await on it
        // hangs and nothing says why.
        ReportVerseDiag(verse_diag::VG2113, {{"signal", DiagArg(Binding->Name)}});
    }
}

AUTORTFM_DISABLE void GodotVerse::ReleaseEventBindings(const TArray<int64>& EventBindings)
{
    FHostState& Host = GetHost();
    for (const int64 Id : EventBindings)
    {
        if (const FSignalBinding* const Binding = GSignalBindings.Find(Id))
        {
            if (Binding->CallableRef != 0 && Host.Godot.ReleaseRef)
            {
                Host.Godot.ReleaseRef(Host.Godot.Ctx, Binding->CallableRef);
            }
            if (Binding->CallbackId != 0)
            {
                RemoveCallback(Binding->CallbackId);
            }
            if (const UObject* const Event = Binding->Event.Get())
            {
                GEventBindingIds.Remove(Event);
            }
        }
        GSignalBindings.Remove(Id);
    }
}

AUTORTFM_DISABLE void GodotVerse::SetRecordedEngineSignalTypes(TSharedPtr<FEngineSignalTypes> Types)
{
    GRecordedEngineSignals = MoveTemp(Types);
}
