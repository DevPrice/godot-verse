// Copyright Epic Games, Inc. All Rights Reserved.

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Utf8String.h"
#include "HostRuntime.h"
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
    return Host.Godot.GetProperty(Host.Godot.Ctx, Handle, Bytes(Name), Name.Len(), &Arena, &OutValue) != 0;
}

/// Godot mutations are deferred to transaction commit: a Verse failure must not leave the scene
/// half-written. Outside a transaction AutoRTFM::OnCommit runs the callback immediately.
template <typename CallableType>
void DeferToCommit(CallableType&& Callable)
{
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

TOptional<int64> GetNode(verse::string const& Path)
{
    FHostState& Host = GetHost();
    if (!Host.Godot.GetNode)
    {
        return {};
    }

    const FUtf8StringView View = ToView(Path);
    const vh_handle Handle = Host.Godot.GetNode(Host.Godot.Ctx, Bytes(View), View.Len());
    if (Handle == 0)
    {
        return {};
    }
    return Handle;
}

bool IsValid(int64 Handle)
{
    FHostState& Host = GetHost();
    return Host.Godot.IsValid && Host.Godot.IsValid(Host.Godot.Ctx, Handle) != 0;
}

TArray<int64> GetChildren(int64 Handle)
{
    TArray<int64> Children;

    FHostState& Host = GetHost();
    if (!Host.Godot.GetChildCount || !Host.Godot.GetChild)
    {
        return Children;
    }

    const int32 Count = Host.Godot.GetChildCount(Host.Godot.Ctx, Handle);
    Children.Reserve(Count);
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const vh_handle Child = Host.Godot.GetChild(Host.Godot.Ctx, Handle, Index);
        if (Child != 0)
        {
            Children.Add(Child);
        }
    }
    return Children;
}

TOptional<double> GetFloat(int64 Handle, verse::string const& Property)
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

void SetFloat(int64 Handle, verse::string const& Property, double Value)
{
    vh_value Wire{};
    Wire.Type = VH_TYPE_FLOAT;
    Wire.Float = Value;
    WriteProperty(Handle, Property, Wire, FUtf8String());
}

TOptional<int64> GetInt(int64 Handle, verse::string const& Property)
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

void SetInt(int64 Handle, verse::string const& Property, int64 Value)
{
    vh_value Wire{};
    Wire.Type = VH_TYPE_INT;
    Wire.Int = Value;
    WriteProperty(Handle, Property, Wire, FUtf8String());
}

TOptional<bool> GetLogic(int64 Handle, verse::string const& Property)
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

void SetLogic(int64 Handle, verse::string const& Property, bool Value)
{
    vh_value Wire{};
    Wire.Type = VH_TYPE_LOGIC;
    Wire.Logic = Value ? 1 : 0;
    WriteProperty(Handle, Property, Wire, FUtf8String());
}

TOptional<verse::string> GetText(int64 Handle, verse::string const& Property)
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

void SetText(int64 Handle, verse::string const& Property, verse::string const& Value)
{
    vh_value Wire{};
    Wire.Type = VH_TYPE_STRING;
    WriteProperty(Handle, Property, Wire, FUtf8String(ToView(Value)));
}

TOptional<verse::tuple<double, double>> GetVector2(int64 Handle, verse::string const& Property)
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

void SetVector2(int64 Handle, verse::string const& Property, double X, double Y)
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

void CallMethod(int64 Handle, verse::string const& Method, TArray<double> const& Args)
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

TMap<verse::string, verse::string> GetMeta(int64 Handle)
{
    TMap<verse::string, verse::string> Meta;

    FHostState& Host = GetHost();
    if (!Host.Godot.GetMeta)
    {
        return Meta;
    }

    FCallArena Arena;
    vh_value Value{};
    if (!Host.Godot.GetMeta(Host.Godot.Ctx, Handle, &Arena, &Value))
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

TOptional<verse::char8> FindChar(verse::string const& Value, int64 Index)
{
    const FUtf8StringView View = ToView(Value);
    if (Index < 0 || Index >= View.Len())
    {
        return {};
    }
    return static_cast<verse::char8>(View[static_cast<int32>(Index)]);
}

}} // namespace verse::Godot
