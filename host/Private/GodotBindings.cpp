// Copyright Epic Games, Inc. All Rights Reserved.

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Utf8String.h"
#include "HostRuntime.h"
#include "Templates/UniquePtr.h"
#include "VerseString.h"
#include "VerseValue.h"

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

/// Reads one property through the Godot callback table. Returns false if the property is
/// missing, the host is not wired up, or the value did not come back at all.
bool ReadProperty(int64 Handle, const verse::string& Property, FCallArena& Arena, vh_value& OutValue)
{
    FHostState& Host = GetHost();
    if (!Host.Godot.GetProperty)
    {
        return false;
    }

    const FUtf8StringView Name = ToView(Property);
    return CallGodot([&] {
        return Host.Godot.GetProperty(Host.Godot.Ctx, Handle, Bytes(Name), Name.Len(), &Arena, &OutValue) != 0;
    });
}

/// Godot mutations are deferred to transaction commit: a Verse failure must not leave the scene
/// half-written. Outside a transaction AutoRTFM::OnCommit runs the callback immediately.
template <typename CallableType>
void DeferToCommit(CallableType&& Callable)
{
    // Commit handlers already run outside the transaction, so no Open is needed (nor allowed) here.
    AutoRTFM::OnCommit(Forward<CallableType>(Callable));
}

void WriteProperty(int64 Handle, const verse::string& Property, vh_value Value, FUtf8String OwnedText)
{
    FUtf8String Name(ToView(Property));
    DeferToCommit([Handle, Name = MoveTemp(Name), Value, Text = MoveTemp(OwnedText)]() mutable {
        FHostState& Host = GetHost();
        if (!Host.Godot.SetProperty)
        {
            return;
        }
        if (Value.Type == VH_TYPE_STRING)
        {
            Value.String.Utf8 = reinterpret_cast<const char*>(*Text);
            Value.String.Len = Text.Len();
        }
        Host.Godot.SetProperty(Host.Godot.Ctx, Handle, reinterpret_cast<const char*>(*Name), Name.Len(), &Value);
    });
}

/// The C++ spelling of Verse's `godot_value`: (Tag, Ints, Floats, Texts).
using FGodotValue = verse::tuple<int64, TArray<int64>, TArray<double>, TArray<verse::string>>;

/// A godot_value detached from the VM. Deferred writes run after the transaction that produced
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
    // Heterogeneous arrays have no godot_value spelling; see the digest.
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

/// Sorts a vh_value's payload into godot_value's three typed slots. A map has no godot_value
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

TOptional<int64> VhGetNode(verse::string const& Path)
{
    FHostState& Host = GetHost();
    if (!Host.Godot.GetNode)
    {
        return {};
    }

    const FUtf8StringView View = ToView(Path);
    const vh_handle Handle = CallGodot([&] { return Host.Godot.GetNode(Host.Godot.Ctx, Bytes(View), View.Len()); });
    if (Handle == 0)
    {
        return {};
    }
    return Handle;
}

bool VhIsValid(int64 Handle)
{
    FHostState& Host = GetHost();
    return Host.Godot.IsValid && CallGodot([&] { return Host.Godot.IsValid(Host.Godot.Ctx, Handle) != 0; });
}

TArray<int64> VhGetChildren(int64 Handle)
{
    TArray<int64> Children;

    FHostState& Host = GetHost();
    if (!Host.Godot.GetChildCount || !Host.Godot.GetChild)
    {
        return Children;
    }

    const int32 Count = CallGodot([&] { return Host.Godot.GetChildCount(Host.Godot.Ctx, Handle); });
    Children.Reserve(Count);
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const vh_handle Child = CallGodot([&] { return Host.Godot.GetChild(Host.Godot.Ctx, Handle, Index); });
        if (Child != 0)
        {
            Children.Add(Child);
        }
    }
    return Children;
}

TOptional<double> VhGetFloat(int64 Handle, verse::string const& Property)
{
    FCallArena Arena;
    vh_value Value{};
    if (!ReadProperty(Handle, Property, Arena, Value))
    {
        return {};
    }
    if (Value.Type == VH_TYPE_FLOAT)
    {
        return Value.Float;
    }
    if (Value.Type == VH_TYPE_INT)
    {
        return static_cast<double>(Value.Int);
    }
    return {};
}

void VhSetFloat(int64 Handle, verse::string const& Property, double Value)
{
    vh_value Wire{};
    Wire.Type = VH_TYPE_FLOAT;
    Wire.Float = Value;
    WriteProperty(Handle, Property, Wire, FUtf8String());
}

TOptional<int64> VhGetInt(int64 Handle, verse::string const& Property)
{
    FCallArena Arena;
    vh_value Value{};
    if (!ReadProperty(Handle, Property, Arena, Value))
    {
        return {};
    }
    if (Value.Type == VH_TYPE_INT)
    {
        return Value.Int;
    }
    if (Value.Type == VH_TYPE_FLOAT)
    {
        return static_cast<int64>(Value.Float);
    }
    return {};
}

void VhSetInt(int64 Handle, verse::string const& Property, int64 Value)
{
    vh_value Wire{};
    Wire.Type = VH_TYPE_INT;
    Wire.Int = Value;
    WriteProperty(Handle, Property, Wire, FUtf8String());
}

TOptional<bool> VhGetLogic(int64 Handle, verse::string const& Property)
{
    FCallArena Arena;
    vh_value Value{};
    if (!ReadProperty(Handle, Property, Arena, Value))
    {
        return {};
    }
    if (Value.Type != VH_TYPE_LOGIC)
    {
        return {};
    }
    return Value.Logic != 0;
}

void VhSetLogic(int64 Handle, verse::string const& Property, bool Value)
{
    vh_value Wire{};
    Wire.Type = VH_TYPE_LOGIC;
    Wire.Logic = Value ? 1 : 0;
    WriteProperty(Handle, Property, Wire, FUtf8String());
}

TOptional<verse::string> VhGetText(int64 Handle, verse::string const& Property)
{
    FCallArena Arena;
    vh_value Value{};
    if (!ReadProperty(Handle, Property, Arena, Value))
    {
        return {};
    }
    if (Value.Type != VH_TYPE_STRING)
    {
        return {};
    }
    return verse::string(GodotVerse::MakeView(Value.String.Utf8, Value.String.Len));
}

void VhSetText(int64 Handle, verse::string const& Property, verse::string const& Value)
{
    vh_value Wire{};
    Wire.Type = VH_TYPE_STRING;
    WriteProperty(Handle, Property, Wire, FUtf8String(ToView(Value)));
}

TOptional<verse::tuple<double, double>> VhGetVector2(int64 Handle, verse::string const& Property)
{
    FCallArena Arena;
    vh_value Value{};
    if (!ReadProperty(Handle, Property, Arena, Value))
    {
        return {};
    }
    if (Value.Type != VH_TYPE_TUPLE || Value.Seq.Count != 2 || Value.Seq.Items == nullptr)
    {
        return {};
    }
    if (Value.Seq.Items[0].Type != VH_TYPE_FLOAT || Value.Seq.Items[1].Type != VH_TYPE_FLOAT)
    {
        return {};
    }
    return verse::tuple<double, double>(Value.Seq.Items[0].Float, Value.Seq.Items[1].Float);
}

void VhSetVector2(int64 Handle, verse::string const& Property, double X, double Y)
{
    FUtf8String Name(ToView(Property));
    DeferToCommit([Handle, Name = MoveTemp(Name), X, Y] {
        FHostState& Host = GetHost();
        if (!Host.Godot.SetProperty)
        {
            return;
        }

        vh_value Items[2]{};
        Items[0].Type = VH_TYPE_FLOAT;
        Items[0].Float = X;
        Items[1].Type = VH_TYPE_FLOAT;
        Items[1].Float = Y;

        vh_value Wire{};
        Wire.Type = VH_TYPE_TUPLE;
        Wire.Seq.Items = Items;
        Wire.Seq.Count = 2;

        Host.Godot.SetProperty(Host.Godot.Ctx, Handle, reinterpret_cast<const char*>(*Name), Name.Len(), &Wire);
    });
}

void VhCallMethod(int64 Handle, verse::string const& Method, TArray<double> const& Args)
{
    FUtf8String Name(ToView(Method));
    TArray<double> OwnedArgs(Args);
    DeferToCommit([Handle, Name = MoveTemp(Name), OwnedArgs = MoveTemp(OwnedArgs)] {
        FHostState& Host = GetHost();
        if (!Host.Godot.CallMethod)
        {
            return;
        }

        TArray<vh_value> Wire;
        Wire.Reserve(OwnedArgs.Num());
        for (double Arg : OwnedArgs)
        {
            vh_value Value{};
            Value.Type = VH_TYPE_FLOAT;
            Value.Float = Arg;
            Wire.Add(Value);
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

TMap<verse::string, verse::string> VhGetMeta(int64 Handle)
{
    TMap<verse::string, verse::string> Meta;

    FHostState& Host = GetHost();
    if (!Host.Godot.GetMeta)
    {
        return Meta;
    }

    FCallArena Arena;
    vh_value Value{};
    if (!CallGodot([&] { return Host.Godot.GetMeta(Host.Godot.Ctx, Handle, &Arena, &Value) != 0; }))
    {
        return Meta;
    }
    if (Value.Type != VH_TYPE_MAP || Value.Map.Pairs == nullptr)
    {
        return Meta;
    }

    Meta.Reserve(Value.Map.Count);
    for (int32 Index = 0; Index < Value.Map.Count; ++Index)
    {
        const vh_pair& Pair = Value.Map.Pairs[Index];
        if (Pair.Key.Type != VH_TYPE_STRING || Pair.Value.Type != VH_TYPE_STRING)
        {
            continue;
        }
        Meta.Add(verse::string(GodotVerse::MakeView(Pair.Key.String.Utf8, Pair.Key.String.Len)),
                 verse::string(GodotVerse::MakeView(Pair.Value.String.Utf8, Pair.Value.String.Len)));
    }
    return Meta;
}

TOptional<FGodotValue> VhCallValue(int64 Handle, verse::string const& Method, TArray<FGodotValue> const& Args)
{
    FHostState& Host = GetHost();
    if (!Host.Godot.CallMethod)
    {
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
    const bool bCalled = CallGodot([&] {
        return Host.Godot.CallMethod(
                   Host.Godot.Ctx, Handle, Bytes(Name), Name.Len(), Wire.GetData(), Wire.Num(), &Arena, &Result)
            != 0;
    });
    if (!bCalled)
    {
        return {};
    }
    return FromWire(Result);
}

void VhCallVoid(int64 Handle, verse::string const& Method, TArray<FGodotValue> const& Args)
{
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

TOptional<int64> VhInstantiate(verse::string const& ClassName)
{
    FHostState& Host = GetHost();
    if (!Host.Godot.Instantiate)
    {
        return {};
    }

    const FUtf8StringView Name = ToView(ClassName);
    const vh_handle Handle = CallGodot([&] { return Host.Godot.Instantiate(Host.Godot.Ctx, Bytes(Name), Name.Len()); });
    if (Handle == 0)
    {
        return {};
    }
    return Handle;
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

TOptional<verse::char8> VhFindChar(verse::string const& Value, int64 Index)
{
    const FUtf8StringView View = ToView(Value);
    if (Index < 0 || Index >= View.Len())
    {
        return {};
    }
    return static_cast<verse::char8>(View[static_cast<int32>(Index)]);
}

}} // namespace verse::Godot
