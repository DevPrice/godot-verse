// Copyright Epic Games, Inc. All Rights Reserved.

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Utf8String.h"
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
                 "IsInstanceValid(...) before reaching through a reference the scene may have dropped."),
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

/// The C++ spelling of Verse's `variant`: (Tag, Ints, Floats, Texts).
using FGodotValue = verse::tuple<int64, TArray<int64>, TArray<double>, TArray<verse::string>>;

/// A variant detached from the VM. Deferred writes run after the transaction that produced
/// their arguments has committed, and a verse::string is not ours to hold across that boundary.
struct FOwnedValue
{
    int64 Tag = VH_VARIANT_NIL;
    TArray<int64> Ints;
    TArray<double> Floats;
    TArray<FUtf8String> Texts;
};

FOwnedValue Own(const FGodotValue& Value)
{
    FOwnedValue Owned;
    Owned.Tag = Value.Get<0>();
    Owned.Ints = Value.Get<1>();
    Owned.Floats = Value.Get<2>();
    for (const verse::string& Text : Value.Get<3>())
    {
        Owned.Texts.Add(FUtf8String(ToView(Text)));
    }
    return Owned;
}

/// Owns every buffer the vh_value trees it builds point at. Each block is allocated separately
/// because growing one array of blocks would move payloads that an already-returned vh_value
/// still points into.
class FWireStore
{
public:
    vh_value Wire(const FOwnedValue& Value);

private:
    vh_value* Block(int32 Count)
    {
        TUniquePtr<TArray<vh_value>>& Slot = Blocks.Add_GetRef(MakeUnique<TArray<vh_value>>());
        Slot->SetNumZeroed(Count);
        return Slot->GetData();
    }

    TArray<TUniquePtr<TArray<vh_value>>> Blocks;
};

vh_value FWireStore::Wire(const FOwnedValue& Value)
{
    const TArray<int64>& Ints = Value.Ints;
    const TArray<double>& Floats = Value.Floats;
    const TArray<FUtf8String>& Texts = Value.Texts;

    vh_value Out{};
    Out.VariantTag = static_cast<int32>(Value.Tag);

    const auto FloatSeq = [&](int32 Count) {
        vh_value* Items = Block(Count);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Items[Index].Type = VH_TYPE_FLOAT;
            Items[Index].Float = Floats.IsValidIndex(Index) ? Floats[Index] : 0.0;
        }
        Out.Seq.Items = Items;
        Out.Seq.Count = Count;
    };

    switch (Value.Tag)
    {
    case VH_VARIANT_BOOL:
        Out.Type = VH_TYPE_LOGIC;
        Out.Logic = (Ints.Num() > 0 && Ints[0] != 0) ? 1 : 0;
        break;

    case VH_VARIANT_INT:
    case VH_VARIANT_RID:
    case VH_VARIANT_OBJECT:
        Out.Type = VH_TYPE_INT;
        Out.Int = Ints.Num() > 0 ? Ints[0] : 0;
        break;

    case VH_VARIANT_FLOAT:
        Out.Type = VH_TYPE_FLOAT;
        Out.Float = Floats.Num() > 0 ? Floats[0] : 0.0;
        break;

    case VH_VARIANT_STRING:
    case VH_VARIANT_STRING_NAME:
    case VH_VARIANT_NODE_PATH:
        Out.Type = VH_TYPE_STRING;
        Out.String.Utf8 = Texts.Num() > 0 ? reinterpret_cast<const char*>(*Texts[0]) : "";
        Out.String.Len = Texts.Num() > 0 ? Texts[0].Len() : 0;
        break;

    case VH_VARIANT_VECTOR2:
    case VH_VARIANT_VECTOR2I:
        Out.Type = VH_TYPE_TUPLE;
        FloatSeq(2);
        break;

    case VH_VARIANT_VECTOR3:
    case VH_VARIANT_VECTOR3I:
        Out.Type = VH_TYPE_TUPLE;
        FloatSeq(3);
        break;

    case VH_VARIANT_VECTOR4:
    case VH_VARIANT_VECTOR4I:
    case VH_VARIANT_RECT2:
    case VH_VARIANT_RECT2I:
    case VH_VARIANT_COLOR:
    case VH_VARIANT_QUATERNION:
    case VH_VARIANT_PLANE:
        Out.Type = VH_TYPE_TUPLE;
        FloatSeq(4);
        break;

    case VH_VARIANT_AABB:
    case VH_VARIANT_TRANSFORM2D:
        Out.Type = VH_TYPE_TUPLE;
        FloatSeq(6);
        break;

    case VH_VARIANT_BASIS:
        Out.Type = VH_TYPE_TUPLE;
        FloatSeq(9);
        break;

    case VH_VARIANT_TRANSFORM3D:
        Out.Type = VH_TYPE_TUPLE;
        FloatSeq(12);
        break;

    case VH_VARIANT_PROJECTION:
        Out.Type = VH_TYPE_TUPLE;
        FloatSeq(16);
        break;

    case VH_VARIANT_PACKED_BYTE_ARRAY:
    case VH_VARIANT_PACKED_INT32_ARRAY:
    case VH_VARIANT_PACKED_INT64_ARRAY:
    {
        vh_value* Items = Block(Ints.Num());
        for (int32 Index = 0; Index < Ints.Num(); ++Index)
        {
            Items[Index].Type = VH_TYPE_INT;
            Items[Index].Int = Ints[Index];
        }
        Out.Type = VH_TYPE_ARRAY;
        Out.Seq.Items = Items;
        Out.Seq.Count = Ints.Num();
        break;
    }

    case VH_VARIANT_PACKED_FLOAT32_ARRAY:
    case VH_VARIANT_PACKED_FLOAT64_ARRAY:
    case VH_VARIANT_PACKED_VECTOR2_ARRAY:
    case VH_VARIANT_PACKED_VECTOR3_ARRAY:
    case VH_VARIANT_PACKED_COLOR_ARRAY:
        Out.Type = VH_TYPE_ARRAY;
        FloatSeq(Floats.Num());
        break;

    case VH_VARIANT_PACKED_STRING_ARRAY:
    {
        vh_value* Items = Block(Texts.Num());
        for (int32 Index = 0; Index < Texts.Num(); ++Index)
        {
            Items[Index].Type = VH_TYPE_STRING;
            Items[Index].String.Utf8 = reinterpret_cast<const char*>(*Texts[Index]);
            Items[Index].String.Len = Texts[Index].Len();
        }
        Out.Type = VH_TYPE_ARRAY;
        Out.Seq.Items = Items;
        Out.Seq.Count = Texts.Num();
        break;
    }

    // An untyped Array has no tag to disambiguate it, so the first non-empty payload wins.
    // Heterogeneous arrays have no variant spelling; see the digest.
    case VH_VARIANT_ARRAY:
        Out.Type = VH_TYPE_ARRAY;
        if (Ints.Num() > 0)
        {
            vh_value* Items = Block(Ints.Num());
            for (int32 Index = 0; Index < Ints.Num(); ++Index)
            {
                Items[Index].Type = VH_TYPE_INT;
                Items[Index].Int = Ints[Index];
            }
            Out.Seq.Items = Items;
            Out.Seq.Count = Ints.Num();
        }
        else
        {
            FloatSeq(Floats.Num());
        }
        break;

    default:
        Out.Type = VH_TYPE_VOID;
        break;
    }

    return Out;
}

/// Sorts a vh_value's payload into variant's three typed slots. A map has no variant
/// spelling and comes back as an empty value of its own tag.
FGodotValue FromWire(const vh_value& Value)
{
    TArray<int64> Ints;
    TArray<double> Floats;
    TArray<verse::string> Texts;

    const auto Scatter = [&](const vh_value& Item) {
        switch (Item.Type)
        {
        case VH_TYPE_LOGIC:
            Ints.Add(Item.Logic != 0 ? 1 : 0);
            break;
        case VH_TYPE_INT:
            Ints.Add(Item.Int);
            break;
        case VH_TYPE_FLOAT:
            Floats.Add(Item.Float);
            break;
        case VH_TYPE_STRING:
            Texts.Add(verse::string(GodotVerse::MakeView(Item.String.Utf8, Item.String.Len)));
            break;
        default:
            break;
        }
    };

    if (Value.Type == VH_TYPE_TUPLE || Value.Type == VH_TYPE_ARRAY)
    {
        for (int32 Index = 0; Index < Value.Seq.Count; ++Index)
        {
            Scatter(Value.Seq.Items[Index]);
        }
    }
    else
    {
        Scatter(Value);
    }

    return {static_cast<int64>(Value.VariantTag), Ints, Floats, Texts};
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
        Value.Get<0>(),
        reinterpret_cast<const char*>(*Name));
}

FGodotValue VhCallValue(int64 Handle, verse::string const& Method, TArray<FGodotValue> const& Args)
{
    FHostState& Host = GetHost();
    if (!Host.Godot.CallMethod)
    {
        RAISE_VERSE_RUNTIME_ERROR_CODE(Verse::ERuntimeDiagnostic::ErrRuntime_NativeInternal);
        return {};
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
        return {};
    }
    return FromWire(Result);
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

TOptional<FGodotValue> VhGetValue(int64 Handle, verse::string const& Property)
{
    FCallArena Arena;
    vh_value Value{};
    if (!ReadProperty(Handle, Property, Arena, Value))
    {
        return {};
    }
    return FromWire(Value);
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
