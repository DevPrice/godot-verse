// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostSidecar.h"

#include "Dom/JsonObject.h"
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
constexpr int32 SidecarVersion = 1;

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
    return Object;
}

GodotVerse::FParamDesc ReadParam(const TSharedPtr<FJsonObject>& Object)
{
    GodotVerse::FParamDesc Param;
    Param.Name = FStringToUtf8(Object->GetStringField(TEXT("name")));
    Param.Type = (vh_type)(int32)Object->GetNumberField(TEXT("type"));
    Param.VariantTag = (int32)Object->GetNumberField(TEXT("tag"));
    Param.bHasDefault = Object->GetBoolField(TEXT("default"));
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

TSharedPtr<FJsonObject> WriteExport(const GodotVerse::FExportDesc& Export)
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

GodotVerse::FExportDesc ReadExport(const TSharedPtr<FJsonObject>& Object)
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

AUTORTFM_DISABLE bool GodotVerse::WriteClassSidecar(const FString& Path, FUtf8String& OutError)
{
    const TSharedPtr<const FAnalysisSnapshot>& Snapshot = GetAnalysisSnapshot();
    if (!Snapshot.IsValid())
    {
        OutError = UTF8TEXT("there is no analysis snapshot to write; nothing was compiled");
        return false;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("version"), SidecarVersion);

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

        TArray<TSharedPtr<FJsonValue>> Exports;
        for (const FExportDesc& Export : Class.Exports)
        {
            Exports.Add(MakeShared<FJsonValueObject>(WriteExport(Export)));
        }
        Entry->SetArrayField(TEXT("exports"), Exports);

        if (Class.Statics.IsValid())
        {
            Entry->SetObjectField(TEXT("statics"), WriteStatics(*Class.Statics));
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

AUTORTFM_DISABLE bool GodotVerse::LoadClassSidecar(const FString& Path, FUtf8String& OutError)
{
    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *Path))
    {
        OutError = FUtf8String(FString::Printf(TEXT("could not read %s"), *Path));
        return false;
    }

    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        OutError = FUtf8String(FString::Printf(TEXT("%s is not valid JSON"), *Path));
        return false;
    }

    const int32 Version = (int32)Root->GetNumberField(TEXT("version"));
    if (Version != SidecarVersion)
    {
        OutError = FUtf8String(FString::Printf(
            TEXT("%s was written by sidecar version %d; this host reads version %d. Re-export the project."),
            *Path, Version, SidecarVersion));
        return false;
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
            if (Entry->TryGetArrayField(TEXT("exports"), Items))
            {
                for (const TSharedPtr<FJsonValue>& Item : *Items)
                {
                    Class.Exports.Add(ReadExport(Item->AsObject()));
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
