// Copyright Epic Games, Inc. All Rights Reserved.

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Utf8String.h"
#include "GodotClasses.h"
#include "HostRuntime.h"
#include "HostEventLoop.h"
#include "HostScript.h"
#include "Templates/UniquePtr.h"
#include "VerseString.h"
#include "VerseValue.h"
#include "VerseVM/VVMCoroutine.h"
#include "VerseVM/VVMRuntimeError.h"

#include "VerseHost.gen.h"
#include "VerseHost.gen.ipp"

namespace {

using GodotVerse::FCallArena;
using GodotVerse::FHostState;
using GodotVerse::GetHost;

FUtf8StringView ToView(const verse::string& String)
{
    return FUtf8StringView(String);
}

const char* Bytes(const FUtf8StringView& View)
{
    return reinterpret_cast<const char*>(View.GetData());
}

vh_value StringValue(const FUtf8StringView& View)
{
    vh_value Value{};
    Value.Type = VH_TYPE_STRING;
    Value.String.Utf8 = Bytes(View);
    Value.String.Len = View.Len();
    return Value;
}

/// Every Godot callback lives in a DLL the AutoRTFM compiler never saw, so it has no instrumented
/// clone and cannot be called from closed code. AutoRTFM::Open is the sanctioned way across.
template <typename CallableType>
decltype(auto) CallGodot(CallableType&& Callable)
{
    return AutoRTFM::Open(Forward<CallableType>(Callable));
}

/// Reaching through a handle Godot has freed is a bug in the script, not a value that happens to
/// be absent, so it is reported the way Verse reports reading a var out of a dead object: an
/// unrecoverable runtime error. Raising aborts every enclosing transaction -- which is what drops
/// the mutations this call had already deferred -- unwinds to the root failure context, and does
/// not return here in any way the caller can observe.
void RaiseCallStatus(int32 Status, int64 Handle, const verse::string& Member, const TCHAR* Verb)
{
    const FUtf8String Name(ToView(Member));
    switch (Status)
    {
    case VH_CALL_DEAD_OBJECT:
        RAISE_VERSE_RUNTIME_ERROR_FORMAT(
            Verse::ERuntimeDiagnostic::ErrRuntime_NativeInternal,
            TEXT("%s `%hs` on Godot object %lld, which Godot has already freed. Test "
                 "IsInstanceValid[...] before reaching through a reference the scene may have dropped."),
            Verb,
            reinterpret_cast<const char*>(*Name),
            Handle);
        break;

    case VH_CALL_BAD_VALUE:
        RAISE_VERSE_RUNTIME_ERROR_FORMAT(
            Verse::ERuntimeDiagnostic::ErrRuntime_NativeInternal,
            TEXT("%s `%hs` on Godot object %lld, and the value has no representation on the Verse "
                 "bridge. This is a gap in the type table in tools/gen_verse_api.py."),
            Verb,
            reinterpret_cast<const char*>(*Name),
            Handle);
        break;

    /* The mirror in GodotClasses.native.verse is generated from the same extension_api.json the
     * engine was built from, so a member the live object does not have means the two have drifted
     * apart. Failing quietly would leave that drift invisible until the call silently did nothing.
     * Property *reads* are the exception and never arrive here -- Godot cannot tell an absent
     * property from a nil one, so a miss there stays an ordinary failure. */
    case VH_CALL_NO_SUCH_MEMBER:
        RAISE_VERSE_RUNTIME_ERROR_FORMAT(
            Verse::ERuntimeDiagnostic::ErrRuntime_NativeInternal,
            TEXT("%s `%hs` on Godot object %lld, which has no such member. The generated Verse "
                 "mirror and this build of Godot disagree; regenerate with tools/gen_verse_api.py."),
            Verb,
            reinterpret_cast<const char*>(*Name),
            Handle);
        break;

    default:
        break;
    }
}

/// The same as RaiseCallStatus, for a reference id rather than a named member of an object.
void RaiseRefStatus(int32 Status, int64 Ref, const TCHAR* Verb)
{
    if (Status == VH_CALL_OK || Status == VH_CALL_NO_SUCH_MEMBER)
    {
        return;
    }
    // Reference 0 never named anything, so the released-handle story is the wrong one to tell: it
    // is what `godot_array{}` holds, and a script can write that -- the container wrappers are
    // public so they can be named in a signature, and their Ref is not. Blaming collection for it
    // sends the author looking for a lifetime bug they do not have.
    if (Ref == 0)
    {
        RAISE_VERSE_RUNTIME_ERROR_FORMAT(
            Verse::ERuntimeDiagnostic::ErrRuntime_NativeInternal,
            TEXT("%s a Godot container that names nothing. A container built in Verse -- "
                 "`godot_array{}` and the like -- holds no Godot value; one has to come back from "
                 "Godot."),
            Verb);
        return;
    }
    RAISE_VERSE_RUNTIME_ERROR_FORMAT(
        Verse::ERuntimeDiagnostic::ErrRuntime_NativeInternal,
        TEXT("%s a Godot container the bridge no longer holds (reference %lld). A reference is "
             "released when the Verse value holding it is collected, so this is a handle kept past "
             "the object that owned it."),
        Verb,
        Ref);
}

/// A deferred write reports nothing useful: by the time OnCommit runs, the transaction a runtime
/// error would have rolled back has already committed. Probing the receiver up front is what lets
/// a write to a freed object fail at the point the script actually made it.
bool RaiseIfDead(int64 Handle, const verse::string& Member, const TCHAR* Verb)
{
    FHostState& Host = GetHost();
    if (!Host.Godot.IsValid)
    {
        return false;
    }
    if (CallGodot([&] { return Host.Godot.IsValid(Host.Godot.Ctx, Handle) != 0; }))
    {
        return false;
    }
    RaiseCallStatus(VH_CALL_DEAD_OBJECT, Handle, Member, Verb);
    return true;
}

/// Reads one property through the Godot callback table. Returns false if the property is missing
/// or the host is not wired up; a dead receiver raises rather than returning.
bool ReadProperty(int64 Handle, const verse::string& Property, FCallArena& Arena, vh_value& OutValue)
{
    FHostState& Host = GetHost();
    if (!Host.Godot.GetProperty)
    {
        return false;
    }

    const FUtf8StringView Name = ToView(Property);
    const int32 Status = CallGodot([&] {
        return Host.Godot.GetProperty(Host.Godot.Ctx, Handle, Bytes(Name), Name.Len(), &Arena, &OutValue);
    });
    if (Status == VH_CALL_NO_SUCH_MEMBER)
    {
        return false;
    }
    if (Status != VH_CALL_OK)
    {
        RaiseCallStatus(Status, Handle, Property, TEXT("Read"));
        return false;
    }
    return true;
}

/// Godot mutations are deferred to transaction commit: a Verse failure must not leave the scene
/// half-written. Outside a transaction AutoRTFM::OnCommit runs the callback immediately.
template <typename CallableType>
void DeferToCommit(CallableType&& Callable)
{
    // Commit handlers already run outside the transaction, so no Open is needed (nor allowed) here.
    AutoRTFM::OnCommit(Forward<CallableType>(Callable));
}

/// The C++ spelling of Verse's `variant`, from GodotClasses.h. One Godot Variant as fixed-width
/// lanes; Godot.native.verse carries the reasoning for the shape.
using FGodotValue = verse::variant;

/// How many float lanes a tag's value occupies, and how many int lanes.
///
/// One table rather than a switch in each direction, because the two directions have to agree
/// exactly: a type written with three floats and read with two is a silent truncation, and the
/// only way to be sure they match is for there to be one statement of it.
struct FLaneCount
{
    int32 Ints = 0;
    int32 Floats = 0;
};

FLaneCount LanesFor(int64 Tag)
{
    switch (Tag)
    {
    case VH_VARIANT_BOOL:
    case VH_VARIANT_INT:
        return {1, 0};
    case VH_VARIANT_FLOAT:
        return {0, 1};

    case VH_VARIANT_VECTOR2:
        return {0, 2};
    case VH_VARIANT_VECTOR3:
        return {0, 3};
    case VH_VARIANT_VECTOR4:
    case VH_VARIANT_RECT2:
    case VH_VARIANT_PLANE:
    case VH_VARIANT_QUATERNION:
    case VH_VARIANT_COLOR:
        return {0, 4};
    case VH_VARIANT_TRANSFORM2D:
    case VH_VARIANT_AABB:
        return {0, 6};
    case VH_VARIANT_BASIS:
        return {0, 9};
    case VH_VARIANT_TRANSFORM3D:
        return {0, 12};
    case VH_VARIANT_PROJECTION:
        return {0, 16};

    // The integer vectors take int lanes rather than float ones: a double stops counting exactly
    // above 2^53, and Godot's are 32-bit signed values that must survive a round trip intact.
    case VH_VARIANT_VECTOR2I:
        return {2, 0};
    case VH_VARIANT_VECTOR3I:
        return {3, 0};
    case VH_VARIANT_VECTOR4I:
    case VH_VARIANT_RECT2I:
        return {4, 0};

    default:
        // Strings ride in Text, references in Ref, and nil occupies nothing.
        return {0, 0};
    }
}

int64 IntLane(const FGodotValue& Value, int32 Index)
{
    switch (Index)
    {
    case 0: return Value.I0;
    case 1: return Value.I1;
    case 2: return Value.I2;
    default: return Value.I3;
    }
}

void SetIntLane(FGodotValue& Value, int32 Index, int64 Lane)
{
    switch (Index)
    {
    case 0: Value.I0 = Lane; break;
    case 1: Value.I1 = Lane; break;
    case 2: Value.I2 = Lane; break;
    default: Value.I3 = Lane; break;
    }
}

double FloatLane(const FGodotValue& Value, int32 Index)
{
    switch (Index)
    {
    case 0: return Value.F0;
    case 1: return Value.F1;
    case 2: return Value.F2;
    case 3: return Value.F3;
    case 4: return Value.F4;
    case 5: return Value.F5;
    case 6: return Value.F6;
    case 7: return Value.F7;
    case 8: return Value.F8;
    case 9: return Value.F9;
    case 10: return Value.F10;
    case 11: return Value.F11;
    case 12: return Value.F12;
    case 13: return Value.F13;
    case 14: return Value.F14;
    default: return Value.F15;
    }
}

void SetFloatLane(FGodotValue& Value, int32 Index, double Lane)
{
    switch (Index)
    {
    case 0: Value.F0 = Lane; break;
    case 1: Value.F1 = Lane; break;
    case 2: Value.F2 = Lane; break;
    case 3: Value.F3 = Lane; break;
    case 4: Value.F4 = Lane; break;
    case 5: Value.F5 = Lane; break;
    case 6: Value.F6 = Lane; break;
    case 7: Value.F7 = Lane; break;
    case 8: Value.F8 = Lane; break;
    case 9: Value.F9 = Lane; break;
    case 10: Value.F10 = Lane; break;
    case 11: Value.F11 = Lane; break;
    case 12: Value.F12 = Lane; break;
    case 13: Value.F13 = Lane; break;
    case 14: Value.F14 = Lane; break;
    default: Value.F15 = Lane; break;
    }
}

/// A variant detached from the VM. Deferred writes run after the transaction that produced their
/// arguments has committed, and a verse::string is not ours to hold across that boundary.
struct FOwnedValue
{
    FGodotValue Lanes;
    FUtf8String Text;
};

FOwnedValue Own(const FGodotValue& Value)
{
    FOwnedValue Owned;
    Owned.Lanes = Value;
    Owned.Text = FUtf8String(ToView(Value.Text));
    // The verse::string is dropped: Text above is the copy that outlives the transaction, and a
    // dangling one here would be indistinguishable from a live one at the point it is read.
    Owned.Lanes.Text = verse::string{};
    return Owned;
}

/// Builds the vh_value the C ABI carries from an owned variant.
///
/// Nothing here allocates: every value type is lanes, and every reference type is an id the
/// consumer minted. That is the whole of what the fixed-width shape buys -- the encoding this
/// replaced built up to four arrays to carry one number.
vh_value WireOf(const FOwnedValue& Owned)
{
    const FGodotValue& Lanes = Owned.Lanes;

    vh_value Out{};
    Out.VariantTag = static_cast<int32>(Lanes.Tag);

    switch (Lanes.Tag)
    {
    case VH_VARIANT_BOOL:
        Out.Type = VH_TYPE_LOGIC;
        Out.Logic = Lanes.I0 != 0 ? 1 : 0;
        return Out;

    case VH_VARIANT_INT:
        Out.Type = VH_TYPE_INT;
        Out.Int = Lanes.I0;
        return Out;

    case VH_VARIANT_FLOAT:
        Out.Type = VH_TYPE_FLOAT;
        Out.Float = Lanes.F0;
        return Out;

    case VH_VARIANT_STRING:
    case VH_VARIANT_STRING_NAME:
    case VH_VARIANT_NODE_PATH:
        Out.Type = VH_TYPE_STRING;
        Out.String.Utf8 = reinterpret_cast<const char*>(*Owned.Text);
        Out.String.Len = Owned.Text.Len();
        return Out;

    // An object is named by Godot's own instance id, so it needs no table; everything else that
    // carries a reference was given an id by the consumer.
    case VH_VARIANT_OBJECT:
    case VH_VARIANT_RID:
        Out.Type = VH_TYPE_INT;
        Out.Int = Lanes.Ref;
        return Out;

    case VH_VARIANT_NIL:
        Out.Type = VH_TYPE_VOID;
        return Out;

    default:
        break;
    }

    const FLaneCount Lanes_ = LanesFor(Lanes.Tag);
    if (Lanes_.Ints == 0 && Lanes_.Floats == 0)
    {
        // A reference type: Array, Dictionary, Callable, Signal, or a packed array.
        Out.Type = VH_TYPE_REF;
        Out.Ref = Lanes.Ref;
        return Out;
    }

    // A math struct. It crosses as a tuple of its components in Godot's own order, which is what
    // verse_value.cpp rebuilds the Godot type from.
    Out.Type = VH_TYPE_TUPLE;
    return Out;
}

/// The components of a math struct, for the wire's tuple arm. Separate from Wire because the
/// items have to live somewhere the caller owns for the length of the call.
void WireComponents(const FGodotValue& Lanes, TArray<vh_value>& OutItems)
{
    const FLaneCount Count = LanesFor(Lanes.Tag);
    OutItems.Reset();
    OutItems.Reserve(Count.Ints + Count.Floats);
    for (int32 Index = 0; Index < Count.Ints; ++Index)
    {
        vh_value& Item = OutItems.AddDefaulted_GetRef();
        Item.Type = VH_TYPE_INT;
        Item.Int = IntLane(Lanes, Index);
    }
    for (int32 Index = 0; Index < Count.Floats; ++Index)
    {
        vh_value& Item = OutItems.AddDefaulted_GetRef();
        Item.Type = VH_TYPE_FLOAT;
        Item.Float = FloatLane(Lanes, Index);
    }
}

/// Owns the component arrays of however many variants one call carries.
///
/// Each block is its own allocation and none is ever moved: a vh_value holds a bare pointer into
/// one, and growing a single array of blocks would leave an already-built value pointing at freed
/// memory.
class FWireStore
{
public:
    vh_value Wire(const FOwnedValue& Owned)
    {
        vh_value Out = WireOf(Owned);
        if (Out.Type != VH_TYPE_TUPLE)
        {
            return Out;
        }
        TUniquePtr<TArray<vh_value>>& Block = Blocks.Add_GetRef(MakeUnique<TArray<vh_value>>());
        WireComponents(Owned.Lanes, *Block);
        Out.Seq.Items = Block->GetData();
        Out.Seq.Count = Block->Num();
        return Out;
    }

private:
    TArray<TUniquePtr<TArray<vh_value>>> Blocks;
};

/// The payload of a numeric vh_value, whichever of the three numeric shapes it arrived in.
///
/// Godot spells a number in whatever Variant its own API declared, and a method the mirror types
/// as `float` can hand back an integer Variant -- `0` is the usual one. Coercing here rather than
/// in the Verse unpackers is what lets those be plain lane reads: by the time a variant exists,
/// the lane its tag names is filled.
int64 AsInt(const vh_value& Value)
{
    switch (Value.Type)
    {
    case VH_TYPE_LOGIC: return Value.Logic != 0 ? 1 : 0;
    case VH_TYPE_FLOAT: return (int64)Value.Float;
    case VH_TYPE_REF: return Value.Ref;
    default: return Value.Int;
    }
}

double AsDouble(const vh_value& Value)
{
    switch (Value.Type)
    {
    case VH_TYPE_LOGIC: return Value.Logic != 0 ? 1.0 : 0.0;
    case VH_TYPE_INT: return (double)Value.Int;
    default: return Value.Float;
    }
}

/// The tag a payload implies, for a consumer that did not name one. Only the unambiguous shapes
/// are guessed: a tuple is equally a Vector2, a Vector2i or a Rect2, and guessing there would be
/// the untagged encoding this design replaced.
int64 InferTag(int32 Type)
{
    switch (Type)
    {
    case VH_TYPE_LOGIC: return VH_VARIANT_BOOL;
    case VH_TYPE_INT: return VH_VARIANT_INT;
    case VH_TYPE_FLOAT: return VH_VARIANT_FLOAT;
    case VH_TYPE_STRING: return VH_VARIANT_STRING;
    default: return VH_VARIANT_NIL;
    }
}

/// Reads a vh_value back into the variant's lanes.
///
/// Driven by the tag rather than by the payload's shape, so that the lane a tag names is the lane
/// that gets filled. The Verse unpackers rely on exactly that.
FGodotValue FromWire(const vh_value& Value)
{
    FGodotValue Out;
    Out.Tag = Value.VariantTag != VH_VARIANT_NIL ? Value.VariantTag : InferTag(Value.Type);

    switch (Out.Tag)
    {
    case VH_VARIANT_NIL:
        return Out;

    case VH_VARIANT_BOOL:
        Out.I0 = AsInt(Value) != 0 ? 1 : 0;
        return Out;

    case VH_VARIANT_INT:
        Out.I0 = AsInt(Value);
        return Out;

    case VH_VARIANT_FLOAT:
        Out.F0 = AsDouble(Value);
        return Out;

    case VH_VARIANT_STRING:
    case VH_VARIANT_STRING_NAME:
    case VH_VARIANT_NODE_PATH:
        if (Value.Type == VH_TYPE_STRING)
        {
            Out.Text = verse::string(GodotVerse::MakeView(Value.String.Utf8, Value.String.Len));
        }
        return Out;

    // An object is named by Godot's own instance id and an RID by its own number; neither needs a
    // table entry, so both land in Ref without one being minted.
    case VH_VARIANT_OBJECT:
    case VH_VARIANT_RID:
        Out.Ref = AsInt(Value);
        return Out;

    default:
        break;
    }

    const FLaneCount Count = LanesFor(Out.Tag);
    if (Count.Ints == 0 && Count.Floats == 0)
    {
        // A reference type: Array, Dictionary, Callable, Signal, or a packed array. The id is the
        // consumer's, and the host holds it until the Verse value wrapping it is collected.
        Out.Ref = Value.Type == VH_TYPE_REF ? Value.Ref : AsInt(Value);
        return Out;
    }

    // A math struct, as its components in Godot's own order. A payload shorter than the lanes its
    // tag promised leaves the rest at zero rather than reading past what it was given.
    if (Value.Type == VH_TYPE_TUPLE || Value.Type == VH_TYPE_ARRAY)
    {
        int32 Cursor = 0;
        for (int32 Index = 0; Index < Count.Ints && Cursor < Value.Seq.Count; ++Index, ++Cursor)
        {
            SetIntLane(Out, Index, AsInt(Value.Seq.Items[Cursor]));
        }
        for (int32 Index = 0; Index < Count.Floats && Cursor < Value.Seq.Count; ++Index, ++Cursor)
        {
            SetFloatLane(Out, Index, AsDouble(Value.Seq.Items[Cursor]));
        }
    }
    return Out;
}

} // namespace

// The variant conversion, for the one caller outside this file.
//
// HostScript describes a script method's `variant` parameter and result, and marshalling one there
// has to mean exactly what marshalling one here means -- a second implementation of the lane rules
// is two chances to disagree about which lane a Rect2 puts its height in. So the wire half is
// these two, and the VM half is FNativeConverter's, which is what VNI's own generated glue calls.
AUTORTFM_DISABLE verse::variant GodotVerse::VariantFromWire(const vh_value& Value)
{
    return FromWire(Value);
}

AUTORTFM_DISABLE vh_value GodotVerse::VariantToWire(const verse::variant& Value,
                                                    FUtf8String& OutText,
                                                    TArray<vh_value>& OutComponents)
{
    const FOwnedValue Owned = Own(Value);
    OutText = Owned.Text;

    vh_value Out = WireOf(Owned);
    if (Out.Type == VH_TYPE_STRING)
    {
        // WireOf pointed at the FOwnedValue's copy, which dies with this frame. The caller's is
        // the one that outlives the call.
        Out.String.Utf8 = reinterpret_cast<const char*>(*OutText);
        Out.String.Len = OutText.Len();
    }
    else if (Out.Type == VH_TYPE_TUPLE)
    {
        WireComponents(Owned.Lanes, OutComponents);
        Out.Seq.Items = OutComponents.GetData();
        Out.Seq.Count = OutComponents.Num();
    }
    return Out;
}

namespace verse { namespace Godot {

void Print(verse::string const& Message)
{
    FUtf8String Text(ToView(Message));
    DeferToCommit([Text = MoveTemp(Text)] {
        FHostState& Host = GetHost();
        if (Host.Godot.Print)
        {
            Host.Godot.Print(Host.Godot.Ctx, reinterpret_cast<const char*>(*Text), Text.Len());
        }
    });
}

bool VhIsValid(int64 Handle)
{
    FHostState& Host = GetHost();
    return Host.Godot.IsValid && CallGodot([&] { return Host.Godot.IsValid(Host.Godot.Ctx, Handle) != 0; });
}

TNonNullPtr<verse::vh_object> VhObjectOf(int64 Handle)
{
    // Open, and for the usual two reasons at once: the class lookup calls a Godot callback in a
    // DLL the AutoRTFM compiler never saw, and NewObject is not instrumented either.
    //
    // AutoRTFM::Open rather than CallGodot, because only Open's own parameter carries
    // AUTORTFM_IMPLICIT_DISABLE -- the wrapper's does not, so a lambda built for it stays closed
    // and naming an AUTORTFM_DISABLE function inside one is a compile error.
    return AutoRTFM::Open([&] {
        return TNonNullPtr<verse::vh_object>(CastChecked<verse::vh_object>(GodotVerse::ObjectForHandle(Handle)));
    });
}

/// R-NODE-3. The peer for an object under construction: the one the host already holds where the
/// host is the side constructing it, and a fresh Godot object otherwise.
///
/// Open for VhObjectOf's two reasons at once -- minting reaches a Godot callback in a DLL the
/// AutoRTFM compiler never saw, and the class walk is not instrumented either. The raise is outside
/// the Open, as every other raise in this file is: raising from closed code trips
/// AutoRTFM::UnreachableIfClosed, and raising from *inside* an Open is nothing the rest of this
/// bridge does either.
int64 VhAdoptOrMint(TNonNullPtr<verse::vh_object> Object)
{
    const char* Refused = nullptr;
    const int64 Handle =
        AutoRTFM::Open([&] { return GodotVerse::AdoptOrMintPeer(Object.Get(), Refused); });
    if (Refused)
    {
        RAISE_VERSE_RUNTIME_ERROR_FORMAT(
            Verse::ERuntimeDiagnostic::ErrRuntime_NativeInternal,
            TEXT("Godot would not make a `%hs`, so this class has no object to be. A Godot class "
                 "that is abstract, or that the engine only ever hands out as a singleton, cannot "
                 "be constructed -- derive from one that can, or reach the singleton through its "
                 "accessor."),
            Refused);
    }
    return Handle;
}

int64 VhCallableFrom(FVerseValue const& Callback)
{
    // Open: the lookup walks the semantic program and asks Godot to mint the Callable, and neither
    // is instrumented. AutoRTFM::Open rather than CallGodot, for the reason VhObjectOf gives.
    return AutoRTFM::Open([&] { return GodotVerse::MakeCallableFor(Callback); });
}

// --- signals ------------------------------------------------------------------------------

void VhSignalEmit(int64 Id, FVerseValue const& Payload)
{
    // Immediate, and the comment on the Verse declaration says why. Open for the usual reason:
    // decomposing the payload reads the semantic program and the emission calls into Godot.
    AutoRTFM::Open([&] { GodotVerse::EmitSignal(Id, Payload); });
}

int64 VhSignalSubscribe(int64 Id, FVerseValue const& Callback)
{
    return AutoRTFM::Open([&] { return GodotVerse::SubscribeSignal(Id, Callback); });
}

void VhSignalCancel(int64 Subscription)
{
    AutoRTFM::Open([&] { GodotVerse::CancelSubscription(Subscription); });
}

int64 VhSignalBind(int64 Handle, verse::string const& Class, verse::string const& Accessor, verse::string const& Name)
{
    const FUtf8String OwnClass(ToView(Class));
    const FUtf8String OwnAccessor(ToView(Accessor));
    const FUtf8String OwnName(ToView(Name));
    return AutoRTFM::Open([&] {
        return GodotVerse::BindEngineSignal(Handle,
                                            FUtf8StringView(OwnClass),
                                            FUtf8StringView(OwnAccessor),
                                            FUtf8StringView(OwnName));
    });
}

int64 VhSignalAwait(TNonNullPtr<verse::vh_signal> Signal)
{
    // Open: connecting reaches Godot, and the binding lookup walks host tables. AutoRTFM::Open
    // rather than CallGodot, for the reason VhObjectOf gives.
    return AutoRTFM::Open([&] { return GodotVerse::BeginSignalAwait(Signal.Get()); });
}

void VhSignalAwaitEnd(int64 Token)
{
    AutoRTFM::Open([&] { GodotVerse::EndSignalAwait(Token); });
}

int64 VhSignalRefAwait(int64 Ref, TNonNullPtr<verse::vh_signal> Waiter)
{
    return AutoRTFM::Open([&] { return GodotVerse::BeginSignalRefAwait(Ref, Waiter.Get()); });
}

int64 VhSignalRefSubscribe(int64 Ref, FVerseValue const& Callback)
{
    return AutoRTFM::Open([&] { return GodotVerse::SubscribeSignalRef(Ref, Callback); });
}

int64 VhSignalRefFor(int64 Handle, verse::string const& Name)
{
    const FUtf8StringView View = ToView(Name);
    FHostState& Host = GetHost();
    if (!Host.Godot.MakeSignalRef)
    {
        return 0;
    }
    return CallGodot([&] { return Host.Godot.MakeSignalRef(Host.Godot.Ctx, Handle, Bytes(View), View.Len()); });
}

// --- sleeping -----------------------------------------------------------------------------

/// Verse's own `Sleep`, on the host's real-time clock.
///
/// The shape is Epic's (`verse::Simulation::Sleep` in Simulation.cpp): capture the call into a
/// TStrongVerseCall so it survives GC while the task is suspended, arrange for something to call
/// `Return` later, and answer `Suspend`. What differs is what "later" means -- Epic uses the
/// world's TimerManager and this uses the pump, because a bridge that must work with no scene tree
/// has no world to ask.
///
/// `Call.Return` re-enters the suspended task's *own* content scope and declines if that scope was
/// terminated, so a sleeping task on a freed node is dropped with no work here (R-ASYNC-5).
FVerseResult Sleep(TVerseCall<void> Call, double Seconds)
{
    const verse::FExecutionContext ExecContext = verse::FExecutionContext::GetActiveContext();
    if (Seconds < 0.0)
    {
        // Not a suspension at all, which is what Epic's does with the same input.
        return Call.Return(ExecContext);
    }

    AutoRTFM::Open([&] {
        GodotVerse::EnqueueSleep(Seconds, [StrongCall = TStrongVerseCall<void>(Call)]() mutable {
            const verse::FExecutionContext ResumeContext = verse::FExecutionContext::GetActiveContext();
            // Its own transaction, nested inside whatever the pump is already in, for the reason
            // DeliverToAwaiter gives: a raise in the resumed task must not roll back anything but
            // the task's own writes.
            AutoRTFM::Transact([&] { AutoRTFM::Open([&] { StrongCall.Return(ResumeContext); }); });
        });
    });
    return Call.Suspend(ExecContext);
}

void VhTypeMismatch(verse::string const& Expected, FGodotValue const& Value)
{
    const FUtf8String Name(ToView(Expected));
    RAISE_VERSE_RUNTIME_ERROR_FORMAT(
        Verse::ERuntimeDiagnostic::ErrRuntime_NativeInternal,
        TEXT("Godot returned a value tagged %lld where the Verse bridge expected `%hs`. The type "
             "table in tools/gen_verse_api.py and this build of Godot disagree."),
        Value.Tag,
        reinterpret_cast<const char*>(*Name));
}

void VhCallValue(int64 Handle, verse::string const& Method, TArray<FGodotValue> const& Args, FGodotValue& OutValue)
{
    OutValue = FGodotValue{};
    FHostState& Host = GetHost();
    if (!Host.Godot.CallMethod)
    {
        RAISE_VERSE_RUNTIME_ERROR_CODE(Verse::ERuntimeDiagnostic::ErrRuntime_NativeInternal);
        return;
    }

    FWireStore Store;
    TArray<vh_value> Wire;
    Wire.Reserve(Args.Num());
    for (const FGodotValue& Arg : Args)
    {
        Wire.Add(Store.Wire(Own(Arg)));
    }

    const FUtf8StringView Name = ToView(Method);
    FCallArena Arena;
    vh_value Result{};
    const int32 Status = CallGodot([&] {
        return Host.Godot.CallMethod(
            Host.Godot.Ctx, Handle, Bytes(Name), Name.Len(), Wire.GetData(), Wire.Num(), &Arena, &Result);
    });
    if (Status != VH_CALL_OK)
    {
        RaiseCallStatus(Status, Handle, Method, TEXT("Called"));
        return;
    }
    OutValue = FromWire(Result);
}

/// `VhCallValue` for a method Godot marks `const`. The same work: what differs is the Verse-side
/// effect, and the promise that goes with it -- this one may not defer anything to commit and may
/// not compensate anything on abort, because there is nothing to undo.
void VhCallValueConst(int64 Handle, verse::string const& Method, TArray<FGodotValue> const& Args, FGodotValue& OutValue)
{
    VhCallValue(Handle, Method, Args, OutValue);
}

void VhCallStatic(verse::string const& Class,
                  verse::string const& Method,
                  TArray<FGodotValue> const& Args,
                  FGodotValue& OutValue)
{
    OutValue = FGodotValue{};
    FHostState& Host = GetHost();
    if (!Host.Godot.CallStatic)
    {
        return;
    }

    FWireStore Store;
    TArray<vh_value> Wire;
    Wire.Reserve(Args.Num());
    for (const FGodotValue& Arg : Args)
    {
        Wire.Add(Store.Wire(Own(Arg)));
    }

    const FUtf8StringView ClassName = ToView(Class);
    const FUtf8StringView Name = ToView(Method);
    FCallArena Arena;
    vh_value Result{};
    const int32 Status = CallGodot([&] {
        return Host.Godot.CallStatic(Host.Godot.Ctx,
                                     Bytes(ClassName), ClassName.Len(),
                                     Bytes(Name), Name.Len(),
                                     Wire.GetData(), Wire.Num(), &Arena, &Result);
    });
    if (Status != VH_CALL_OK)
    {
        RaiseCallStatus(Status, 0, Method, TEXT("Called static"));
        return;
    }
    OutValue = FromWire(Result);
}

void VhCallUtility(verse::string const& Name, TArray<FGodotValue> const& Args, FGodotValue& OutValue)
{
    OutValue = FGodotValue{};
    FHostState& Host = GetHost();
    if (!Host.Godot.CallUtility)
    {
        return;
    }

    FWireStore Store;
    TArray<vh_value> Wire;
    Wire.Reserve(Args.Num());
    for (const FGodotValue& Arg : Args)
    {
        Wire.Add(Store.Wire(Own(Arg)));
    }

    const FUtf8StringView Which = ToView(Name);
    FCallArena Arena;
    vh_value Result{};
    const int32 Status = CallGodot([&] {
        return Host.Godot.CallUtility(Host.Godot.Ctx, Bytes(Which), Which.Len(),
                                      Wire.GetData(), Wire.Num(), &Arena, &Result);
    });
    if (Status != VH_CALL_OK)
    {
        RaiseCallStatus(Status, 0, Name, TEXT("Called"));
        return;
    }
    OutValue = FromWire(Result);
}

/// `VhCallUtility` for a utility that only looks something up. Same work; see the Verse declaration
/// for why the set is chosen by hand rather than read off a flag.
void VhCallUtilityConst(verse::string const& Name, TArray<FGodotValue> const& Args, FGodotValue& OutValue)
{
    VhCallUtility(Name, Args, OutValue);
}

void VhCallVoid(int64 Handle, verse::string const& Method, TArray<FGodotValue> const& Args)
{
    if (RaiseIfDead(Handle, Method, TEXT("Called")))
    {
        return;
    }

    FUtf8String Name(ToView(Method));
    TArray<FOwnedValue> Owned;
    Owned.Reserve(Args.Num());
    for (const FGodotValue& Arg : Args)
    {
        Owned.Add(Own(Arg));
    }

    DeferToCommit([Handle, Name = MoveTemp(Name), Owned = MoveTemp(Owned)] {
        FHostState& Host = GetHost();
        if (!Host.Godot.CallMethod)
        {
            return;
        }

        FWireStore Store;
        TArray<vh_value> Wire;
        Wire.Reserve(Owned.Num());
        for (const FOwnedValue& Arg : Owned)
        {
            Wire.Add(Store.Wire(Arg));
        }

        FCallArena Arena;
        vh_value Result{};
        Host.Godot.CallMethod(Host.Godot.Ctx,
                              Handle,
                              reinterpret_cast<const char*>(*Name),
                              Name.Len(),
                              Wire.GetData(),
                              Wire.Num(),
                              &Arena,
                              &Result);
    });
}

void VhGetValue(int64 Handle, verse::string const& Property, TOptional<FGodotValue>& OutValue)
{
    OutValue.Reset();
    FCallArena Arena;
    vh_value Value{};
    if (!ReadProperty(Handle, Property, Arena, Value))
    {
        return;
    }
    OutValue = FromWire(Value);
}

// `Variant(Value:any)`. The type dispatch happens here because Verse cannot do it at compile time:
// an overload set may hold at most one parameter from the emptiable family (`logic`, any option and
// any array, and `string` is `[]char`), so the 38-way overloaded builder an author reaches for does
// not exist as a set. One `any` parameter has nothing to resolve against and sidesteps it.
//
// Failing rather than answering Nil is the point of the `<decides>`: a value with no Godot meaning
// is the author's mistake, and "you cannot build a variant from this" is a better answer than a
// variant holding nothing, which would be indistinguishable from a deliberate `variant{}`.
void VhVariantFromAny(FVerseValue const& Value, TOptional<FGodotValue>& OutValue)
{
    OutValue.Reset();

    // Storage outlives the Open because the string lane points into it until FromWire copies it.
    GodotVerse::FFieldStorage Storage;
    vh_value Wire{};
    const bool bDescribed = AutoRTFM::Open([&] {
        Verse::FRunningContext Context = Verse::FRunningContextPromise{};
        return GodotVerse::ReadSelfDescribingValue(Context, Value.GetValue(), Storage, Wire);
    });
    if (!bDescribed)
    {
        return;
    }
    OutValue = FromWire(Wire);
}

void VhSetValue(int64 Handle, verse::string const& Property, FGodotValue const& Value)
{
    if (RaiseIfDead(Handle, Property, TEXT("Wrote")))
    {
        return;
    }

    FUtf8String Name(ToView(Property));
    DeferToCommit([Handle, Name = MoveTemp(Name), Owned = Own(Value)] {
        FHostState& Host = GetHost();
        if (!Host.Godot.SetProperty)
        {
            return;
        }

        FWireStore Store;
        const vh_value Wire = Store.Wire(Owned);
        Host.Godot.SetProperty(Host.Godot.Ctx, Handle, reinterpret_cast<const char*>(*Name), Name.Len(), &Wire);
    });
}

// --- reference values ---------------------------------------------------------------------

void VhAdoptRef(TNonNullPtr<verse::godot_ref> Value)
{
    // Nothing to do but exist. Passing the object here is what materialises its UObject shadow,
    // and the shadow is what will release the id -- a Verse value that never crossed to native has
    // no UObject, so nothing would ever run for it.
    (void)Value;
}

void VhRefGet(int64 Ref, FGodotValue const& Key, TOptional<FGodotValue>& OutValue)
{
    OutValue.Reset();
    FHostState& Host = GetHost();
    if (!Host.Godot.RefGet)
    {
        return;
    }

    FWireStore Store;
    const vh_value WireKey = Store.Wire(Own(Key));
    FCallArena Arena;
    vh_value Result{};
    const int32 Status = CallGodot([&] {
        return Host.Godot.RefGet(Host.Godot.Ctx, Ref, &WireKey, &Arena, &Result);
    });
    if (Status == VH_CALL_NO_SUCH_MEMBER)
    {
        // An absent key or an index out of range: an ordinary Verse failure, not an error.
        return;
    }
    if (Status != VH_CALL_OK)
    {
        RaiseRefStatus(Status, Ref, TEXT("Read"));
        return;
    }
    OutValue = FromWire(Result);
}

/// A container write happens now, unlike every other mutation in the mirror, and it is the second
/// stated exception to "writes defer to commit" (signal emission is the other -- phase-4-design
/// 6.4, and Phase 4.5 audits the set).
///
/// Two reasons, and the first is that deferring was never consistent: VhRefGet and VhRefSize are
/// immediate, so a deferred write left a container disagreeing with itself inside one expression --
/// `A.SetInt(0, 5)` followed by `A.GetInt[0]` read the old value. The second is that it made a
/// container the script built itself useless, which is the whole of R-TYPE-2's other half: the
/// append that `MakeArray()` exists for is a write at the current size, and the call that consumes
/// the result happens before the commit that would have filled it.
void VhRefSet(int64 Ref, FGodotValue const& Key, FGodotValue const& Value)
{
    FHostState& Host = GetHost();
    if (!Host.Godot.RefSet)
    {
        return;
    }
    FWireStore Store;
    const vh_value WireKey = Store.Wire(Own(Key));
    const vh_value WireValue = Store.Wire(Own(Value));
    const int32 Status =
        CallGodot([&] { return Host.Godot.RefSet(Host.Godot.Ctx, Ref, &WireKey, &WireValue); });
    if (Status != VH_CALL_OK && Status != VH_CALL_NO_SUCH_MEMBER)
    {
        RaiseRefStatus(Status, Ref, TEXT("Wrote"));
    }
}

int64 VhRefSize(int64 Ref)
{
    FHostState& Host = GetHost();
    if (!Host.Godot.RefSize)
    {
        return 0;
    }
    int64 Size = 0;
    const int32 Status = CallGodot([&] { return Host.Godot.RefSize(Host.Godot.Ctx, Ref, &Size); });
    if (Status != VH_CALL_OK)
    {
        RaiseRefStatus(Status, Ref, TEXT("Sized"));
        return 0;
    }
    return Size;
}

int64 VhRefNew(int64 Tag)
{
    FHostState& Host = GetHost();
    return Host.Godot.NewRef ? CallGodot([&] { return Host.Godot.NewRef(Host.Godot.Ctx, (int32)Tag); }) : 0;
}

void VhRefInvoke(int64 Ref, TArray<FGodotValue> const& Args, FGodotValue& OutValue)
{
    OutValue = FGodotValue{};
    FHostState& Host = GetHost();
    if (!Host.Godot.InvokeCallable)
    {
        return;
    }

    FWireStore Store;
    TArray<vh_value> Wire;
    Wire.Reserve(Args.Num());
    for (const FGodotValue& Arg : Args)
    {
        Wire.Add(Store.Wire(Own(Arg)));
    }

    FCallArena Arena;
    vh_value Result{};
    const int32 Status = CallGodot([&] {
        return Host.Godot.InvokeCallable(Host.Godot.Ctx, Ref, Wire.GetData(), Wire.Num(), &Arena, &Result);
    });
    if (Status != VH_CALL_OK)
    {
        RaiseRefStatus(Status, Ref, TEXT("Called"));
        return;
    }
    OutValue = FromWire(Result);
}

void VhRefInvokeVoid(int64 Ref, TArray<FGodotValue> const& Args)
{
    FGodotValue Ignored;
    VhRefInvoke(Ref, Args, Ignored);
}

/// A method of the *builtin type* a reference names -- `Signal.emit`, `Callable.bind` and the rest
/// of what the mirror does not wrap.
///
/// `VhCallValue` cannot reach these and no spelling would have made it: it takes a vh_handle, which
/// names a Godot Object, and none of Godot's builtin types is one.
///
/// A name this build of Godot does not have raises, unlike a *container* read, where a miss is an
/// ordinary Verse failure: an absent key is data and a misspelled method is a bug.
void VhRefCall(int64 Ref, verse::string const& Method, TArray<FGodotValue> const& Args, FGodotValue& OutValue)
{
    OutValue = FGodotValue{};
    FHostState& Host = GetHost();
    if (!Host.Godot.RefCall)
    {
        return;
    }

    FWireStore Store;
    TArray<vh_value> Wire;
    Wire.Reserve(Args.Num());
    for (const FGodotValue& Arg : Args)
    {
        Wire.Add(Store.Wire(Own(Arg)));
    }

    const FUtf8StringView Name = ToView(Method);
    FCallArena Arena;
    vh_value Result{};
    const int32 Status = CallGodot([&] {
        return Host.Godot.RefCall(Host.Godot.Ctx, Ref, Bytes(Name), Name.Len(),
                                  Wire.GetData(), Wire.Num(), &Arena, &Result);
    });
    if (Status == VH_CALL_NO_SUCH_MEMBER)
    {
        RAISE_VERSE_RUNTIME_ERROR_FORMAT(
            Verse::ERuntimeDiagnostic::ErrRuntime_NativeInternal,
            TEXT("Godot has no method `%s` on the value reference %lld names."),
            *FString(Name), (long long)Ref);
        return;
    }
    if (Status != VH_CALL_OK)
    {
        RaiseRefStatus(Status, Ref, TEXT("Called a method on"));
        return;
    }
    OutValue = FromWire(Result);
}

void VhRefCallVoid(int64 Ref, verse::string const& Method, TArray<FGodotValue> const& Args)
{
    FGodotValue Ignored;
    VhRefCall(Ref, Method, Args, Ignored);
}

/// The whole container, as the vh_value sequence the bulk converters read.
///
/// Returns false when the host is not wired up or the id names nothing; the converters above turn
/// that into an empty array rather than a raise, because a container the script is iterating is
/// not the place to discover the bridge is down.
bool ReadRefContents(int64 Ref, GodotVerse::FCallArena& Arena, vh_value& OutValue)
{
    FHostState& Host = GetHost();
    if (!Host.Godot.RefContents)
    {
        return false;
    }
    const int32 Status = CallGodot([&] {
        return Host.Godot.RefContents(Host.Godot.Ctx, Ref, &Arena, &OutValue);
    });
    if (Status != VH_CALL_OK)
    {
        RaiseRefStatus(Status, Ref, TEXT("Read"));
        return false;
    }
    return true;
}

TArray<int64> VhRefInts(int64 Ref)
{
    TArray<int64> Out;
    FCallArena Arena;
    vh_value Contents{};
    if (!ReadRefContents(Ref, Arena, Contents))
    {
        return Out;
    }
    Out.Reserve(Contents.Seq.Count);
    for (int32 Index = 0; Index < Contents.Seq.Count; ++Index)
    {
        Out.Add(AsInt(Contents.Seq.Items[Index]));
    }
    return Out;
}

TArray<double> VhRefFloats(int64 Ref)
{
    TArray<double> Out;
    FCallArena Arena;
    vh_value Contents{};
    if (!ReadRefContents(Ref, Arena, Contents))
    {
        return Out;
    }

    // A packed array of vectors flattens: a PackedVector2Array of three is six floats, which is
    // what the Verse side reads it as. Each element arrives as its own tuple, so the components
    // are spliced rather than appended.
    for (int32 Index = 0; Index < Contents.Seq.Count; ++Index)
    {
        const vh_value& Item = Contents.Seq.Items[Index];
        if (Item.Type == VH_TYPE_TUPLE || Item.Type == VH_TYPE_ARRAY)
        {
            for (int32 Inner = 0; Inner < Item.Seq.Count; ++Inner)
            {
                Out.Add(AsDouble(Item.Seq.Items[Inner]));
            }
        }
        else
        {
            Out.Add(AsDouble(Item));
        }
    }
    return Out;
}

TArray<verse::string> VhRefStrings(int64 Ref)
{
    TArray<verse::string> Out;
    FCallArena Arena;
    vh_value Contents{};
    if (!ReadRefContents(Ref, Arena, Contents))
    {
        return Out;
    }
    Out.Reserve(Contents.Seq.Count);
    for (int32 Index = 0; Index < Contents.Seq.Count; ++Index)
    {
        const vh_value& Item = Contents.Seq.Items[Index];
        Out.Add(Item.Type == VH_TYPE_STRING
                    ? verse::string(GodotVerse::MakeView(Item.String.Utf8, Item.String.Len))
                    : verse::string{});
    }
    return Out;
}

void VhRefValues(int64 Ref, TArray<FGodotValue>& Out)
{
    Out.Reset();
    FCallArena Arena;
    vh_value Contents{};
    if (!ReadRefContents(Ref, Arena, Contents))
    {
        return;
    }
    Out.Reserve(Contents.Seq.Count);
    for (int32 Index = 0; Index < Contents.Seq.Count; ++Index)
    {
        Out.Add(FromWire(Contents.Seq.Items[Index]));
    }
}

/// Fills a fresh container of Tag with Items, and answers its id.
int64 NewRefFrom(int64 Tag, const TArray<vh_value>& Items)
{
    FHostState& Host = GetHost();
    if (!Host.Godot.NewRef || !Host.Godot.RefSet)
    {
        return 0;
    }
    const int64 Ref = CallGodot([&] { return Host.Godot.NewRef(Host.Godot.Ctx, (int32)Tag); });
    if (Ref == 0)
    {
        return 0;
    }

    // A fresh packed array has no room in it, so each element is appended by setting the index one
    // past the end -- which is what Godot's own resize-on-set does for these types.
    for (int32 Index = 0; Index < Items.Num(); ++Index)
    {
        vh_value Key{};
        Key.Type = VH_TYPE_INT;
        Key.VariantTag = VH_VARIANT_INT;
        Key.Int = Index;
        CallGodot([&] { return Host.Godot.RefSet(Host.Godot.Ctx, Ref, &Key, &Items[Index]); });
    }
    return Ref;
}

int64 VhRefFromInts(int64 Tag, TArray<int64> const& Values)
{
    TArray<vh_value> Items;
    Items.Reserve(Values.Num());
    for (int64 Value : Values)
    {
        vh_value& Item = Items.AddDefaulted_GetRef();
        Item.Type = VH_TYPE_INT;
        Item.VariantTag = VH_VARIANT_INT;
        Item.Int = Value;
    }
    return NewRefFrom(Tag, Items);
}

int64 VhRefFromFloats(int64 Tag, TArray<double> const& Values)
{
    TArray<vh_value> Items;
    Items.Reserve(Values.Num());
    for (double Value : Values)
    {
        vh_value& Item = Items.AddDefaulted_GetRef();
        Item.Type = VH_TYPE_FLOAT;
        Item.VariantTag = VH_VARIANT_FLOAT;
        Item.Float = Value;
    }
    return NewRefFrom(Tag, Items);
}

int64 VhRefFromStrings(int64 Tag, TArray<verse::string> const& Values)
{
    // The bytes have to outlive the vh_values that point at them, and a verse::string is not ours
    // to hold past this call.
    TArray<FUtf8String> Owned;
    Owned.Reserve(Values.Num());
    for (const verse::string& Value : Values)
    {
        Owned.Add(FUtf8String(ToView(Value)));
    }

    TArray<vh_value> Items;
    Items.Reserve(Owned.Num());
    for (const FUtf8String& Text : Owned)
    {
        vh_value& Item = Items.AddDefaulted_GetRef();
        Item.Type = VH_TYPE_STRING;
        Item.VariantTag = VH_VARIANT_STRING;
        Item.String.Utf8 = reinterpret_cast<const char*>(*Text);
        Item.String.Len = Text.Len();
    }
    return NewRefFrom(Tag, Items);
}

int64 VhRefFromValues(int64 Tag, TArray<FGodotValue> const& Values)
{
    FWireStore Store;
    TArray<vh_value> Items;
    Items.Reserve(Values.Num());
    for (const FGodotValue& Value : Values)
    {
        Items.Add(Store.Wire(Own(Value)));
    }
    return NewRefFrom(Tag, Items);
}

TOptional<int64> VhSingleton(verse::string const& Name)
{
    FHostState& Host = GetHost();
    if (!Host.Godot.GetSingleton)
    {
        return {};
    }

    const FUtf8StringView View = ToView(Name);
    const vh_handle Handle = CallGodot([&] { return Host.Godot.GetSingleton(Host.Godot.Ctx, Bytes(View), View.Len()); });
    if (Handle == 0)
    {
        return {};
    }
    return Handle;
}

}} // namespace verse::Godot
