// Copyright Epic Games, Inc. All Rights Reserved.

// verse_host_unit's entry point (docs/architecture-review.md item 3 step 4): host/Private linked
// with a test main, so the units item 5 split out of HostScript.cpp -- the type model, the
// converters, the sidecar, the failure-to-status map and the engine adapters -- are asked
// directly, about types the host compiled out of tests/host_unit, with no Godot anywhere.
//
// build_host.py stages this file into Private/ for the VerseHostUnit target alone, and
// VerseHostUnit.Target.cs defines VH_HOST_UNIT; the guard is for a staged tree some other target
// happens to find it in.

#include "verse_host_abi.h"

#if defined(VH_HOST_UNIT)

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HostEngineAdapters.h"
#include "HostMarshal.h"
#include "HostResult.h"
#include "HostRuntime.h"
#include "HostScript.h"
#include "HostScriptState.h"
#include "HostSidecar.h"
#include "HostSnapshot.h"
#include "HostTypeModel.h"
#include "HostVerseEntry.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ULangUEUtils.h"
#include "uLang/Semantics/DataDefinition.h"
#include "uLang/Semantics/SemanticClass.h"
#include "uLang/Semantics/SemanticFunction.h"
#include "uLang/Semantics/SemanticProgram.h"
#include "uLang/Semantics/SemanticTypes.h"
#include "VerseVM/Inline/VVMValueInline.h"
#include "VerseVM/VVMValueObject.h"

#include <cstdio>

// Named rather than anonymous: a unity build puts this file in one translation unit with the rest of
// Private/, whose anonymous namespaces already hold helpers with names as short as these.
namespace GodotVerseUnit {

using namespace GodotVerse;

int32 GPassed = 0;
int32 GFailed = 0;

void Say(const FString& Line)
{
	const FTCHARToUTF8 Utf8(*(Line + TEXT("\n")));
	fwrite(Utf8.Get(), 1, Utf8.Length(), stdout);
	fflush(stdout);
}

void Case(const FString& Name, bool bOk, const FString& Detail = FString())
{
	if (bOk)
	{
		++GPassed;
		Say(FString::Printf(TEXT("[host_unit] %s: ok"), *Name));
		return;
	}
	++GFailed;
	Say(Detail.IsEmpty() ? FString::Printf(TEXT("[host_unit] %s: FAIL"), *Name)
	                     : FString::Printf(TEXT("[host_unit] %s: FAIL (%s)"), *Name, *Detail));
}

void Expect(const FString& Name, const FString& Got, const FString& Expected)
{
	Case(Name, Got == Expected, FString::Printf(TEXT("got %s, expected %s"), *Got, *Expected));
}

void ExpectInt(const FString& Name, int64 Got, int64 Expected)
{
	Case(Name, Got == Expected, FString::Printf(TEXT("got %lld, expected %lld"), Got, Expected));
}

FString Str(const FUtf8String& Text)
{
	return FString(Text);
}

// Exhaustive, so a kind added to EDeclaredKind fails this file's build until the test names it.
const TCHAR* KindName(EDeclaredKind Kind)
{
	VH_EXHAUSTIVE_SWITCH_BEGIN
	switch (Kind)
	{
	case EDeclaredKind::Other: return TEXT("Other");
	case EDeclaredKind::Logic: return TEXT("Logic");
	case EDeclaredKind::Int: return TEXT("Int");
	case EDeclaredKind::Float: return TEXT("Float");
	case EDeclaredKind::Char: return TEXT("Char");
	case EDeclaredKind::String: return TEXT("String");
	case EDeclaredKind::Enum: return TEXT("Enum");
	case EDeclaredKind::Array: return TEXT("Array");
	case EDeclaredKind::Map: return TEXT("Map");
	case EDeclaredKind::Tuple: return TEXT("Tuple");
	case EDeclaredKind::Option: return TEXT("Option");
	case EDeclaredKind::Variant: return TEXT("Variant");
	case EDeclaredKind::Rid: return TEXT("Rid");
	case EDeclaredKind::MathStruct: return TEXT("MathStruct");
	case EDeclaredKind::UserStruct: return TEXT("UserStruct");
	case EDeclaredKind::Container: return TEXT("Container");
	case EDeclaredKind::TypedContainer: return TEXT("TypedContainer");
	case EDeclaredKind::Reference: return TEXT("Reference");
	case EDeclaredKind::OtherClass: return TEXT("OtherClass");
	}
	VH_EXHAUSTIVE_SWITCH_END
	return TEXT("?");
}

const TCHAR* OriginName(EClassOrigin Origin)
{
	VH_EXHAUSTIVE_SWITCH_BEGIN
	switch (Origin)
	{
	case EClassOrigin::Other: return TEXT("Other");
	case EClassOrigin::Mirrored: return TEXT("Mirrored");
	case EClassOrigin::Script: return TEXT("Script");
	case EClassOrigin::Binding: return TEXT("Binding");
	}
	VH_EXHAUSTIVE_SWITCH_END
	return TEXT("?");
}

/// What StatusFor must answer, restated rather than read back: the point is that the mapping
/// changes only when someone changes this table too. Unset for a value past the last enumerator,
/// which is how the loop below knows where the enum ends. bNew is a consumer at 12.4 or later.
TOptional<int32> ExpectedStatus(EHostFailure Failure, bool bNew, const TCHAR*& OutName)
{
	VH_EXHAUSTIVE_SWITCH_BEGIN
	switch (Failure)
	{
	case EHostFailure::NotAnalysed: OutName = TEXT("NotAnalysed"); return bNew ? VH_ERR_NOT_ANALYSED : VH_ERR_NOT_FOUND;
	case EHostFailure::AnalysisRunning: OutName = TEXT("AnalysisRunning"); return bNew ? VH_ERR_NOT_ANALYSED : VH_ERR_STATE;
	case EHostFailure::BuiltSinceAnalysis: OutName = TEXT("BuiltSinceAnalysis"); return bNew ? VH_ERR_NOT_ANALYSED : VH_ERR_STATE;
	case EHostFailure::BufferNotAnalysed: OutName = TEXT("BufferNotAnalysed"); return bNew ? VH_ERR_NOT_ANALYSED : VH_ERR_STATE;
	case EHostFailure::InvalidPosition: OutName = TEXT("InvalidPosition"); return VH_ERR_NOT_FOUND;
	case EHostFailure::NothingAtPosition: OutName = TEXT("NothingAtPosition"); return VH_ERR_NOT_FOUND;
	case EHostFailure::NotAFunction: OutName = TEXT("NotAFunction"); return VH_ERR_NOT_FOUND;
	case EHostFailure::NoSuchClass: OutName = TEXT("NoSuchClass"); return VH_ERR_NOT_FOUND;
	case EHostFailure::NotPublished: OutName = TEXT("NotPublished"); return VH_ERR_ARGUMENT;
	case EHostFailure::TypeMismatch: OutName = TEXT("TypeMismatch"); return VH_ERR_ARGUMENT;
	case EHostFailure::Unconvertible: OutName = TEXT("Unconvertible"); return VH_ERR_ARGUMENT;
	case EHostFailure::NullNotOptional: OutName = TEXT("NullNotOptional"); return VH_ERR_ARGUMENT;
	case EHostFailure::EnumOrdinalOutOfRange: OutName = TEXT("EnumOrdinalOutOfRange"); return VH_ERR_ARGUMENT;
	case EHostFailure::MissingField: OutName = TEXT("MissingField"); return VH_ERR_ARGUMENT;
	case EHostFailure::ConstructionFailed: OutName = TEXT("ConstructionFailed"); return VH_ERR_ARGUMENT;
	case EHostFailure::CallbackMissing: OutName = TEXT("CallbackMissing"); return VH_ERR_ARGUMENT;
	case EHostFailure::CallbackFailed: OutName = TEXT("CallbackFailed"); return VH_ERR_ARGUMENT;
	case EHostFailure::UnknownId: OutName = TEXT("UnknownId"); return VH_ERR_NOT_FOUND;
	case EHostFailure::NotASignal: OutName = TEXT("NotASignal"); return VH_ERR_NOT_FOUND;
	case EHostFailure::Aborted: OutName = TEXT("Aborted"); return VH_ERR_RUNTIME;
	case EHostFailure::Halted: OutName = TEXT("Halted"); return VH_ERR_HALTED;
	case EHostFailure::Declined: OutName = TEXT("Declined"); return VH_ERR_FAILED;
	case EHostFailure::InstanceReleased: OutName = TEXT("InstanceReleased"); return VH_ERR_STATE;
	case EHostFailure::NoSuchMember: OutName = TEXT("NoSuchMember"); return VH_ERR_NOT_FOUND;
	case EHostFailure::Unset: OutName = TEXT("Unset"); return VH_ERR_NOT_FOUND;
	case EHostFailure::NotAssignable: OutName = TEXT("NotAssignable"); return VH_ERR_NOT_FOUND;
	case EHostFailure::NoSuchMethod: OutName = TEXT("NoSuchMethod"); return VH_ERR_NOT_FOUND;
	case EHostFailure::SignatureNotRecorded: OutName = TEXT("SignatureNotRecorded"); return VH_ERR_NOT_FOUND;
	case EHostFailure::WrongArgumentCount: OutName = TEXT("WrongArgumentCount"); return VH_ERR_ARGUMENT;
	case EHostFailure::GodotUnavailable: OutName = TEXT("GodotUnavailable"); return VH_ERR_STATE;
	case EHostFailure::NotBuilt: OutName = TEXT("NotBuilt"); return VH_ERR_STATE;
	case EHostFailure::AnalysisInFlight: OutName = TEXT("AnalysisInFlight"); return VH_ERR_STATE;
	case EHostFailure::AnalysisNotReaped: OutName = TEXT("AnalysisNotReaped"); return VH_ERR_STATE;
	}
	VH_EXHAUSTIVE_SWITCH_END
	return {};
}

AUTORTFM_DISABLE void TestStatusFor()
{
	FHostState& Host = GetHost();
	const int32 Declared = Host.ConsumerAbiVersion;
	const struct
	{
		int32 Version;
		bool bNew;
		const TCHAR* Label;
	} Consumers[] = {
		{VH_ABI_VERSION_MAJOR * 1000 + 4, true, TEXT("12.4")},
		{VH_ABI_VERSION_MAJOR * 1000 + 3, false, TEXT("12.3")},
	};
	for (const auto& Consumer : Consumers)
	{
		Host.ConsumerAbiVersion = Consumer.Version;
		int32 Covered = 0;
		for (int32 Raw = 0; Raw < 256; ++Raw)
		{
			const TCHAR* Name = nullptr;
			const TOptional<int32> Expected = ExpectedStatus((EHostFailure)Raw, Consumer.bNew, Name);
			if (!Expected.IsSet())
			{
				break;
			}
			++Covered;
			ExpectInt(FString::Printf(TEXT("status_for %s at %s"), Name, Consumer.Label),
			          StatusFor((EHostFailure)Raw), Expected.GetValue());
		}
		ExpectInt(FString::Printf(TEXT("status_for covers every failure at %s"), Consumer.Label),
		          Covered, (int32)EHostFailure::AnalysisNotReaped + 1);
	}
	Host.ConsumerAbiVersion = Declared;
}

const uLang::CDataDefinition* FindMember(const uLang::CClass& Class, const char* Name)
{
	for (const uLang::TSRef<uLang::CDataDefinition>& Member : Class.GetDefinitionsOfKind<uLang::CDataDefinition>())
	{
		if (FUtf8StringView(Member->AsNameCString()).Equals(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Name))))
		{
			return &*Member;
		}
	}
	return nullptr;
}

struct FKindRow
{
	const char* Member;
	EDeclaredKind Kind;
	EDeclaredKind OptionOf;
	bool bIsOption;
	/// Checked only for a row whose kind has a class to come from.
	TOptional<EClassOrigin> Origin;
	int32 ContainerTag;
};

AUTORTFM_DISABLE void TestClassifier(const uLang::CClass& Probe, const uLang::CSemanticProgram& Program)
{
	using K = EDeclaredKind;
	using O = EClassOrigin;
	const FKindRow Rows[] = {
		{"AnInt", K::Int, K::Other, false, {}, 0},
		{"ABounded", K::Int, K::Other, false, {}, 0},
		{"ALogic", K::Logic, K::Other, false, {}, 0},
		{"AFloat", K::Float, K::Other, false, {}, 0},
		{"AChar", K::Char, K::Other, false, {}, 0},
		{"AString", K::String, K::Other, false, {}, 0},
		{"AnEnum", K::Enum, K::Other, false, {}, 0},
		{"AnArray", K::Array, K::Other, false, {}, 0},
		{"AStringArray", K::Array, K::Other, false, {}, 0},
		{"AMap", K::Map, K::Other, false, {}, 0},
		{"ATuple", K::Tuple, K::Other, false, {}, 0},
		{"AnOption", K::Option, K::Int, true, {}, 0},
		{"AnEnumOption", K::Option, K::Enum, true, {}, 0},
		{"ANested", K::Option, K::Other, true, {}, 0},
		{"AVariant", K::Variant, K::Other, false, {}, 0},
		{"ARid", K::Rid, K::Other, false, {}, 0},
		{"AVector", K::MathStruct, K::Other, false, O::Mirrored, 0},
		{"AColor", K::MathStruct, K::Other, false, O::Mirrored, 0},
		{"ARect", K::MathStruct, K::Other, false, O::Mirrored, 0},
		{"AReport", K::UserStruct, K::Other, false, O::Script, 0},
		{"ANode", K::Reference, K::Other, false, O::Mirrored, 0},
		{"AMaybeNode", K::Reference, K::Other, true, O::Mirrored, 0},
		{"AHelper", K::Reference, K::Other, true, O::Script, 0},
		{"ABinding", K::Reference, K::Other, true, O::Binding, 0},
		{"AGodotArray", K::Container, K::Other, false, O::Mirrored, VH_VARIANT_ARRAY},
		{"ADictionary", K::Container, K::Other, false, O::Mirrored, VH_VARIANT_DICTIONARY},
		{"ACallable", K::Container, K::Other, false, O::Mirrored, VH_VARIANT_CALLABLE},
		{"ASignalRef", K::Container, K::Other, false, O::Mirrored, VH_VARIANT_SIGNAL},
		{"ANodeArray", K::TypedContainer, K::Other, false, {}, 0},
		{"APlain", K::OtherClass, K::Other, false, O::Script, 0},
		{"AnEvent", K::OtherClass, K::Other, false, {}, 0},
	};

	for (const FKindRow& Row : Rows)
	{
		const FString Name = FString::Printf(TEXT("classify %s"), UTF8_TO_TCHAR(Row.Member));
		const uLang::CDataDefinition* Member = FindMember(Probe, Row.Member);
		if (!Member)
		{
			Case(Name, false, TEXT("unit_probe declares no such member"));
			continue;
		}
		const FDeclaredType Declared = ClassifyDeclaredType(Member->GetType(), Program);
		FString Why;
		if (Declared.Kind != Row.Kind)
		{
			Why = FString::Printf(TEXT("got %s, expected %s"), KindName(Declared.Kind), KindName(Row.Kind));
		}
		else if (Declared.bIsOption != Row.bIsOption)
		{
			Why = FString::Printf(TEXT("bIsOption %d, expected %d"), (int32)Declared.bIsOption, (int32)Row.bIsOption);
		}
		else if (Row.Kind == K::Option && Declared.OptionOf != Row.OptionOf)
		{
			Why = FString::Printf(TEXT("OptionOf %s, expected %s"), KindName(Declared.OptionOf), KindName(Row.OptionOf));
		}
		else if (Row.Origin.IsSet() && Declared.Origin != Row.Origin.GetValue())
		{
			Why = FString::Printf(TEXT("origin %s, expected %s"), OriginName(Declared.Origin), OriginName(Row.Origin.GetValue()));
		}
		else if (Row.ContainerTag != 0 && Declared.ContainerTag != Row.ContainerTag)
		{
			Why = FString::Printf(TEXT("container tag %d, expected %d"), Declared.ContainerTag, Row.ContainerTag);
		}
		else if (Row.Kind == K::MathStruct && Declared.Layout == nullptr)
		{
			Why = TEXT("no generated layout");
		}
		Case(Name, Why.IsEmpty(), Why);
	}

	Case(TEXT("classify no type at all"), ClassifyDeclaredType(nullptr, Program).Kind == K::Other);
}

AUTORTFM_DISABLE FMemberType Describe(const uLang::CClass& Probe, const uLang::CSemanticProgram& Program, const char* Member)
{
	const uLang::CDataDefinition* Definition = FindMember(Probe, Member);
	return Definition ? DescribeType(Definition->GetType(), Program) : FMemberType{};
}

AUTORTFM_DISABLE void TestDescribers(const uLang::CClass& Probe, const uLang::CSemanticProgram& Program)
{
	const auto Export = [&](const char* Member) {
		FExportDesc Desc;
		if (const uLang::CDataDefinition* Definition = FindMember(Probe, Member))
		{
			DescribeExportType(Definition->GetType(), Program, Desc);
		}
		return Desc;
	};
	const auto ExpectExport = [](const TCHAR* Member, const FExportDesc& Desc, vh_type Type, int32 Tag, int32 Reject) {
		ExpectInt(FString::Printf(TEXT("describe_export %s type"), Member), Desc.Type, Type);
		ExpectInt(FString::Printf(TEXT("describe_export %s tag"), Member), Desc.VariantTag, Tag);
		ExpectInt(FString::Printf(TEXT("describe_export %s reject"), Member), Desc.Reject, Reject);
	};

	ExpectExport(TEXT("AnInt"), Export("AnInt"), VH_TYPE_INT, VH_VARIANT_INT, VH_EXPORT_OK);
	ExpectExport(TEXT("AString"), Export("AString"), VH_TYPE_STRING, VH_VARIANT_STRING, VH_EXPORT_OK);
	ExpectExport(TEXT("AVector"), Export("AVector"), VH_TYPE_TUPLE, VH_VARIANT_VECTOR2, VH_EXPORT_OK);
	ExpectExport(TEXT("AnArray"), Export("AnArray"), VH_TYPE_ARRAY, VH_VARIANT_PACKED_INT64_ARRAY, VH_EXPORT_OK);
	ExpectExport(TEXT("AStringArray"), Export("AStringArray"), VH_TYPE_ARRAY, VH_VARIANT_PACKED_STRING_ARRAY, VH_EXPORT_OK);
	ExpectExport(TEXT("AVariant"), Export("AVariant"), VH_TYPE_VARIANT, VH_VARIANT_NIL, VH_EXPORT_UNSUPPORTED_TYPE);
	ExpectExport(TEXT("ARid"), Export("ARid"), VH_TYPE_INT, VH_VARIANT_RID, VH_EXPORT_UNSUPPORTED_TYPE);
	ExpectExport(TEXT("AGodotArray"), Export("AGodotArray"), VH_TYPE_REF, VH_VARIANT_ARRAY, VH_EXPORT_UNSUPPORTED_TYPE);
	ExpectInt(TEXT("describe_export AnOption reject"), Export("AnOption").Reject, VH_EXPORT_OPTION_NOT_OBJECT);
	ExpectInt(TEXT("describe_export ANode reject"), Export("ANode").Reject, VH_EXPORT_OBJECT_NOT_OPTIONAL);
	ExpectInt(TEXT("describe_export ABinding reject"), Export("ABinding").Reject, VH_EXPORT_BINDING_CLASS_UNSUPPORTED);
	Expect(TEXT("describe_export ABinding native class"), Str(Export("ABinding").NativeClass), TEXT("ref_counted"));

	const FExportDesc Maybe = Export("AMaybeNode");
	ExpectExport(TEXT("AMaybeNode"), Maybe, VH_TYPE_OPTION, VH_VARIANT_OBJECT, VH_EXPORT_OK);
	ExpectInt(TEXT("describe_export AMaybeNode hint"), Maybe.Hint, VH_EXPORT_HINT_CLASS);
	Expect(TEXT("describe_export AMaybeNode hint string"), Str(Maybe.HintString), TEXT("node2d"));

	// Not @global_class, so Godot has no name for it and the slot is filtered by its native base.
	const FExportDesc Helper = Export("AHelper");
	ExpectInt(TEXT("describe_export AHelper hint"), Helper.Hint, VH_EXPORT_HINT_CLASS);
	Expect(TEXT("describe_export AHelper hint string"), Str(Helper.HintString), TEXT("node2d"));
	ExpectInt(TEXT("describe_export AHelper reject"), Helper.Reject, VH_EXPORT_OK);

	const FExportDesc Enum = Export("AnEnum");
	ExpectInt(TEXT("describe_export AnEnum hint"), Enum.Hint, VH_EXPORT_HINT_ENUM);
	Expect(TEXT("describe_export AnEnum hint string"), Str(Enum.HintString), TEXT("Idle,Busy,Done"));

	const FExportDesc Bounded = Export("ABounded");
	ExpectInt(TEXT("describe_export ABounded hint"), Bounded.Hint, VH_EXPORT_HINT_RANGE);
	Case(TEXT("describe_export ABounded range"),
	     Bounded.bHasRangeMin && Bounded.bHasRangeMax && Bounded.RangeMin == 0.0 && Bounded.RangeMax == 10.0,
	     FString::Printf(TEXT("got [%g, %g] (%d, %d)"), Bounded.RangeMin, Bounded.RangeMax, (int32)Bounded.bHasRangeMin, (int32)Bounded.bHasRangeMax));
	Case(TEXT("describe_export AnInt has no range"), !Export("AnInt").bHasRangeMin && Export("AnInt").Hint == VH_EXPORT_HINT_NONE);

	const FMemberType EnumOption = Describe(Probe, Program, "AnEnumOption");
	ExpectInt(TEXT("describe_type AnEnumOption enumerators"), EnumOption.EnumeratorCount, 3);
	Expect(TEXT("describe_type AnEnumOption enumeration"), Str(EnumOption.EnumerationName), TEXT("(/user@localhost:)unit_mode"));

	const FMemberType Report = Describe(Probe, Program, "AReport");
	Case(TEXT("describe_type AReport layout"), Report.UserStruct.IsValid());
	if (Report.UserStruct.IsValid())
	{
		Expect(TEXT("describe_type AReport decorated name"), Str(Report.UserStruct->DecoratedName), TEXT("(/user@localhost:)unit_report"));
		Expect(TEXT("describe_type AReport field names"),
		       FString::JoinBy(Report.UserStruct->FieldNames, TEXT(","), [](const FUtf8String& N) { return FString(N); }),
		       TEXT("Count,Label"));
		Case(TEXT("describe_type AReport field kinds"),
		     Report.UserStruct->FieldTypes.Num() == 2 && Report.UserStruct->FieldTypes[0].Kind == EDeclaredKind::Int
		         && Report.UserStruct->FieldTypes[1].Kind == EDeclaredKind::String);
	}

	const FMemberType Vector = Describe(Probe, Program, "AVector");
	Expect(TEXT("describe_type AVector struct"), Str(Vector.StructName), TEXT("vector2"));
	Case(TEXT("describe_type AVector layout"), Vector.Struct != nullptr);

	const FMemberType Helper2 = Describe(Probe, Program, "AHelper");
	Expect(TEXT("describe_type AHelper reference"), Str(Helper2.ReferenceName), TEXT("unit_helper"));
	Expect(TEXT("describe_type AHelper qualified"), Str(Helper2.ReferenceQualifiedName), TEXT("unit_helper"));
	Case(TEXT("describe_type AHelper is optional"), Helper2.bReferenceIsOption);

	FUtf8String ClassName;
	int32 ClassKind = -1;
	DescribeClassOf(Helper2, Program, ClassName, ClassKind);
	Expect(TEXT("describe_class_of AHelper name"), Str(ClassName), TEXT("node2d"));
	ExpectInt(TEXT("describe_class_of AHelper kind"), ClassKind, VH_CLASS_MIRRORED);

	const FMemberType Node = Describe(Probe, Program, "ANode");
	Expect(TEXT("describe_type ANode qualified"), Str(Node.ReferenceQualifiedName), TEXT("node2d"));
	Case(TEXT("describe_type ANode is bare"), !Node.bReferenceIsOption);

	const FMemberType Plain = Describe(Probe, Program, "APlain");
	Expect(TEXT("describe_type APlain reference"), Str(Plain.ReferenceName), TEXT("unit_plain"));
	DescribeClassOf(Plain, Program, ClassName, ClassKind);
	ExpectInt(TEXT("describe_class_of APlain kind"), ClassKind, VH_CLASS_NONE);
}

/// Every scalar a wire value carries, depth first, so a math struct compares the same whether its
/// nested fields arrive flat or as tuples of their own.
void Flatten(const vh_value& Value, TArray<double>& Out)
{
	switch (Value.Type)
	{
	case VH_TYPE_TUPLE:
	case VH_TYPE_ARRAY:
		for (int32 Index = 0; Index < Value.Seq.Count; ++Index)
		{
			Flatten(Value.Seq.Items[Index], Out);
		}
		return;
	case VH_TYPE_INT:
		Out.Add((double)Value.Int);
		return;
	case VH_TYPE_FLOAT:
		Out.Add(Value.Float);
		return;
	case VH_TYPE_LOGIC:
		Out.Add(Value.Logic ? 1.0 : 0.0);
		return;
	default:
		return;
	}
}

FString Numbers(const TArray<double>& Values)
{
	return FString::JoinBy(Values, TEXT(","), [](double V) { return FString::Printf(TEXT("%g"), V); });
}

FString FailureName(EHostFailure Failure)
{
	const TCHAR* Name = TEXT("?");
	ExpectedStatus(Failure, true, Name);
	return Name;
}

vh_value Scalar(vh_type Type, int32 Tag)
{
	vh_value Value{};
	Value.Type = Type;
	Value.VariantTag = Tag;
	return Value;
}

vh_value WireTuple(const TArray<vh_value>& Items)
{
	vh_value Value{};
	Value.Type = VH_TYPE_TUPLE;
	Value.Seq.Items = Items.GetData();
	Value.Seq.Count = Items.Num();
	return Value;
}

vh_value WireFloat(double Number)
{
	vh_value Value = Scalar(VH_TYPE_FLOAT, VH_VARIANT_FLOAT);
	Value.Float = Number;
	return Value;
}

vh_value WireInt(int64 Number, int32 Tag = VH_VARIANT_INT)
{
	vh_value Value = Scalar(VH_TYPE_INT, Tag);
	Value.Int = Number;
	return Value;
}

vh_value WireString(const char* Utf8)
{
	vh_value Value = Scalar(VH_TYPE_STRING, VH_VARIANT_STRING);
	Value.String.Utf8 = Utf8;
	Value.String.Len = (int32)strlen(Utf8);
	return Value;
}

/// One wire value through WireToValue and back through ValueToWire, against the declaration a
/// member of unit_probe carries. Answers the value that came back, or the first failure.
struct FRoundTrip
{
	TOptional<EHostFailure> Failure;
	const TCHAR* FailedIn = TEXT("");
	vh_value Back{};
	FFieldStorage Storage;
	bool bBuiltStruct = false;
};

AUTORTFM_DISABLE void RoundTrip(const vh_value& Wire, const FMemberType& Declared, FRoundTrip& Out)
{
	Verse::FRunningContext Context = Verse::FRunningContextPromise{};
	EnterVerse(Context, [&] {
		Verse::VValue Value;
		const TResult<void> In = WireToValue(Context, Wire, Declared, Value);
		if (!In)
		{
			Out.Failure = In.GetFailure();
			Out.FailedIn = TEXT("WireToValue");
			return;
		}
		Out.bBuiltStruct = Value.DynamicCast<Verse::VValueObject>() != nullptr;
		const TResult<void> Back = ValueToWire(Context, Value, Declared, Out.Storage, Out.Back);
		if (!Back)
		{
			Out.Failure = Back.GetFailure();
			Out.FailedIn = TEXT("ValueToWire");
		}
	});
}

AUTORTFM_DISABLE void TestMarshal(const uLang::CClass& Probe, const uLang::CSemanticProgram& Program)
{
	const auto ExpectNumbers = [&](const TCHAR* Name, const char* Member, const vh_value& Wire, vh_type BackType,
	                               const TArray<double>& Expected) {
		FRoundTrip Trip;
		RoundTrip(Wire, Describe(Probe, Program, Member), Trip);
		if (Trip.Failure.IsSet())
		{
			Case(Name, false, FString::Printf(TEXT("%s answered %s"), Trip.FailedIn, *FailureName(Trip.Failure.GetValue())));
			return;
		}
		TArray<double> Got;
		Flatten(Trip.Back, Got);
		Case(Name, Trip.Back.Type == BackType && Got == Expected,
		     FString::Printf(TEXT("got type %d [%s], expected type %d [%s]"), Trip.Back.Type, *Numbers(Got), (int32)BackType,
		                     *Numbers(Expected)));
	};
	const auto ExpectFailure = [&](const TCHAR* Name, const char* Member, const vh_value& Wire, const TCHAR* Stage,
	                               EHostFailure Expected) {
		FRoundTrip Trip;
		RoundTrip(Wire, Describe(Probe, Program, Member), Trip);
		const FString Got = Trip.Failure.IsSet() ? FString::Printf(TEXT("%s in %s"), *FailureName(Trip.Failure.GetValue()), Trip.FailedIn)
		                                         : FString(TEXT("a value"));
		Expect(Name, Got, FString::Printf(TEXT("%s in %s"), *FailureName(Expected), Stage));
	};

	ExpectNumbers(TEXT("round_trip int"), "AnInt", WireInt(42), VH_TYPE_INT, {42.0});
	ExpectNumbers(TEXT("round_trip int negative"), "AnInt", WireInt(-1234567890123), VH_TYPE_INT, {-1234567890123.0});
	ExpectNumbers(TEXT("round_trip float"), "AFloat", WireFloat(2.5), VH_TYPE_FLOAT, {2.5});
	// Godot spells 0 as an integer whatever the receiving type, so a float slot widens one.
	ExpectNumbers(TEXT("round_trip float from int"), "AFloat", WireInt(3), VH_TYPE_FLOAT, {3.0});
	{
		vh_value Yes = Scalar(VH_TYPE_LOGIC, VH_VARIANT_BOOL);
		Yes.Logic = 1;
		ExpectNumbers(TEXT("round_trip logic true"), "ALogic", Yes, VH_TYPE_LOGIC, {1.0});
		vh_value No = Scalar(VH_TYPE_LOGIC, VH_VARIANT_BOOL);
		ExpectNumbers(TEXT("round_trip logic false"), "ALogic", No, VH_TYPE_LOGIC, {0.0});
	}
	{
		FRoundTrip Trip;
		const char* Text = "h\xc3\xa9llo, world";
		RoundTrip(WireString(Text), Describe(Probe, Program, "AString"), Trip);
		const FString Got = Trip.Failure.IsSet() ? FailureName(Trip.Failure.GetValue())
		                                         : FString(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Trip.Back.String.Utf8), Trip.Back.String.Len));
		Case(TEXT("round_trip string"), !Trip.Failure.IsSet() && Trip.Back.Type == VH_TYPE_STRING && Got == FString(UTF8_TO_TCHAR(Text)),
		     FString::Printf(TEXT("got %s"), *Got));
	}
	ExpectNumbers(TEXT("round_trip enum ordinal"), "AnEnum", WireInt(2), VH_TYPE_INT, {2.0});
	ExpectFailure(TEXT("round_trip enum ordinal out of range"), "AnEnum", WireInt(3), TEXT("WireToValue"), EHostFailure::EnumOrdinalOutOfRange);
	ExpectNumbers(TEXT("round_trip optional enum"), "AnEnumOption", WireInt(1), VH_TYPE_INT, {1.0});
	ExpectNumbers(TEXT("round_trip rid"), "ARid", WireInt(123, VH_VARIANT_RID), VH_TYPE_INT, {123.0});
	ExpectNumbers(TEXT("round_trip variant holding int"), "AVariant", WireInt(7), VH_TYPE_INT, {7.0});
	{
		FRoundTrip Trip;
		RoundTrip(WireString("lane"), Describe(Probe, Program, "AVariant"), Trip);
		Case(TEXT("round_trip variant holding string"),
		     !Trip.Failure.IsSet() && Trip.Back.Type == VH_TYPE_STRING && Trip.Back.VariantTag == VH_VARIANT_STRING
		         && FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Trip.Back.String.Utf8), Trip.Back.String.Len).Equals(UTF8TEXTVIEW("lane")),
		     Trip.Failure.IsSet() ? FailureName(Trip.Failure.GetValue()) : FString::Printf(TEXT("type %d tag %d"), Trip.Back.Type, Trip.Back.VariantTag));
	}
	{
		const TArray<vh_value> Items = {WireFloat(1.5), WireFloat(-2.0)};
		ExpectNumbers(TEXT("round_trip vector2"), "AVector", WireTuple(Items), VH_TYPE_TUPLE, {1.5, -2.0});
	}
	{
		const TArray<vh_value> Items = {WireFloat(0.25), WireFloat(0.5), WireFloat(0.75), WireFloat(1.0)};
		ExpectNumbers(TEXT("round_trip color"), "AColor", WireTuple(Items), VH_TYPE_TUPLE, {0.25, 0.5, 0.75, 1.0});
	}
	{
		const TArray<vh_value> Items = {WireFloat(1.0), WireFloat(2.0), WireFloat(3.0), WireFloat(4.0)};
		ExpectNumbers(TEXT("round_trip rect2"), "ARect", WireTuple(Items), VH_TYPE_TUPLE, {1.0, 2.0, 3.0, 4.0});
	}
	{
		const TArray<vh_value> Items = {WireFloat(1.0)};
		ExpectFailure(TEXT("round_trip vector2 short a component"), "AVector", WireTuple(Items), TEXT("WireToValue"), EHostFailure::TypeMismatch);
	}
	{
		const TArray<vh_value> Items = {WireInt(4), WireInt(5), WireInt(6)};
		vh_value Array{};
		Array.Type = VH_TYPE_ARRAY;
		Array.Seq.Items = Items.GetData();
		Array.Seq.Count = Items.Num();
		ExpectNumbers(TEXT("round_trip int array"), "AnArray", Array, VH_TYPE_ARRAY, {4.0, 5.0, 6.0});
	}
	{
		vh_value Ref = Scalar(VH_TYPE_REF, VH_VARIANT_ARRAY);
		Ref.Ref = 42;
		FRoundTrip Trip;
		RoundTrip(Ref, Describe(Probe, Program, "AGodotArray"), Trip);
		Case(TEXT("round_trip godot_array id"),
		     !Trip.Failure.IsSet() && Trip.Back.Type == VH_TYPE_REF && Trip.Back.Ref == 42 && Trip.Back.VariantTag == VH_VARIANT_ARRAY,
		     Trip.Failure.IsSet() ? FailureName(Trip.Failure.GetValue())
		                          : FString::Printf(TEXT("type %d ref %lld tag %d"), Trip.Back.Type, Trip.Back.Ref, Trip.Back.VariantTag));
	}
	{
		// Godot's null into an optional reference is the empty option, which reads back as one: the
		// declaration, not the cell, is what tells it from a logic false.
		FRoundTrip Trip;
		RoundTrip(WireInt(0, VH_VARIANT_OBJECT), Describe(Probe, Program, "AMaybeNode"), Trip);
		Case(TEXT("round_trip null into ?node2d"),
		     !Trip.Failure.IsSet() && Trip.Back.Type == VH_TYPE_OPTION && Trip.Back.Option == nullptr && Trip.Back.VariantTag == VH_VARIANT_OBJECT,
		     Trip.Failure.IsSet() ? FailureName(Trip.Failure.GetValue()) : FString::Printf(TEXT("type %d tag %d"), Trip.Back.Type, Trip.Back.VariantTag));
	}
	ExpectFailure(TEXT("round_trip null into node2d"), "ANode", WireInt(0, VH_VARIANT_OBJECT), TEXT("WireToValue"), EHostFailure::NullNotOptional);
	{
		// A project's own struct is built from the wire and has no way back out of ValueToWire,
		// which reads a struct only through a generated layout. A signal payload takes one apart
		// field by field instead (DescribePayload), so nothing reaches this; the case pins it.
		const TArray<vh_value> Items = {WireInt(5), WireString("five")};
		FRoundTrip Trip;
		RoundTrip(WireTuple(Items), Describe(Probe, Program, "AReport"), Trip);
		Case(TEXT("wire_to_value unit_report builds a struct"), Trip.bBuiltStruct,
		     Trip.Failure.IsSet() ? FString::Printf(TEXT("%s answered %s"), Trip.FailedIn, *FailureName(Trip.Failure.GetValue())) : FString());
		Expect(TEXT("value_to_wire unit_report declines"),
		       Trip.Failure.IsSet() ? FString::Printf(TEXT("%s in %s"), *FailureName(Trip.Failure.GetValue()), Trip.FailedIn) : FString(TEXT("a value")),
		       TEXT("Unconvertible in ValueToWire"));
	}
	{
		const TArray<vh_value> Items = {WireInt(5)};
		ExpectFailure(TEXT("round_trip unit_report short a field"), "AReport", WireTuple(Items), TEXT("WireToValue"), EHostFailure::TypeMismatch);
	}
}

EDeclaredKind RecordsAs(EDeclaredKind Kind)
{
	// The one kind the sidecar cannot carry: a typed container describes exactly as Other does, and
	// every converter treats the two alike (HostSidecar.cpp's RecordedKind).
	return Kind == EDeclaredKind::TypedContainer ? EDeclaredKind::Other : Kind;
}

EClassOrigin RecordsAs(EClassOrigin Origin)
{
	// The sidecar's refOrigin is shared with the interpreter and has no Binding.
	return Origin == EClassOrigin::Binding ? EClassOrigin::Other : Origin;
}

FString DescribedDifference(const FExportDesc& Before, const FExportDesc& After)
{
	if (Before.Type != After.Type) return FString::Printf(TEXT("type %d, expected %d"), After.Type, Before.Type);
	if (Before.VariantTag != After.VariantTag) return FString::Printf(TEXT("tag %d, expected %d"), After.VariantTag, Before.VariantTag);
	if (Before.ElementVariantTag != After.ElementVariantTag) return TEXT("element tag differs");
	if (Before.Hint != After.Hint) return TEXT("hint differs");
	if (Before.HintString != After.HintString) return TEXT("hint string differs");
	if (Before.NativeClass != After.NativeClass) return TEXT("native class differs");
	if (Before.Reject != After.Reject) return FString::Printf(TEXT("reject %d, expected %d"), After.Reject, Before.Reject);
	if (Before.bHasRangeMin != After.bHasRangeMin || Before.bHasRangeMax != After.bHasRangeMax
	    || Before.RangeMin != After.RangeMin || Before.RangeMax != After.RangeMax)
	{
		return TEXT("range differs");
	}
	return FString();
}

FString MemberTypeDifference(const FMemberType& Before, const FMemberType& After)
{
	if (After.Kind != RecordsAs(Before.Kind))
	{
		return FString::Printf(TEXT("kind %s, expected %s"), KindName(After.Kind), KindName(RecordsAs(Before.Kind)));
	}
	if (After.bIsVar != Before.bIsVar) return FString::Printf(TEXT("var %d, expected %d"), (int32)After.bIsVar, (int32)Before.bIsVar);
	if (After.ReferenceName != Before.ReferenceName) return TEXT("reference name differs");
	if (After.ReferenceQualifiedName != Before.ReferenceQualifiedName) return TEXT("qualified reference name differs");
	if (After.ReferenceOrigin != RecordsAs(Before.ReferenceOrigin))
	{
		return FString::Printf(TEXT("origin %s, expected %s"), OriginName(After.ReferenceOrigin), OriginName(RecordsAs(Before.ReferenceOrigin)));
	}
	if (After.bReferenceIsOption != Before.bReferenceIsOption) return TEXT("reference optionality differs");
	if (After.StructName != Before.StructName || After.Struct != Before.Struct) return TEXT("math struct differs");
	if (After.EnumeratorCount != Before.EnumeratorCount || After.EnumerationName != Before.EnumerationName) return TEXT("enumeration differs");
	const FString Described = DescribedDifference(Before.Described, After.Described);
	if (!Described.IsEmpty()) return Described;
	if (After.UserStruct.IsValid() != Before.UserStruct.IsValid()) return TEXT("user struct presence differs");
	if (Before.UserStruct.IsValid())
	{
		const FUserStructLayout& A = *Before.UserStruct;
		const FUserStructLayout& B = *After.UserStruct;
		if (A.DecoratedName != B.DecoratedName || A.FieldNames != B.FieldNames || A.FieldKeys != B.FieldKeys
		    || A.FieldTypes.Num() != B.FieldTypes.Num())
		{
			return TEXT("user struct layout differs");
		}
		for (int32 Index = 0; Index < A.FieldTypes.Num(); ++Index)
		{
			const FString Field = MemberTypeDifference(A.FieldTypes[Index], B.FieldTypes[Index]);
			if (!Field.IsEmpty()) return FString::Printf(TEXT("field %d: %s"), Index, *Field);
		}
	}
	return FString();
}

FString PayloadDifference(const FPayloadShape& Before, const FPayloadShape& After)
{
	if (Before.Kind != After.Kind) return TEXT("shape differs");
	if (Before.Reject != After.Reject) return TEXT("reject differs");
	if (Before.Args.Num() != After.Args.Num()) return TEXT("argument count differs");
	for (int32 Index = 0; Index < Before.Args.Num(); ++Index)
	{
		if (Before.Args[Index].Name != After.Args[Index].Name || Before.Args[Index].FieldKey != After.Args[Index].FieldKey)
		{
			return FString::Printf(TEXT("argument %d is named differently"), Index);
		}
		const FString Arg = MemberTypeDifference(Before.Args[Index].Type, After.Args[Index].Type);
		if (!Arg.IsEmpty()) return FString::Printf(TEXT("argument %d: %s"), Index, *Arg);
	}
	return MemberTypeDifference(Before.Whole, After.Whole);
}

/// One class's three tables against their round trip, a case per row.
void CompareDeclaredTypes(const TCHAR* Prefix, const FDeclaredTypes& Before, const FDeclaredTypes& After)
{
	for (const TPair<FUtf8String, FMemberType>& Pair : Before.Members)
	{
		const FString Name = FString::Printf(TEXT("%s member %s"), Prefix, *Str(Pair.Key));
		const FMemberType* Back = After.Members.Find(Pair.Key);
		Case(Name, Back != nullptr && MemberTypeDifference(Pair.Value, *Back).IsEmpty(),
		     Back ? MemberTypeDifference(Pair.Value, *Back) : FString(TEXT("missing")));
	}
	ExpectInt(FString::Printf(TEXT("%s member count"), Prefix), After.Members.Num(), Before.Members.Num());

	for (const TPair<FUtf8String, FMethodSignatureTypes>& Pair : Before.Methods)
	{
		const FString Name = FString::Printf(TEXT("%s method %s"), Prefix, *Str(Pair.Key));
		const FMethodSignatureTypes* Back = After.Methods.Find(Pair.Key);
		FString Why = Back ? FString() : FString(TEXT("missing"));
		if (Back && Back->Params.Num() != Pair.Value.Params.Num())
		{
			Why = TEXT("parameter count differs");
		}
		for (int32 Index = 0; Back && Why.IsEmpty() && Index < Pair.Value.Params.Num(); ++Index)
		{
			const FString Param = MemberTypeDifference(Pair.Value.Params[Index], Back->Params[Index]);
			if (!Param.IsEmpty())
			{
				Why = FString::Printf(TEXT("parameter %d: %s"), Index, *Param);
			}
		}
		if (Back && Why.IsEmpty())
		{
			Why = MemberTypeDifference(Pair.Value.Result, Back->Result);
		}
		Case(Name, Why.IsEmpty(), Why);
	}
	ExpectInt(FString::Printf(TEXT("%s method count"), Prefix), After.Methods.Num(), Before.Methods.Num());

	for (const TPair<FUtf8String, FPayloadShape>& Pair : Before.Signals)
	{
		const FString Name = FString::Printf(TEXT("%s signal %s"), Prefix, *Str(Pair.Key));
		const FPayloadShape* Back = After.Signals.Find(Pair.Key);
		const FString Why = Back ? PayloadDifference(Pair.Value, *Back) : FString(TEXT("missing"));
		Case(Name, Why.IsEmpty(), Why);
	}
	ExpectInt(FString::Printf(TEXT("%s signal count"), Prefix), After.Signals.Num(), Before.Signals.Num());
}

AUTORTFM_DISABLE void TestSidecar(int32 Generation)
{
	const TSharedPtr<const FAnalysisSnapshot> Before = GetAnalysisSnapshot();
	Case(TEXT("sidecar has a snapshot to write"), Before.IsValid());
	if (!Before.IsValid())
	{
		return;
	}
	const FAnalysisSnapshot::FClass* Probe = Before->Classes.Find(FUtf8String(UTF8TEXT("unit_probe")));
	Case(TEXT("snapshot describes unit_probe"), Probe != nullptr && Probe->Types.IsValid());
	if (!Probe || !Probe->Types.IsValid())
	{
		return;
	}

	const FMemberType* FloatMember = Probe->Types->Members.Find(FUtf8String(UTF8TEXT("AFloat")));
	const FMemberType* IntMember = Probe->Types->Members.Find(FUtf8String(UTF8TEXT("AnInt")));
	Case(TEXT("snapshot records AFloat as var"), FloatMember && FloatMember->bIsVar);
	Case(TEXT("snapshot records AnInt as not var"), IntMember && !IntMember->bIsVar);
	Case(TEXT("snapshot records the Pinged signal"), Probe->Types->Signals.Contains(FUtf8String(UTF8TEXT("Pinged"))));
	Case(TEXT("snapshot records the Take method"), Probe->Types->Methods.Num() >= 1);
	Case(TEXT("snapshot records the ToString extension"), !Probe->ToStringDecorated.IsEmpty());

	const TSharedPtr<FJsonObject> Written = WriteDeclaredTypes(*Probe->Types);
	const TSharedPtr<FDeclaredTypes> Read = ReadDeclaredTypes(Written);
	Case(TEXT("declared types read back"), Read.IsValid());
	if (Read.IsValid())
	{
		CompareDeclaredTypes(TEXT("declared types"), *Probe->Types, *Read);
		const FMemberType* ReadFloat = Read->Members.Find(FUtf8String(UTF8TEXT("AFloat")));
		Case(TEXT("declared types keep var-ness"), ReadFloat && ReadFloat->bIsVar);
	}
	Case(TEXT("declared types null in, null out"), !ReadDeclaredTypes(nullptr).IsValid());

	// The whole file, which is what a cook writes and a runtime host reads: through the manifest
	// reader's stamp check, then published as *the* snapshot. Last, because it replaces the one the
	// analysis left.
	const FString Path = FPaths::Combine(FPlatformProcess::UserTempDir(),
	                                     FString::Printf(TEXT("verse_host_unit_%u.json"), FPlatformProcess::GetCurrentProcessId()));
	FUtf8String Error;
	const TArray<FString> Packages = {TEXT("/unit/one"), TEXT("/unit/two")};
	const bool bWritten = WriteClassSidecar(Path, Packages, Generation, Error);
	Case(TEXT("sidecar writes"), bWritten, Str(Error));
	if (!bWritten)
	{
		return;
	}

	TArray<FString> ReadPackages;
	int32 ReadGeneration = -1;
	const bool bManifest = ReadCookedManifest(Path, ReadPackages, ReadGeneration, Error);
	Case(TEXT("sidecar manifest passes the stamp check"), bManifest, Str(Error));
	Case(TEXT("sidecar manifest keeps the package list"), ReadPackages == Packages,
	     FString::Printf(TEXT("got %s"), *FString::Join(ReadPackages, TEXT(","))));
	ExpectInt(TEXT("sidecar manifest keeps the generation"), ReadGeneration, Generation);

	const bool bLoaded = LoadClassSidecar(Path, FUtf8StringView(), Error);
	Case(TEXT("sidecar loads"), bLoaded, Str(Error));
	IFileManager::Get().Delete(*Path);
	if (!bLoaded)
	{
		return;
	}

	const TSharedPtr<const FAnalysisSnapshot>& After = GetAnalysisSnapshot();
	Case(TEXT("sidecar replaces the snapshot"), After.IsValid() && After.Get() != Before.Get());
	if (!After.IsValid())
	{
		return;
	}
	TArray<FUtf8String> BeforeNames;
	TArray<FUtf8String> AfterNames;
	Before->Classes.GetKeys(BeforeNames);
	After->Classes.GetKeys(AfterNames);
	BeforeNames.Sort();
	AfterNames.Sort();
	Case(TEXT("sidecar keeps every class"), BeforeNames == AfterNames,
	     FString::JoinBy(AfterNames, TEXT(","), [](const FUtf8String& N) { return FString(N); }));

	const FAnalysisSnapshot::FClass* Loaded = After->Classes.Find(FUtf8String(UTF8TEXT("unit_probe")));
	if (Loaded && Loaded->Types.IsValid())
	{
		CompareDeclaredTypes(TEXT("sidecar"), *Probe->Types, *Loaded->Types);
		Expect(TEXT("sidecar keeps the ToString extension"), Str(Loaded->ToStringDecorated), Str(Probe->ToStringDecorated));
		Case(TEXT("sidecar keeps the published flag"), Loaded->bInPublishedProgram == Probe->bInPublishedProgram);
		ExpectInt(TEXT("sidecar keeps the method list"), Loaded->Methods.Num(), Probe->Methods.Num());
		ExpectInt(TEXT("sidecar keeps the signal list"), Loaded->Signals.Num(), Probe->Signals.Num());
	}
	else
	{
		Case(TEXT("sidecar describes unit_probe"), false);
	}
}

const uLang::CClass* MirroredClass(const uLang::CSemanticProgram& Program, const char* Name)
{
	const FUtf8String Path = FUtf8String(GodotVersePath) + UTF8TEXT("/") + FUtf8String(Name);
	return Program.FindDefinitionByVersePath<uLang::CClass>(FULangConversionUtils::FUtf8StringViewToULangStringView(Path));
}

AUTORTFM_DISABLE void TestAdapters(const uLang::CClass& Probe, const uLang::CSemanticProgram& Program)
{
	const uLang::CClass* const Inner = FindScriptClass(Program, UTF8TEXTVIEW("unit_mod/unit_inner"));
	Case(TEXT("find_script_class by qualified name"), Inner != nullptr);
	Case(TEXT("find_script_class refuses the bare name of a module's class"), FindScriptClass(Program, UTF8TEXTVIEW("unit_inner")) == nullptr);
	Case(TEXT("find_script_class unit_probe"), FindScriptClass(Program, UTF8TEXTVIEW("unit_probe")) == &Probe);
	Case(TEXT("find_script_class_live agrees"), FindScriptClassLive(UTF8TEXTVIEW("unit_probe")) == &Probe);

	Expect(TEXT("qualified_name unit_probe"), Str(QualifiedNameOf(Probe)), TEXT("unit_probe"));
	if (Inner)
	{
		Expect(TEXT("qualified_name unit_inner"), Str(QualifiedNameOf(*Inner)), TEXT("unit_mod/unit_inner"));
		Expect(TEXT("decorated_name unit_inner"), Str(DecoratedNameOf(*Inner)), TEXT("(/user@localhost/unit_mod:)unit_inner"));
		Expect(TEXT("class_origin unit_inner"), OriginName(ClassOriginOf(*Inner, Program)), TEXT("Script"));
	}
	Expect(TEXT("decorated_name unit_probe"), Str(DecoratedNameOf(Probe)), TEXT("(/user@localhost:)unit_probe"));

	const uLang::CClass* const Node2D = MirroredClass(Program, "node2d");
	Case(TEXT("mirror declares node2d"), Node2D != nullptr);
	if (Node2D)
	{
		Expect(TEXT("qualified_name node2d"), Str(QualifiedNameOf(*Node2D)), TEXT("node2d"));
		Expect(TEXT("class_origin node2d"), OriginName(ClassOriginOf(*Node2D, Program)), TEXT("Mirrored"));
		Expect(TEXT("native_class_of node2d"), Str(NativeClassOf(*Node2D, Program)), TEXT("node2d"));
	}
	Expect(TEXT("class_origin unit_probe"), OriginName(ClassOriginOf(Probe, Program)), TEXT("Script"));
	Expect(TEXT("native_class_of unit_probe"), Str(NativeClassOf(Probe, Program)), TEXT("node2d"));
	{
		const TArray<const uLang::CClass*> Chain = ClassChainOfOrigin(Probe, Program, EClassOrigin::Script);
		Case(TEXT("class_chain_of_origin stops at the mirror"), Chain.Num() == 1 && Chain[0] == &Probe);
	}

	const FDeclaredType Binding = ClassifyDeclaredType(FindMember(Probe, "ABinding") ? FindMember(Probe, "ABinding")->GetType() : nullptr, Program);
	if (Binding.Class)
	{
		Expect(TEXT("qualified_name unit_binding"), Str(QualifiedNameOf(*Binding.Class)), TEXT("unit_binding"));
		Expect(TEXT("native_class_of unit_binding"), Str(NativeClassOf(*Binding.Class, Program)), TEXT("ref_counted"));
	}
	else
	{
		Case(TEXT("qualified_name unit_binding"), false, TEXT("ABinding has no class"));
	}

	Case(TEXT("prototype_of an ordinary definition is itself"), &PrototypeOf(Probe) == &Probe);

	// An instantiated parametric class mints a definition per member that was written nowhere; the
	// generic declaration's is the one with a file and a line.
	const FDeclaredType NodeArray = ClassifyDeclaredType(FindMember(Probe, "ANodeArray") ? FindMember(Probe, "ANodeArray")->GetType() : nullptr, Program);
	const uLang::CFunction* ToArray = nullptr;
	if (NodeArray.Class)
	{
		for (const uLang::TSRef<uLang::CFunction>& Function : NodeArray.Class->GetDefinitionsOfKind<uLang::CFunction>())
		{
			if (FUtf8StringView(Function->AsNameCString()).Equals(UTF8TEXTVIEW("ToArray")))
			{
				ToArray = &*Function;
			}
		}
	}
	Case(TEXT("typed_array(node) has its own ToArray"), ToArray != nullptr);
	if (ToArray)
	{
		const uLang::CDefinition& Prototype = PrototypeOf(*ToArray);
		Case(TEXT("prototype_of an instantiated member is the generic one"), &Prototype != ToArray);
		Case(TEXT("prototype_of is idempotent"), &PrototypeOf(Prototype) == &Prototype);
		FUtf8String Path;
		int32 Line = -1;
		int32 Column = -1;
		FillLocation(*ToArray, Path, Line, Column);
		Case(TEXT("fill_location of an instantiated member names the generic's file"),
		     FString(Path).EndsWith(TEXT("GodotApi.native.verse")) && Line > 0,
		     FString::Printf(TEXT("got %s:%d"), *Str(Path), Line));
	}

	// The extension method R-NODE-10 finds a class's ToString through: module scope, receiver first.
	const uLang::CFunction* ToString = nullptr;
	for (const uLang::TSRef<uLang::CFunction>& Function : Probe._EnclosingScope.GetLogicalScope().GetDefinitionsOfKind<uLang::CFunction>())
	{
		if (FUtf8StringView(Function->AsNameCString()).Equals(UTF8TEXTVIEW("operator'.ToString'")))
		{
			ToString = &*Function;
		}
	}
	Case(TEXT("unit_probe's ToString extension is at module scope"), ToString != nullptr);
	if (ToString)
	{
		Expect(TEXT("extension_method_name"), Str(ExtensionMethodName(*ToString)), TEXT("ToString"));
		const FString Decorated = Str(ExtensionMethodDecoratedName(*ToString));
		Case(TEXT("extension_method_decorated_name"),
		     Decorated.StartsWith(TEXT("(/user@localhost:)(/user@localhost:)operator'.ToString'(")) && Decorated.Contains(TEXT("unit_probe")),
		     FString::Printf(TEXT("got %s"), *Decorated));
	}
}

void OnDiagnostic(void*, const vh_diagnostic* Diagnostic)
{
	if (!Diagnostic)
	{
		return;
	}
	const FString Message(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Diagnostic->MessageUtf8), Diagnostic->MessageLen));
	const FString File = Diagnostic->FilePathUtf8
		? FString(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Diagnostic->FilePathUtf8), Diagnostic->FilePathLen))
		: FString();
	Say(FString::Printf(TEXT("[host_unit] diagnostic %s:%d:%d (severity %d) %s"), *FPaths::GetCleanFilename(File),
	                    Diagnostic->Line, Diagnostic->Column, Diagnostic->Severity, *Message));
}

void OnRuntimeError(void*, const vh_runtime_error* Error)
{
	if (Error)
	{
		Say(FString::Printf(TEXT("[host_unit] runtime error %s"),
		                    *FString(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Error->MessageUtf8), Error->MessageLen))));
	}
}

void OnPrint(void*, const char* Utf8, int32_t Len)
{
	Say(FString::Printf(TEXT("[host_unit] print %s"), *FString(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Utf8), Len))));
}

const char* const BindingsSource =
	"using { /Godot.org/Godot }\n"
	"\n"
	"unit_binding<public> := class(ref_counted):\n"
	"    Size<public>()<transacts>:int = 3\n";

/// Flushes and leaves with Code, the way the cooker does: the process's work is done, and nothing
/// this program needs happens in an engine teardown (CookMain.cpp's Leave).
[[noreturn]] void Leave(int32 Code)
{
	fflush(stdout);
	if (GLog)
	{
		GLog->Flush();
	}
	FPlatformMisc::RequestExitWithStatus(/*Force*/ true, (uint8)Code);
	for (;;)
	{
	}
}

[[noreturn]] void Finish(double Started)
{
	Say(FString::Printf(TEXT("[host_unit] %d passed, %d failed, 0 skipped"), GPassed, GFailed));
	Say(FString::Printf(TEXT("[host_unit] ran in %.2f s"), FPlatformTime::Seconds() - Started));
	Leave(GFailed == 0 ? 0 : 1);
}

[[noreturn]] AUTORTFM_DISABLE void Run(const FString& RepoRoot)
{
	const double Started = FPlatformTime::Seconds();
	const FString Fixtures = FPaths::Combine(RepoRoot, TEXT("tests"), TEXT("host_unit"));
	const FString ProbePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(Fixtures, TEXT("unit_probe.verse")));
	const FString InnerPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(Fixtures, TEXT("unit_mod"), TEXT("unit_inner.verse")));

	vh_init_desc Desc{};
	Desc.StructSize = sizeof(vh_init_desc);
	Desc.AbiVersion = VH_ABI_VERSION;
	Desc.LayoutDigest = VH_LAYOUT_DIGEST;
	Desc.Godot.StructSize = sizeof(vh_godot_api);
	Desc.Godot.Print = &OnPrint;
	Desc.OnDiagnostic = &OnDiagnostic;
	Desc.OnRuntimeError = &OnRuntimeError;
	const int32_t InitStatus = vh_init(&Desc);
	const double Booted = FPlatformTime::Seconds();
	Say(FString::Printf(TEXT("[host_unit] booted in %.2f s"), Booted - Started));
	ExpectInt(TEXT("vh_init"), InitStatus, VH_OK);
	if (InitStatus != VH_OK)
	{
		Finish(Started);
	}

	TestStatusFor();

	const vh_binding_class Roster[] = {{"UnitBinding", 11, nullptr, 0, "unit_binding", 12}};
	ExpectInt(TEXT("vh_set_bindings"), vh_set_bindings(BindingsSource, (int32_t)strlen(BindingsSource), Roster, 1), VH_OK);

	const FTCHARToUTF8 ProbeUtf8(*ProbePath);
	const FTCHARToUTF8 InnerUtf8(*InnerPath);
	const vh_source_file Files[] = {
		{ProbeUtf8.Get(), nullptr},
		{InnerUtf8.Get(), "unit_mod"},
	};
	int32_t Generation = 0;
	const int32_t CompileStatus = vh_compile_project(Files, 2, &Generation);
	ExpectInt(TEXT("vh_compile_project"), CompileStatus, VH_OK);
	if (CompileStatus != VH_OK)
	{
		Finish(Started);
	}

	// An analysis after the build, so the program the tests read is an analysis's rather than one
	// IR generation has rewritten -- the program every describer reads in an editor.
	FString ProbeText;
	FFileHelper::LoadFileToString(ProbeText, *ProbePath);
	const FTCHARToUTF8 ProbeTextUtf8(*ProbeText);
	ExpectInt(TEXT("vh_check_project"), vh_check_project(ProbeUtf8.Get(), ProbeTextUtf8.Get()), VH_OK);
	const double Compiled = FPlatformTime::Seconds();
	Say(FString::Printf(TEXT("[host_unit] built and analysed in %.2f s"), Compiled - Booted));

	const uLang::CSemanticProgram* const Program = CurrentSemanticProgram();
	const uLang::CClass* const Probe = FindScriptClassLive(UTF8TEXTVIEW("unit_probe"));
	Case(TEXT("the analysis holds unit_probe"), Program != nullptr && Probe != nullptr);
	if (Program && Probe)
	{
		TestClassifier(*Probe, *Program);
		TestDescribers(*Probe, *Program);
		TestMarshal(*Probe, *Program);
		TestAdapters(*Probe, *Program);
		TestSidecar(Generation);
	}

	Finish(Started);
}

} // namespace GodotVerseUnit

// AUTORTFM_DISABLE for the reason CookMain.cpp's entry point carries it: the target is built by
// the AutoRTFM clang, and the chain from here into the engine has to be uninstrumented.
AUTORTFM_DISABLE INT32_MAIN_INT32_ARGC_TCHAR_ARGV()
{
	FTaskTagScope Scope(ETaskTag::EGameThread);
	if (ArgC < 2)
	{
		GodotVerseUnit::Say(TEXT("usage: verse_host_unit <repo root>"));
		return 2;
	}
	GodotVerseUnit::Run(FString(ArgV[1]));
}

#endif // defined(VH_HOST_UNIT)
