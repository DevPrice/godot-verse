// Copyright Epic Games, Inc. All Rights Reserved.

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Utf8String.h"
#include "GodotClasses.h"
#include "HostRuntime.h"
#include "Templates/UniquePtr.h"
#include "VerseString.h"
#include "VerseValue.h"
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
