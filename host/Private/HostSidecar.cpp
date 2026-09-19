// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostSidecar.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "host_build_id.gen.h"
#include "Dom/JsonValue.h"
#include "HostScript.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace {

/// Bumped when the shape below changes in a way a reader of the old shape would misread. The
/// cooker and the runtime host are built together and shipped together, so this is a tripwire
/// against a stale cook in a game directory rather than a compatibility mechanism.
constexpr int32 SidecarVersion = 8;

FString Utf8ToFString(const FUtf8String& Value)
{
    return FString(Value);
}

FUtf8String FStringToUtf8(const FString& Value)
{
    return FUtf8String(Value);
}

// ------------------------------------------------------------------- values --

/// A vh_value, recursively. The reader owns the bytes the value points at, which is what
/// FFieldStorage is for -- an array's items live in a block and the value holds a bare pointer
/// into it, so blocks are reserved before any is filled and none is ever resized. Writing is the
/// easy direction; reading has to rebuild that discipline.
TSharedPtr<FJsonObject> WriteValue(const vh_value& Value);

TSharedPtr<FJsonObject> WriteValue(const vh_value& Value)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("type"), Value.Type);
    Object->SetNumberField(TEXT("tag"), Value.VariantTag);
    switch (Value.Type)
    {
    case VH_TYPE_LOGIC:
        Object->SetBoolField(TEXT("v"), Value.Logic != 0);
        break;
    case VH_TYPE_INT:
        // As a string: a JSON number is a double, and an int64 past 2^53 would not survive one.
        Object->SetStringField(TEXT("v"), FString::Printf(TEXT("%lld"), (long long)Value.Int));
        break;
    case VH_TYPE_FLOAT:
        Object->SetNumberField(TEXT("v"), Value.Float);
        break;
    case VH_TYPE_CHAR:
        Object->SetNumberField(TEXT("v"), (double)Value.Char);
        break;
    case VH_TYPE_STRING:
        Object->SetStringField(TEXT("v"),
            FString(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Value.String.Utf8), Value.String.Len)));
        break;
    case VH_TYPE_ARRAY:
    case VH_TYPE_TUPLE:
    {
        TArray<TSharedPtr<FJsonValue>> Items;
        for (int32 Index = 0; Index < Value.Seq.Count; ++Index)
        {
            Items.Add(MakeShared<FJsonValueObject>(WriteValue(Value.Seq.Items[Index])));
        }
        Object->SetArrayField(TEXT("v"), Items);
        break;
    }
    case VH_TYPE_MAP:
    {
        TArray<TSharedPtr<FJsonValue>> Pairs;
        for (int32 Index = 0; Index < Value.Map.Count; ++Index)
        {
            TSharedPtr<FJsonObject> Pair = MakeShared<FJsonObject>();
            Pair->SetObjectField(TEXT("k"), WriteValue(Value.Map.Pairs[Index].Key));
            Pair->SetObjectField(TEXT("v"), WriteValue(Value.Map.Pairs[Index].Value));
            Pairs.Add(MakeShared<FJsonValueObject>(Pair));
        }
        Object->SetArrayField(TEXT("v"), Pairs);
        break;
    }
    case VH_TYPE_OPTION:
        if (Value.Option)
        {
            Object->SetObjectField(TEXT("v"), WriteValue(*Value.Option));
        }
        break;
    default:
        // VH_TYPE_VOID has no payload, and VH_TYPE_REF is an id in the *consumer's* table, which
        // means nothing in another process. A static holding one crosses as void rather than as a
        // dangling id; nothing in the mirror declares such a constant.
        break;
    }
    return Object;
}

/// Counts the blocks a value's sub-arrays will need, so Storage.Blocks can be reserved once.
int32 CountBlocks(const TSharedPtr<FJsonObject>& Object)
{
    if (!Object.IsValid())
    {
        return 0;
    }
    const int32 Type = (int32)Object->GetNumberField(TEXT("type"));
    int32 Count = 0;
    if (Type == VH_TYPE_ARRAY || Type == VH_TYPE_TUPLE)
    {
        Count = 1;
        const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        if (Object->TryGetArrayField(TEXT("v"), Items))
        {
            for (const TSharedPtr<FJsonValue>& Item : *Items)
            {
                Count += CountBlocks(Item->AsObject());
            }
        }
    }
    else if (Type == VH_TYPE_MAP)
    {
        Count = 1;
        const TArray<TSharedPtr<FJsonValue>>* Pairs = nullptr;
        if (Object->TryGetArrayField(TEXT("v"), Pairs))
        {
            for (const TSharedPtr<FJsonValue>& Pair : *Pairs)
            {
                const TSharedPtr<FJsonObject> PairObject = Pair->AsObject();
                if (PairObject.IsValid())
                {
                    Count += CountBlocks(PairObject->GetObjectField(TEXT("k")));
                    Count += CountBlocks(PairObject->GetObjectField(TEXT("v")));
                }
            }
        }
    }
    else if (Type == VH_TYPE_OPTION)
    {
        const TSharedPtr<FJsonObject>* Inner = nullptr;
        if (Object->TryGetObjectField(TEXT("v"), Inner))
        {
            Count = 1 + CountBlocks(*Inner);
        }
    }
    return Count;
}

/// Reads a value back into Storage. Blocks must already be reserved to their final count: a
/// vh_value holds a bare pointer into one, and growing the array would leave it dangling.
void ReadValue(const TSharedPtr<FJsonObject>& Object, GodotVerse::FFieldStorage& Storage, vh_value& OutValue)
{
    OutValue = vh_value{};
    if (!Object.IsValid())
    {
        return;
    }
    OutValue.Type = (int32)Object->GetNumberField(TEXT("type"));
    OutValue.VariantTag = (int32)Object->GetNumberField(TEXT("tag"));
    switch (OutValue.Type)
    {
    case VH_TYPE_LOGIC:
        OutValue.Logic = Object->GetBoolField(TEXT("v")) ? 1 : 0;
        break;
    case VH_TYPE_INT:
        OutValue.Int = FCString::Atoi64(*Object->GetStringField(TEXT("v")));
        break;
    case VH_TYPE_FLOAT:
        OutValue.Float = Object->GetNumberField(TEXT("v"));
        break;
    case VH_TYPE_CHAR:
        OutValue.Char = (uint32)Object->GetNumberField(TEXT("v"));
        break;
    case VH_TYPE_STRING:
    {
        const int32 Index = Storage.Strings.Add(FUtf8String(Object->GetStringField(TEXT("v"))));
        OutValue.String.Utf8 = reinterpret_cast<const char*>(*Storage.Strings[Index]);
        OutValue.String.Len = Storage.Strings[Index].Len();
        break;
    }
    case VH_TYPE_ARRAY:
    case VH_TYPE_TUPLE:
    {
        const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        Object->TryGetArrayField(TEXT("v"), Items);
        const int32 BlockIndex = Storage.Blocks.AddDefaulted();
        const int32 Count = Items ? Items->Num() : 0;
        Storage.Blocks[BlockIndex].SetNum(Count);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            vh_value Item{};
            ReadValue((*Items)[Index]->AsObject(), Storage, Item);
            Storage.Blocks[BlockIndex][Index] = Item;
        }
        OutValue.Seq.Items = Count > 0 ? Storage.Blocks[BlockIndex].GetData() : nullptr;
        OutValue.Seq.Count = Count;
        break;
    }
    case VH_TYPE_OPTION:
    {
        const TSharedPtr<FJsonObject>* Inner = nullptr;
        if (Object->TryGetObjectField(TEXT("v"), Inner))
        {
            const int32 BlockIndex = Storage.Blocks.AddDefaulted();
            Storage.Blocks[BlockIndex].SetNum(1);
            vh_value Item{};
            ReadValue(*Inner, Storage, Item);
            Storage.Blocks[BlockIndex][0] = Item;
            OutValue.Option = Storage.Blocks[BlockIndex].GetData();
        }
        break;
    }
    default:
        // VH_TYPE_MAP is not written for a static (see WriteValue), so nothing reads one back.
        break;
    }
}

// -------------------------------------------------------------- descriptors --

TSharedPtr<FJsonObject> WriteParam(const GodotVerse::FParamDesc& Param)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("name"), Utf8ToFString(Param.Name));
    Object->SetNumberField(TEXT("type"), (int32)Param.Type);
    Object->SetNumberField(TEXT("tag"), Param.VariantTag);
    Object->SetBoolField(TEXT("default"), Param.bHasDefault);
    Object->SetStringField(TEXT("class"), Utf8ToFString(Param.ClassName));
    Object->SetNumberField(TEXT("classKind"), Param.ClassKind);
    return Object;
}

GodotVerse::FParamDesc ReadParam(const TSharedPtr<FJsonObject>& Object)
{
    GodotVerse::FParamDesc Param;
    Param.Name = FStringToUtf8(Object->GetStringField(TEXT("name")));
    Param.Type = (vh_type)(int32)Object->GetNumberField(TEXT("type"));
    Param.VariantTag = (int32)Object->GetNumberField(TEXT("tag"));
    Param.bHasDefault = Object->GetBoolField(TEXT("default"));
    Param.ClassName = FStringToUtf8(Object->GetStringField(TEXT("class")));
    Param.ClassKind = (int32)Object->GetNumberField(TEXT("classKind"));
    return Param;
}

TSharedPtr<FJsonObject> WriteMethod(const GodotVerse::FMethodDesc& Method)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("name"), Utf8ToFString(Method.Name));
    Object->SetStringField(TEXT("decorated"), Utf8ToFString(Method.DecoratedName));
    TArray<TSharedPtr<FJsonValue>> Params;
    for (const GodotVerse::FParamDesc& Param : Method.Params)
    {
        Params.Add(MakeShared<FJsonValueObject>(WriteParam(Param)));
    }
    Object->SetArrayField(TEXT("params"), Params);
    Object->SetNumberField(TEXT("required"), Method.RequiredParamCount);
    Object->SetNumberField(TEXT("result"), (int32)Method.ResultType);
    Object->SetNumberField(TEXT("resultTag"), Method.ResultVariantTag);
    Object->SetStringField(TEXT("resultClass"), Utf8ToFString(Method.ResultClassName));
    Object->SetNumberField(TEXT("resultClassKind"), Method.ResultClassKind);
    Object->SetBoolField(TEXT("canFail"), Method.bCanFail);
    Object->SetBoolField(TEXT("suspends"), Method.bSuspends);
    Object->SetStringField(TEXT("virtual"), Utf8ToFString(Method.GodotVirtual));
    Object->SetNumberField(TEXT("line"), Method.Line);
    Object->SetNumberField(TEXT("column"), Method.Column);
    return Object;
}

GodotVerse::FMethodDesc ReadMethod(const TSharedPtr<FJsonObject>& Object)
{
    GodotVerse::FMethodDesc Method;
    Method.Name = FStringToUtf8(Object->GetStringField(TEXT("name")));
    Method.DecoratedName = FStringToUtf8(Object->GetStringField(TEXT("decorated")));
    const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
    if (Object->TryGetArrayField(TEXT("params"), Params))
    {
        for (const TSharedPtr<FJsonValue>& Param : *Params)
        {
            Method.Params.Add(ReadParam(Param->AsObject()));
        }
    }
    Method.RequiredParamCount = (int32)Object->GetNumberField(TEXT("required"));
    Method.ResultType = (vh_type)(int32)Object->GetNumberField(TEXT("result"));
    Method.ResultVariantTag = (int32)Object->GetNumberField(TEXT("resultTag"));
    Method.ResultClassName = FStringToUtf8(Object->GetStringField(TEXT("resultClass")));
    Method.ResultClassKind = (int32)Object->GetNumberField(TEXT("resultClassKind"));
    Method.bCanFail = Object->GetBoolField(TEXT("canFail"));
    Method.bSuspends = Object->GetBoolField(TEXT("suspends"));
    Method.GodotVirtual = FStringToUtf8(Object->GetStringField(TEXT("virtual")));
    Method.Line = (int32)Object->GetNumberField(TEXT("line"));
    Method.Column = (int32)Object->GetNumberField(TEXT("column"));
    return Method;
}

TSharedPtr<FJsonObject> WriteSignal(const GodotVerse::FSignalDesc& Signal)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("name"), Utf8ToFString(Signal.Name));
    TArray<TSharedPtr<FJsonValue>> Args;
    for (const GodotVerse::FParamDesc& Arg : Signal.Args)
    {
        Args.Add(MakeShared<FJsonValueObject>(WriteParam(Arg)));
    }
    Object->SetArrayField(TEXT("args"), Args);
    Object->SetNumberField(TEXT("line"), Signal.Line);
    Object->SetNumberField(TEXT("column"), Signal.Column);
    Object->SetNumberField(TEXT("reject"), Signal.Reject);
    Object->SetStringField(TEXT("rejectDetail"), Utf8ToFString(Signal.RejectDetail));
    return Object;
}

GodotVerse::FSignalDesc ReadSignal(const TSharedPtr<FJsonObject>& Object)
{
    GodotVerse::FSignalDesc Signal;
    Signal.Name = FStringToUtf8(Object->GetStringField(TEXT("name")));
    const TArray<TSharedPtr<FJsonValue>>* Args = nullptr;
    if (Object->TryGetArrayField(TEXT("args"), Args))
    {
        for (const TSharedPtr<FJsonValue>& Arg : *Args)
        {
            Signal.Args.Add(ReadParam(Arg->AsObject()));
        }
    }
    Signal.Line = (int32)Object->GetNumberField(TEXT("line"));
    Signal.Column = (int32)Object->GetNumberField(TEXT("column"));
    Signal.Reject = (int32)Object->GetNumberField(TEXT("reject"));
    Signal.RejectDetail = FStringToUtf8(Object->GetStringField(TEXT("rejectDetail")));
    return Signal;
}

// R-EXP-9's `@rpc`, which is version 6. An exported game reads its rpc config out of the
// snapshot like everything else, and a runtime host has no semantic program to have built one from
// -- so without this every `@rpc` in a shipped game was silently absent and Godot refused the call
// with "not marked for RPCs in the local script", naming a method the author had marked.
TSharedPtr<FJsonObject> WriteRpc(const GodotVerse::FRpcDesc& Rpc)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("name"), Utf8ToFString(Rpc.Name));
    Object->SetNumberField(TEXT("mode"), Rpc.RpcMode);
    Object->SetBoolField(TEXT("callLocal"), Rpc.bCallLocal);
    Object->SetNumberField(TEXT("transfer"), Rpc.TransferMode);
    Object->SetNumberField(TEXT("channel"), Rpc.Channel);
    Object->SetNumberField(TEXT("line"), Rpc.Line);
    Object->SetNumberField(TEXT("column"), Rpc.Column);
    Object->SetNumberField(TEXT("reject"), Rpc.Reject);
    Object->SetStringField(TEXT("rejectDetail"), Utf8ToFString(Rpc.RejectDetail));
    return Object;
}

GodotVerse::FRpcDesc ReadRpc(const TSharedPtr<FJsonObject>& Object)
{
    GodotVerse::FRpcDesc Rpc;
    Rpc.Name = FStringToUtf8(Object->GetStringField(TEXT("name")));
    Rpc.RpcMode = (int32)Object->GetNumberField(TEXT("mode"));
    Rpc.bCallLocal = Object->GetBoolField(TEXT("callLocal"));
    Rpc.TransferMode = (int32)Object->GetNumberField(TEXT("transfer"));
    Rpc.Channel = (int32)Object->GetNumberField(TEXT("channel"));
    Rpc.Line = (int32)Object->GetNumberField(TEXT("line"));
    Rpc.Column = (int32)Object->GetNumberField(TEXT("column"));
    Rpc.Reject = (int32)Object->GetNumberField(TEXT("reject"));
    Rpc.RejectDetail = FStringToUtf8(Object->GetStringField(TEXT("rejectDetail")));
    return Rpc;
}

TSharedPtr<FJsonObject> WriteExportImpl(const GodotVerse::FExportDesc& Export)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("name"), Utf8ToFString(Export.Name));
    Object->SetNumberField(TEXT("type"), (int32)Export.Type);
    Object->SetNumberField(TEXT("tag"), Export.VariantTag);
    Object->SetNumberField(TEXT("elementTag"), Export.ElementVariantTag);
    Object->SetBoolField(TEXT("isVar"), Export.bIsVar);
    Object->SetNumberField(TEXT("hint"), Export.Hint);
    Object->SetStringField(TEXT("hintString"), Utf8ToFString(Export.HintString));
    Object->SetStringField(TEXT("nativeClass"), Utf8ToFString(Export.NativeClass));
    Object->SetNumberField(TEXT("rangeMin"), Export.RangeMin);
    Object->SetNumberField(TEXT("rangeMax"), Export.RangeMax);
    Object->SetBoolField(TEXT("hasRangeMin"), Export.bHasRangeMin);
    Object->SetBoolField(TEXT("hasRangeMax"), Export.bHasRangeMax);
    Object->SetNumberField(TEXT("groupKind"), Export.GroupKind);
    Object->SetStringField(TEXT("groupName"), Utf8ToFString(Export.GroupName));
    Object->SetNumberField(TEXT("line"), Export.Line);
    Object->SetNumberField(TEXT("column"), Export.Column);
    Object->SetNumberField(TEXT("reject"), Export.Reject);
    return Object;
}

GodotVerse::FExportDesc ReadExportImpl(const TSharedPtr<FJsonObject>& Object)
{
    GodotVerse::FExportDesc Export;
    Export.Name = FStringToUtf8(Object->GetStringField(TEXT("name")));
    Export.Type = (vh_type)(int32)Object->GetNumberField(TEXT("type"));
    Export.VariantTag = (int32)Object->GetNumberField(TEXT("tag"));
    Export.ElementVariantTag = (int32)Object->GetNumberField(TEXT("elementTag"));
    Export.bIsVar = Object->GetBoolField(TEXT("isVar"));
    Export.Hint = (int32)Object->GetNumberField(TEXT("hint"));
    Export.HintString = FStringToUtf8(Object->GetStringField(TEXT("hintString")));
    Export.NativeClass = FStringToUtf8(Object->GetStringField(TEXT("nativeClass")));
    Export.RangeMin = Object->GetNumberField(TEXT("rangeMin"));
    Export.RangeMax = Object->GetNumberField(TEXT("rangeMax"));
    Export.bHasRangeMin = Object->GetBoolField(TEXT("hasRangeMin"));
    Export.bHasRangeMax = Object->GetBoolField(TEXT("hasRangeMax"));
    Export.GroupKind = (int32)Object->GetNumberField(TEXT("groupKind"));
    Export.GroupName = FStringToUtf8(Object->GetStringField(TEXT("groupName")));
    Export.Line = (int32)Object->GetNumberField(TEXT("line"));
    Export.Column = (int32)Object->GetNumberField(TEXT("column"));
    Export.Reject = (int32)Object->GetNumberField(TEXT("reject"));
    return Export;
}

TSharedPtr<FJsonObject> WriteStatics(const GodotVerse::FClassStatics& Statics)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Members;
    for (int32 Index = 0; Index < Statics.Statics.Num(); ++Index)
    {
        const GodotVerse::FStaticDesc& Static = Statics.Statics[Index];
        TSharedPtr<FJsonObject> Member = MakeShared<FJsonObject>();
        Member->SetStringField(TEXT("name"), Utf8ToFString(Static.Name));
        Member->SetBoolField(TEXT("isFunction"), Static.bIsFunction);
        Member->SetNumberField(TEXT("line"), Static.Line);
        Member->SetNumberField(TEXT("column"), Static.Column);
        if (!Static.bIsFunction && Statics.Values.IsValidIndex(Index))
        {
            Member->SetObjectField(TEXT("value"), WriteValue(Statics.Values[Index]));
        }
        Members.Add(MakeShared<FJsonValueObject>(Member));
    }
    Object->SetArrayField(TEXT("members"), Members);
    return Object;
}

TSharedRef<GodotVerse::FClassStatics> ReadStatics(const TSharedPtr<FJsonObject>& Object)
{
    TSharedRef<GodotVerse::FClassStatics> Statics = MakeShared<GodotVerse::FClassStatics>();
    const TArray<TSharedPtr<FJsonValue>>* Members = nullptr;
    if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("members"), Members))
    {
        return Statics;
    }

    // Sized before anything is filled, for the reason FClassStatics is one block: a vh_value in
    // Values points into the FFieldStorage at the same index, and growing either array afterwards
    // would move the bytes out from under it.
    const int32 Count = Members->Num();
    Statics->Statics.SetNum(Count);
    Statics->Values.SetNum(Count);
    Statics->Storage.SetNum(Count);

    for (int32 Index = 0; Index < Count; ++Index)
    {
        const TSharedPtr<FJsonObject> Member = (*Members)[Index]->AsObject();
        GodotVerse::FStaticDesc& Static = Statics->Statics[Index];
        Static.Name = FStringToUtf8(Member->GetStringField(TEXT("name")));
        Static.bIsFunction = Member->GetBoolField(TEXT("isFunction"));
        Static.Line = (int32)Member->GetNumberField(TEXT("line"));
        Static.Column = (int32)Member->GetNumberField(TEXT("column"));

        const TSharedPtr<FJsonObject>* Value = nullptr;
        if (Member->TryGetObjectField(TEXT("value"), Value))
        {
            GodotVerse::FFieldStorage& Storage = Statics->Storage[Index];
            Storage.Blocks.Reserve(CountBlocks(*Value));
            Storage.Strings.Reserve(CountBlocks(*Value) + 1);
            ReadValue(*Value, Storage, Statics->Values[Index]);
        }
    }
    return Statics;
}

} // namespace

// An `@export`'s description is also what every *declared type* carries (HostScript.cpp's
// FMemberType::Described), and the two have to agree field for field -- so there is one writer and
// one reader, here, where the rest of the sidecar's JSON lives.
AUTORTFM_DISABLE TSharedPtr<FJsonObject> GodotVerse::WriteExportDesc(const FExportDesc& Export)
{
    return WriteExportImpl(Export);
}

AUTORTFM_DISABLE GodotVerse::FExportDesc GodotVerse::ReadExportDesc(const TSharedPtr<FJsonObject>& Object)
{
    return ReadExportImpl(Object);
}

AUTORTFM_DISABLE bool GodotVerse::WriteClassSidecar(const FString& Path,
    const TArray<FString>& CookedPackages, int32 Generation, FUtf8String& OutError)
{
    const TSharedPtr<const FAnalysisSnapshot>& Snapshot = GetAnalysisSnapshot();
    if (!Snapshot.IsValid())
    {
        OutError = UTF8TEXT("there is no analysis snapshot to write; nothing was compiled");
        return false;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("version"), SidecarVersion);

    // The stamp (D6). The ABI version is the one that governs compatibility and is the one a
    // mismatch is most likely to be about; the host id is a digest of the host sources the cooker
    // was built from, so it moves when the cook's meaning could have moved and stays put across a
    // commit that touched only docs. The engine commit is recorded and never compared.
    Root->SetNumberField(TEXT("abi"), VH_ABI_VERSION);
    Root->SetStringField(TEXT("hostId"), TEXT(VH_BUILD_HOST_ID));
    Root->SetStringField(TEXT("engineCommit"), TEXT(VH_BUILD_ENGINE_COMMIT));
    Root->SetNumberField(TEXT("generation"), Generation);

    TArray<TSharedPtr<FJsonValue>> PackageList;
    for (const FString& PackagePath : CookedPackages)
    {
        PackageList.Add(MakeShared<FJsonValueString>(PackagePath));
    }
    Root->SetArrayField(TEXT("packages"), PackageList);

    // The class-to-binding table (R-INT-11). The *editor* enumerated it -- from ClassDB and from
    // Godot's global class list, neither of which a cook or an exported game has -- so it travels
    // in the sidecar beside the packages it describes. Without it a handle crosses as its nearest
    // mirrored ancestor and every cast to a binding declines, which is a wrong answer rather than
    // an error. Version 8 is this field.
    TArray<TSharedPtr<FJsonValue>> BindingList;
    for (const GodotVerse::FBindingClass& Binding : GodotVerse::GetBindingClasses())
    {
        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("verse"), Utf8ToFString(Binding.VerseClass));
        // Exactly one of the two keys is set, and which one decides how a handle is matched: a
        // ClassDB name is what GetClassOf answers, and a script's global name is what
        // GetScriptClassOf does, because GetClassOf answers the script's native base.
        if (!Binding.GodotClass.IsEmpty())
        {
            Row->SetStringField(TEXT("godot"), Utf8ToFString(Binding.GodotClass));
        }
        if (!Binding.ScriptClass.IsEmpty())
        {
            Row->SetStringField(TEXT("script"), Utf8ToFString(Binding.ScriptClass));
        }
        BindingList.Add(MakeShared<FJsonValueObject>(Row));
    }
    Root->SetArrayField(TEXT("bindings"), BindingList);

    // Collected here rather than in the snapshot: it describes the *mirror*, which does not change
    // between analyses, and walking 1036 classes is not something an editor's per-keystroke
    // analysis should pay for. The cook takes one of these, once.
    if (const TSharedPtr<GodotVerse::FEngineSignalTypes> EngineSignals = GodotVerse::CollectEngineSignalTypes())
    {
        Root->SetObjectField(TEXT("engineSignals"), GodotVerse::WriteEngineSignalTypes(*EngineSignals));
    }

    TSharedRef<FJsonObject> Classes = MakeShared<FJsonObject>();
    for (const TPair<FUtf8String, FAnalysisSnapshot::FClass>& Pair : Snapshot->Classes)
    {
        const FAnalysisSnapshot::FClass& Class = Pair.Value;
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetBoolField(TEXT("abstract"), Class.bAbstract);
        // An exported game's answer to "does this class exist" is this flag, so a class the cook
        // analysed but did not publish must not claim to be there.
        Entry->SetBoolField(TEXT("published"), Class.bInPublishedProgram);
        Entry->SetBoolField(TEXT("exportsHarvested"), Class.bExportsHarvested);
        // R-NODE-10's ToString. Written even when empty is wasteful, so it is written only when
        // there is one -- but it has to be *carried*, because an exported game has no semantic
        // program to find an extension method in and this name is the whole of what the lookup
        // needs. Version 5 is this field.
        if (!Class.ToStringDecorated.IsEmpty())
        {
            Entry->SetStringField(TEXT("toString"), FString(Class.ToStringDecorated));
        }

        TArray<TSharedPtr<FJsonValue>> Methods;
        for (const FMethodDesc& Method : Class.Methods)
        {
            Methods.Add(MakeShared<FJsonValueObject>(WriteMethod(Method)));
        }
        Entry->SetArrayField(TEXT("methods"), Methods);

        TArray<TSharedPtr<FJsonValue>> Signals;
        for (const FSignalDesc& Signal : Class.Signals)
        {
            Signals.Add(MakeShared<FJsonValueObject>(WriteSignal(Signal)));
        }
        Entry->SetArrayField(TEXT("signals"), Signals);

        TArray<TSharedPtr<FJsonValue>> Rpcs;
        for (const FRpcDesc& Rpc : Class.Rpcs)
        {
            Rpcs.Add(MakeShared<FJsonValueObject>(WriteRpc(Rpc)));
        }
        Entry->SetArrayField(TEXT("rpcs"), Rpcs);

        TArray<TSharedPtr<FJsonValue>> Exports;
        for (const FExportDesc& Export : Class.Exports)
        {
            Exports.Add(MakeShared<FJsonValueObject>(WriteExportImpl(Export)));
        }
        Entry->SetArrayField(TEXT("exports"), Exports);

        if (Class.Statics.IsValid())
        {
            Entry->SetObjectField(TEXT("statics"), WriteStatics(*Class.Statics));
        }

        // The declared types, which are the one thing in here a runtime host cannot re-derive from
        // anything it loads: the VM erases them and there is no semantic program to ask.
        if (Class.Types.IsValid())
        {
            Entry->SetObjectField(TEXT("types"), GodotVerse::WriteDeclaredTypes(*Class.Types));
        }

        Classes->SetObjectField(Utf8ToFString(Pair.Key), Entry);
    }
    Root->SetObjectField(TEXT("classes"), Classes);

    FString Text;
    TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text);
    if (!FJsonSerializer::Serialize(Root, Writer))
    {
        OutError = UTF8TEXT("the class sidecar could not be serialised");
        return false;
    }

    if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        OutError = FUtf8String(FString::Printf(TEXT("could not write %s"), *Path));
        return false;
    }
    return true;
}

namespace {

/// Reads the sidecar and checks its version, which is the half both readers share.
///
/// Two of D6's three refusals are here: a file that is not there names the export as incomplete,
/// and one this host cannot read names both versions. The third -- the stamp -- is in
/// ReadCookedManifest, because only the reader that runs before anything is loaded can act on it.
AUTORTFM_DISABLE bool ParseSidecar(const FString& Path, TSharedPtr<FJsonObject>& OutRoot, FUtf8String& OutError)
{
    if (!IFileManager::Get().FileExists(*Path))
    {
        OutError = FUtf8String(FString::Printf(
            TEXT("Verse data not found at %s. The export is incomplete; export the project again."),
            *Path));
        return false;
    }

    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *Path))
    {
        OutError = FUtf8String(FString::Printf(TEXT("could not read %s"), *Path));
        return false;
    }

    TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
    if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
    {
        OutError = FUtf8String(FString::Printf(TEXT("%s is not valid JSON"), *Path));
        return false;
    }

    const int32 Version = (int32)OutRoot->GetNumberField(TEXT("version"));
    if (Version != SidecarVersion)
    {
        OutError = FUtf8String(FString::Printf(
            TEXT("%s was written by sidecar version %d; this host reads version %d. Re-export the project."),
            *Path, Version, SidecarVersion));
        return false;
    }
    return true;
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::ReadCookedManifest(const FString& Path, TArray<FString>& OutPackages,
    int32& OutGeneration, FUtf8String& OutError)
{
    TSharedPtr<FJsonObject> Root;
    if (!ParseSidecar(Path, Root, OutError))
    {
        return false;
    }

    const int32 CookedAbi = (int32)Root->GetNumberField(TEXT("abi"));
    const FString CookedHostId = Root->GetStringField(TEXT("hostId"));
    if (CookedAbi != VH_ABI_VERSION || CookedHostId != TEXT(VH_BUILD_HOST_ID))
    {
        OutError = FUtf8String(FString::Printf(
            TEXT("This game's Verse data was cooked by a different build of godot-verse ")
            TEXT("(cooked %d/%s, host %d/%s). Export the project again."),
            CookedAbi, *CookedHostId.Left(7), VH_ABI_VERSION,
            *FString(TEXT(VH_BUILD_HOST_ID)).Left(7)));
        return false;
    }

    OutGeneration = (int32)Root->GetNumberField(TEXT("generation"));

    const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
    if (!Root->TryGetArrayField(TEXT("packages"), Items))
    {
        OutError = FUtf8String(FString::Printf(TEXT("%s names no cooked packages"), *Path));
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Item : *Items)
    {
        OutPackages.Add(Item->AsString());
    }
    return true;
}

AUTORTFM_DISABLE bool GodotVerse::LoadClassSidecar(const FString& Path, FUtf8StringView BindingsPackage,
    FUtf8String& OutError)
{
    TSharedPtr<FJsonObject> Root;
    if (!ParseSidecar(Path, Root, OutError))
    {
        return false;
    }

    // The binding table before the classes, because the caller has already mounted the package it
    // names and a cast can be asked for the moment the first script runs. A cook with no addons and
    // no `class_name` scripts writes an empty array, and an empty table is the right answer to
    // every question it is asked.
    const TArray<TSharedPtr<FJsonValue>>* BindingRows = nullptr;
    if (Root->TryGetArrayField(TEXT("bindings"), BindingRows))
    {
        TArray<FBindingClass> Bindings;
        for (const TSharedPtr<FJsonValue>& Row : *BindingRows)
        {
            const TSharedPtr<FJsonObject> Entry = Row->AsObject();
            if (!Entry.IsValid())
            {
                continue;
            }
            FBindingClass Binding;
            Binding.VerseClass = FStringToUtf8(Entry->GetStringField(TEXT("verse")));
            FString Named;
            if (Entry->TryGetStringField(TEXT("godot"), Named))
            {
                Binding.GodotClass = FStringToUtf8(Named);
            }
            if (Entry->TryGetStringField(TEXT("script"), Named))
            {
                Binding.ScriptClass = FStringToUtf8(Named);
            }
            Bindings.Add(MoveTemp(Binding));
        }
        GodotVerse::AdoptCookedBindings(MoveTemp(Bindings), BindingsPackage);
    }

    const TSharedPtr<FJsonObject>* EngineSignals = nullptr;
    if (Root->TryGetObjectField(TEXT("engineSignals"), EngineSignals))
    {
        GodotVerse::SetRecordedEngineSignalTypes(GodotVerse::ReadEngineSignalTypes(*EngineSignals));
    }

    TSharedRef<FAnalysisSnapshot> Snapshot = MakeShared<FAnalysisSnapshot>();
    const TSharedPtr<FJsonObject>* Classes = nullptr;
    if (Root->TryGetObjectField(TEXT("classes"), Classes))
    {
        for (const auto& Pair : (*Classes)->Values)
        {
            const TSharedPtr<FJsonObject> Entry = Pair.Value->AsObject();
            if (!Entry.IsValid())
            {
                continue;
            }
            FAnalysisSnapshot::FClass Class;
            Class.bAbstract = Entry->GetBoolField(TEXT("abstract"));
            Class.bInPublishedProgram = Entry->GetBoolField(TEXT("published"));
            Class.bExportsHarvested = Entry->GetBoolField(TEXT("exportsHarvested"));

            FString ToStringName;
            if (Entry->TryGetStringField(TEXT("toString"), ToStringName))
            {
                Class.ToStringDecorated = FUtf8String(ToStringName);
            }

            const TSharedPtr<FJsonObject>* Types = nullptr;
            if (Entry->TryGetObjectField(TEXT("types"), Types))
            {
                Class.Types = GodotVerse::ReadDeclaredTypes(*Types);
            }

            const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
            if (Entry->TryGetArrayField(TEXT("methods"), Items))
            {
                for (const TSharedPtr<FJsonValue>& Item : *Items)
                {
                    Class.Methods.Add(ReadMethod(Item->AsObject()));
                }
            }
            if (Entry->TryGetArrayField(TEXT("signals"), Items))
            {
                for (const TSharedPtr<FJsonValue>& Item : *Items)
                {
                    Class.Signals.Add(ReadSignal(Item->AsObject()));
                }
            }
            if (Entry->TryGetArrayField(TEXT("rpcs"), Items))
            {
                for (const TSharedPtr<FJsonValue>& Item : *Items)
                {
                    Class.Rpcs.Add(ReadRpc(Item->AsObject()));
                }
            }
            if (Entry->TryGetArrayField(TEXT("exports"), Items))
            {
                for (const TSharedPtr<FJsonValue>& Item : *Items)
                {
                    Class.Exports.Add(ReadExportImpl(Item->AsObject()));
                }
            }
            const TSharedPtr<FJsonObject>* Statics = nullptr;
            if (Entry->TryGetObjectField(TEXT("statics"), Statics))
            {
                Class.Statics = ReadStatics(*Statics);
            }

            Snapshot->Classes.Add(FStringToUtf8(FString(Pair.Key)), MoveTemp(Class));
        }
    }

    // False, and it is not an oversight: the AST half is ResolveUnknownName's, which needs a
    // compiler and is refused with VH_ERR_UNSUPPORTED before it could ask.
    Snapshot->bAstAvailable = false;

    SetAnalysisSnapshot(Snapshot);
    return true;
}
