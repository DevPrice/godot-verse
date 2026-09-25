// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostVbcWriter.h"

#if VH_HOST_KIND == VH_HOST_KIND_COOKER

#include "Algo/BinarySearch.h"
#include "Algo/StableSort.h"
#include "HAL/FileManager.h"
#include "HostScript.h"
#include "HostVbcOps.gen.h"
#include "host_build_id.gen.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/TopLevelAssetPath.h"
#include "VerseVM/Inline/VVMEnterVMInline.h"
#include "VerseVM/Inline/VVMEnumerationInline.h"
#include "VerseVM/Inline/VVMValueInline.h"
#include "VerseVM/VVMAccessor.h"
#include "VerseVM/VVMArray.h"
#include "VerseVM/VVMArrayBase.h"
#include "VerseVM/VVMBytecodesAndCaptures.h"
#include "VerseVM/VVMClass.h"
#include "VerseVM/VVMEmergentType.h"
#include "VerseVM/VVMEnumeration.h"
#include "VerseVM/VVMEnumerator.h"
#include "VerseVM/VVMFalse.h"
#include "VerseVM/VVMFloatType.h"
#include "VerseVM/VVMFunction.h"
#include "VerseVM/VVMGlobalProgram.h"
#include "VerseVM/VVMHeapInt.h"
#include "VerseVM/VVMIntType.h"
#include "VerseVM/VVMMap.h"
#include "VerseVM/VVMMutableArray.h"
#include "VerseVM/VVMNamedType.h"
#include "VerseVM/VVMNativeProcedure.h"
#include "VerseVM/VVMNativeStruct.h"
#include "VerseVM/VVMOption.h"
#include "VerseVM/VVMPackage.h"
#include "VerseVM/VVMProcedure.h"
#include "VerseVM/VVMProgram.h"
#include "VerseVM/VVMRational.h"
#include "VerseVM/VVMRestValue.h"
#include "VerseVM/VVMScope.h"
#include "VerseVM/VVMShape.h"
#include "VerseVM/VVMTask.h"
#include "VerseVM/VVMTupleType.h"
#include "VerseVM/VVMType.h"
#include "VerseVM/VVMUnion.h"
#include "VerseVM/VVMUnionVariant.h"
#include "VerseVM/VVMUnionVariantTag.h"
#include "VerseVM/VVMUniqueString.h"
#include "VerseVM/VVMValueObject.h"
#include "VerseVM/VVMVerseClass.h"
#include "VerseVM/VVMVerseModuleClass.h"
#include "VerseVM/VVMVerseStruct.h"

namespace {

using GodotVerse::Vbc::ERole;

// A handful of the fields the writer needs are private or protected, with no accessor. A pointer to
// member named in an explicit instantiation is exempt from access checking ([temp.explicit]), which
// is the one way to reach them without editing the engine.
template <typename TTag, typename TTag::TMember Member>
struct TMemberOf
{
	friend typename TTag::TMember MemberPointer(TTag) { return Member; }
};

#define VBC_REACH(Tag, Class, Type, Field) \
	struct Tag                             \
	{                                      \
		using TMember = Type Class::*;     \
		friend TMember MemberPointer(Tag); \
	};                                     \
	template struct TMemberOf<Tag, &Class::Field>

VBC_REACH(FClassFlagsOf, Verse::VClass, Verse::VClass::EFlags, Flags);
VBC_REACH(FClassConstructorOf, Verse::VClass, Verse::TWriteBarrier<Verse::VFunction>, Constructor);
VBC_REACH(FClassBlocksOf, Verse::VClass, Verse::TWriteBarrier<Verse::VFunction>, Blocks);
VBC_REACH(FNamedPackageOf, Verse::VNamedType, Verse::TWriteBarrier<Verse::VPackage>, Package);
VBC_REACH(FNamedRelativePathOf, Verse::VNamedType, Verse::TWriteBarrier<Verse::VArray>, RelativePath);
VBC_REACH(FNamedBaseNameOf, Verse::VNamedType, Verse::TWriteBarrier<Verse::VArray>, BaseName);
VBC_REACH(FNamedAttributeIndicesOf, Verse::VNamedType, Verse::TWriteBarrier<Verse::VArray>, AttributeIndices);
VBC_REACH(FNamedAttributesOf, Verse::VNamedType, Verse::TWriteBarrier<Verse::VArray>, Attributes);
VBC_REACH(FAccessorCountOf, Verse::VAccessor, uint32, NumAccessors);

#undef VBC_REACH

struct FObjectFieldDataOf
{
	using TMember = Verse::VRestValue* (Verse::VObject::*)(const Verse::VCppClassInfo&) const;
	friend TMember MemberPointer(FObjectFieldDataOf);
};
template struct TMemberOf<FObjectFieldDataOf, &Verse::VObject::GetFieldData>;

template <typename TTag, typename TObject>
AUTORTFM_DISABLE decltype(auto) Reach(TObject& Object)
{
	return Object.*MemberPointer(TTag{});
}

namespace Kind
{
constexpr uint8 False = 1;
constexpr uint8 True = 2;
constexpr uint8 BuiltinPackage = 3;
constexpr uint8 Name = 4;
constexpr uint8 Array = 5;
constexpr uint8 MutableArray = 6;
constexpr uint8 Map = 7;
constexpr uint8 MutableMap = 8;
constexpr uint8 Option = 9;
constexpr uint8 HeapInt = 10;
constexpr uint8 Rational = 11;
constexpr uint8 Procedure = 12;
constexpr uint8 NativeProcedure = 13;
constexpr uint8 Function = 14;
constexpr uint8 Scope = 15;
constexpr uint8 Class = 16;
constexpr uint8 Archetype = 17;
constexpr uint8 AccessSpecifier = 18;
constexpr uint8 Enumeration = 19;
constexpr uint8 Enumerator = 20;
constexpr uint8 Package = 24;
constexpr uint8 Module = 25;
constexpr uint8 ValueObject = 26;
constexpr uint8 IntType = 27;
constexpr uint8 FloatType = 28;
constexpr uint8 TupleType = 29;
constexpr uint8 MapType = 30;
constexpr uint8 SimpleType = 31;
constexpr uint8 Accessor = 32;
constexpr uint8 ArrayType = 33;
constexpr uint8 OptionType = 34;
constexpr uint8 PointerType = 35;
} // namespace Kind

constexpr uint8 EndMarker = 0xE5;
constexpr uint32 FormatVersion = 1;
constexpr uint32 NewClassOpcode = static_cast<uint32>(Verse::EOpcode::NewClass);

struct AUTORTFM_DISABLE FSink
{
	TArray<uint8>& Bytes;

	void U8(uint8 Value) { Bytes.Add(Value); }

	void Uv(uint64 Value)
	{
		do
		{
			uint8 Byte = static_cast<uint8>(Value & 0x7f);
			Value >>= 7;
			if (Value != 0)
			{
				Byte |= 0x80;
			}
			Bytes.Add(Byte);
		} while (Value != 0);
	}

	void Sv(int64 Value) { Uv((static_cast<uint64>(Value) << 1) ^ static_cast<uint64>(Value >> 63)); }

	void F64(double Value)
	{
		uint64 Bits;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		for (int32 Shift = 0; Shift < 64; Shift += 8)
		{
			Bytes.Add(static_cast<uint8>(Bits >> Shift));
		}
	}

	void Str(FUtf8StringView Text)
	{
		Uv(static_cast<uint64>(Text.Len()));
		Bytes.Append(reinterpret_cast<const uint8*>(Text.GetData()), Text.Len());
	}
};

AUTORTFM_DISABLE bool IsBuiltInPackage(Verse::VPackage& Package)
{
	return Package.GetName().AsStringView().Equals(UTF8TEXT("$BuiltIn"));
}

/// A module is a UVerseModuleClass, or -- for the two epic_internal modules VNI declares, Persona
/// and Predicts -- a UVerseClass flagged as one.
AUTORTFM_DISABLE bool IsModule(UObject& Object)
{
	if (Object.IsA<UVerseModuleClass>())
	{
		return true;
	}
	const UVerseClass* VerseClass = Cast<UVerseClass>(&Object);
	return VerseClass && VerseClass->IsVerseModule();
}

/// The native procedure a definition holds, directly or as the callee of a function.
AUTORTFM_DISABLE Verse::VNativeProcedure* NativeProcedureOf(Verse::VValue Value)
{
	Value = Value.Follow();
	if (!Value.IsCell())
	{
		return nullptr;
	}
	if (Verse::VNativeProcedure* Native = Value.AsCell().DynamicCast<Verse::VNativeProcedure>())
	{
		return Native;
	}
	if (Verse::VFunction* Function = Value.AsCell().DynamicCast<Verse::VFunction>())
	{
		const Verse::VValue Callee = Function->Procedure.Get().Follow();
		return Callee.IsCell() ? Callee.AsCell().DynamicCast<Verse::VNativeProcedure>() : nullptr;
	}
	return nullptr;
}

/// The characters of a string-shaped VM array. False when it holds anything but UTF-8 code units.
AUTORTFM_DISABLE bool ArrayText(Verse::VArrayBase* Array, FUtf8String& Out)
{
	Out.Reset();
	if (!Array)
	{
		return true;
	}
	switch (Array->GetArrayType())
	{
		case Verse::EArrayType::None:
			return Array->Num() == 0;
		case Verse::EArrayType::Char8:
			Out = FUtf8String(FUtf8StringView(Array->GetData<UTF8CHAR>(), Array->Num()));
			return true;
		default:
			return false;
	}
}

/// One procedure's ops: the byte offset each starts at, ascending.
struct AUTORTFM_DISABLE FOpMap
{
	TArray<Verse::FOp*> Ops;
	TArray<uint32> Starts;
	uint32 NumOpBytes = 0;

	/// The index of the op that starts exactly at Offset, the op count for the end of the stream, or
	/// INDEX_NONE for a position inside an op.
	int32 At(uint32 Offset) const
	{
		if (Offset == NumOpBytes)
		{
			return Starts.Num();
		}
		const int32 Index = Algo::LowerBound(Starts, Offset);
		return Starts.IsValidIndex(Index) && Starts[Index] == Offset ? Index : INDEX_NONE;
	}

	int32 Containing(uint32 Offset) const
	{
		return FMath::Max(Algo::UpperBound(Starts, Offset) - 1, 0);
	}
};

struct FParkRow
{
	int32 Cell = INDEX_NONE;
	FUtf8String Name;
	FUtf8String File;
	int32 Ops = 0;
	int32 MayPark = 0;
	int32 MayParkOnRegister = 0;
};

class AUTORTFM_DISABLE FVbcWriter
{
public:
	explicit FVbcWriter(Verse::FAllocationContext InContext)
		: Context(InContext)
	{
	}

	/// The cell a key names, allocated and queued on first sight; the index is its position in the
	/// cell table.
	int32 SlotFor(Verse::VCell* Cell, UObject* Object)
	{
		const void* Key = Cell ? static_cast<const void*>(Cell) : static_cast<const void*>(Object);
		if (const int32* Found = SlotOf.Find(Key))
		{
			return *Found;
		}
		const int32 Index = Slots.Num();
		const int32 Inherited = CurrentDefinition != INDEX_NONE
			? CurrentDefinition
			: (Current != INDEX_NONE ? Slots[Current].Definition : INDEX_NONE);
		FSlot& Slot = Slots.AddDefaulted_GetRef();
		Slot.Cell = Cell;
		Slot.Object = Object;
		Slot.Parent = Current;
		Slot.Definition = Inherited;
		SlotOf.Add(Key, Index);
		return Index;
	}

	uint32 Sid(FUtf8StringView Text)
	{
		const FUtf8String Key(Text);
		if (const uint32* Found = StringIds.Find(Key))
		{
			return *Found;
		}
		const uint32 Id = static_cast<uint32>(Strings.Num());
		Strings.Add(Key);
		StringIds.Add(Key, Id);
		return Id;
	}

	void WriteSid(FSink& Sink, FUtf8StringView Text) { Sink.Uv(Sid(Text)); }

	void WriteRef(FSink& Sink, Verse::VCell* Cell)
	{
		Sink.Uv(Cell ? static_cast<uint64>(SlotFor(Cell, nullptr)) + 1 : 0);
	}

	void WriteObjectRef(FSink& Sink, UObject* Object)
	{
		Sink.Uv(Object ? static_cast<uint64>(SlotFor(nullptr, Object)) + 1 : 0);
	}

	/// A value (format.md §3), with bound placeholders followed.
	void WriteValue(FSink& Sink, Verse::VValue Value, const TCHAR* What)
	{
		Value = Value.Follow();
		if (Value.IsUninitialized())
		{
			Sink.U8(0);
		}
		else if (Value.IsPlaceholder())
		{
			Refuse(FString::Printf(TEXT("%s is an unbound placeholder"), What));
			Sink.U8(0);
		}
		else if (Value.IsInt32())
		{
			Sink.U8(1);
			Sink.Sv(Value.AsInt32());
		}
		else if (Value.IsFloat())
		{
			Sink.U8(2);
			Sink.F64(Value.AsFloat().AsDouble());
		}
		else if (Value.IsChar())
		{
			Sink.U8(3);
			Sink.U8(static_cast<uint8>(Value.AsChar()));
		}
		else if (Value.IsChar32())
		{
			Sink.U8(4);
			Sink.Uv(static_cast<uint32>(Value.AsChar32()));
		}
		else if (Value.IsCell())
		{
			Sink.U8(5);
			WriteRef(Sink, &Value.AsCell());
		}
		else if (Value.IsUObject())
		{
			Sink.U8(5);
			WriteObjectRef(Sink, Value.AsUObject());
		}
		else
		{
			Refuse(FString::Printf(TEXT("%s has a value encoding the format has no tag for (bits %llx)"),
			                       What, static_cast<unsigned long long>(Value.Encode())));
			Sink.U8(0);
		}
	}

	/// A slot that may still hold the fresh-variable root no placeholder has been made for yet.
	void WriteRestValue(FSink& Sink, Verse::VRestValue& Slot, const TCHAR* What)
	{
		if (Slot.IsRoot())
		{
			Refuse(FString::Printf(TEXT("%s is an unbound placeholder"), What));
			Sink.U8(0);
			return;
		}
		WriteValue(Sink, Slot.GetRaw(), What);
	}

	void Refuse(const FString& What)
	{
		++NumRefusals;
		if (Errors.Num() < 40)
		{
			Errors.Add(What + TEXT(", ") + Describe(Current));
		}
	}

	bool Run(int32 Generation, TArray<uint8>& OutFile, FString& OutReport);

	int32 NumRefusals = 0;
	TArray<FString> Errors;
	TArray<FParkRow> ParkRows;
	int32 NumProcedures = 0;
	int32 NumNewClassOps = 0;
	/// Per package: how many of its classes carry EmulateCaseInsensitiveOverrides, of how many.
	TMap<FString, TPair<int32, int32>> ClassFlagTally;
	TArray<FString> UnkeyedNatives;

private:
	/// Reading an engine property can make a cell nothing else holds, and the slot table is not a
	/// root the collector knows about.
	void KeepAlive(Verse::VValue Value)
	{
		if (Value.IsCell())
		{
			Verse::TGlobalHeapPtr<Verse::VValue>& Root = *Rooted.Add_GetRef(MakeUnique<Verse::TGlobalHeapPtr<Verse::VValue>>());
			Root.Set(Context, Value);
		}
	}

	/// The definitions-table key each native procedure is bound by, which its decorated name alone
	/// does not identify: `event.Await` and `task.Await` decorate alike.
	void CollectNativeBindingKeys()
	{
		for (uint32 Index = 0; Index < Verse::GlobalProgram->NumPackages(); ++Index)
		{
			Verse::VPackage& Package = Verse::GlobalProgram->GetPackage(Index);
			for (uint32 Definition = 0; Definition < Package.NumDefinitions(); ++Definition)
			{
				Verse::VNativeProcedure* Native = NativeProcedureOf(Package.GetDefinition(Definition));
				if (!Native)
				{
					continue;
				}
				const FUtf8StringView Key = Package.GetDefinitionName(Definition).AsStringView();
				FUtf8String* Existing = NativeBindingKeys.Find(Native);
				if (!Existing)
				{
					NativeBindingKeys.Add(Native, FUtf8String(Key));
				}
				else if (!Existing->EndsWith(UTF8TEXT(":)Native")) && Key.EndsWith(UTF8TEXT(":)Native")))
				{
					*Existing = FUtf8String(Key);
				}
			}
		}
	}

	Verse::FAllocationContext Context;
	TArray<TUniquePtr<Verse::TGlobalHeapPtr<Verse::VValue>>> Rooted;
	TMap<const Verse::VNativeProcedure*, FUtf8String> NativeBindingKeys;

	struct FSlot
	{
		Verse::VCell* Cell = nullptr;
		UObject* Object = nullptr;
		int32 Parent = INDEX_NONE;
		int32 Definition = INDEX_NONE;
		TArray<uint8> Bytes;
	};

	FString CellName(int32 Index) const
	{
		const FSlot& Slot = Slots[Index];
		return Slot.Cell ? Slot.Cell->DebugName() : (Slot.Object ? Slot.Object->GetFullName() : FString(TEXT("?")));
	}

	/// What reached the cell being written: the nearest procedure on the path from a package, and
	/// the package definition the path started at.
	FString Describe(int32 Index) const
	{
		if (Index == INDEX_NONE)
		{
			return FString(TEXT("reached from the file's own tables"));
		}
		FString Result = FString::Printf(TEXT("in %s"), *CellName(Index));
		for (int32 At = Index; At != INDEX_NONE; At = Slots[At].Parent)
		{
			Verse::VProcedure* Procedure = Slots[At].Cell ? Slots[At].Cell->DynamicCast<Verse::VProcedure>() : nullptr;
			if (Procedure)
			{
				Result += FString::Printf(TEXT(", reached from procedure `%s` (%s)"),
				                          *FString(Procedure->Name->AsStringView()), *FString(Procedure->FilePath->AsStringView()));
				break;
			}
		}
		const int32 Definition = Slots[Index].Definition;
		if (Definition != INDEX_NONE)
		{
			Result += FString::Printf(TEXT(", under the definition `%s`"), *FString(Strings[Definition]));
		}
		return Result;
	}

	void EncodeCell(int32 Index);
	void EncodeObject(FSink& Sink, UObject& Object);
	void EncodePackage(FSink& Sink, Verse::VPackage& Package);
	void EncodeProcedure(FSink& Sink, Verse::VProcedure& Procedure);
	void EncodeClass(FSink& Sink, Verse::VClass& Class);
	void EncodeArchetype(FSink& Sink, Verse::VArchetype& Archetype);
	void EncodeArray(FSink& Sink, Verse::VArrayBase& Array);
	void EncodeValueObject(FSink& Sink, Verse::VValueObject& Object);
	bool EncodeType(FSink& Sink, Verse::VCell& Cell);

	void WriteArrayText(FSink& Sink, Verse::VArrayBase* Array, const TCHAR* What)
	{
		FUtf8String Text;
		if (!ArrayText(Array, Text))
		{
			Refuse(FString::Printf(TEXT("%s is not a string"), What));
		}
		WriteSid(Sink, Text);
	}

	void WriteNameOrEmpty(FSink& Sink, Verse::VUniqueString* Name)
	{
		WriteSid(Sink, Name ? Name->AsStringView() : FUtf8StringView());
	}

	friend struct FOpEncoder;

	TArray<FSlot> Slots;
	TMap<const void*, int32> SlotOf;
	TArray<FUtf8String> Strings;
	TMap<FUtf8String, uint32> StringIds;
	TMap<const UObject*, FUtf8String> ModulePaths;
	int32 Current = INDEX_NONE;
	int32 CurrentDefinition = INDEX_NONE;
};

/// The generated EncodeOp's other half: one method per operand kind, as HostVbcOps.gen.h calls them.
struct AUTORTFM_DISABLE FOpEncoder
{
	FVbcWriter& Writer;
	Verse::VProcedure& Procedure;
	const FOpMap& Map;
	FSink& Sink;
	bool bRegisterUse = false;

	void Opcode(uint32 Number) { Sink.Uv(Number); }

	void UnknownOpcode(Verse::FOp& Op)
	{
		Writer.Refuse(FString::Printf(TEXT("opcode %u is not in ops.json"), static_cast<uint32>(Op.Opcode)));
	}

	void Register(Verse::FRegisterIndex Register, ERole)
	{
		if (!Register)
		{
			Writer.Refuse(TEXT("a register operand ops.json does not mark optional is absent"));
		}
		Sink.Uv(Register.Index);
	}

	void OptRegister(Verse::FRegisterIndex Register, ERole)
	{
		Sink.U8(Register ? 1 : 0);
		if (Register)
		{
			Sink.Uv(Register.Index);
		}
	}

	void Value(const Verse::FValueOperand& Operand, ERole Role)
	{
		if (Operand.IsRegister())
		{
			Sink.Uv(1 + (static_cast<uint64>(Operand.Index) << 1));
			bRegisterUse |= Role == ERole::Use;
		}
		else if (Operand.IsConstant())
		{
			Sink.Uv(1 + ((static_cast<uint64>(Operand.AsConstant().Index) << 1) | 1));
		}
		else
		{
			Sink.Uv(0);
		}
	}

	void OptValue(const Verse::FValueOperand& Operand, ERole Role)
	{
		Sink.U8(Operand ? 1 : 0);
		if (Operand)
		{
			Value(Operand, Role);
		}
	}

	void ValueRange(Verse::TOperandRange<Verse::FValueOperand> Range, ERole Role)
	{
		Sink.Uv(static_cast<uint32>(Range.Num));
		for (int32 Index = 0; Index < Range.Num; ++Index)
		{
			Value(Procedure.GetOperandsBegin()[Range.Index + Index], Role);
		}
	}

	void ValueImm(const Verse::TWriteBarrier<Verse::VValue>& Immediate, ERole)
	{
		Writer.WriteValue(Sink, Immediate.Get(), TEXT("an immediate operand"));
	}

	void OptValueImm(const Verse::TWriteBarrier<Verse::VValue>& Immediate, ERole)
	{
		const bool bPresent = !Immediate.Get().IsUninitialized();
		Sink.U8(bPresent ? 1 : 0);
		if (bPresent)
		{
			Writer.WriteValue(Sink, Immediate.Get(), TEXT("an immediate operand"));
		}
	}

	/// A run of immediate values, which the emitter keeps in the constant pool.
	void ValueImmRange(Verse::TOperandRange<Verse::TWriteBarrier<Verse::VValue>> Range, ERole)
	{
		Sink.Uv(static_cast<uint32>(Range.Num));
		for (int32 Index = 0; Index < Range.Num; ++Index)
		{
			Writer.WriteValue(Sink, Procedure.GetConstantsBegin()[Range.Index + Index].Get(), TEXT("an immediate operand"));
		}
	}

	template <typename TCell>
	void Cell(const Verse::TWriteBarrier<TCell>& Immediate, ERole)
	{
		if (!Immediate.Get())
		{
			Writer.Refuse(TEXT("a cell operand ops.json does not mark optional is null"));
		}
		Writer.WriteRef(Sink, Immediate.Get());
	}

	template <typename TCell>
	void OptCell(const Verse::TWriteBarrier<TCell>& Immediate, ERole)
	{
		Sink.U8(Immediate.Get() ? 1 : 0);
		if (Immediate.Get())
		{
			Writer.WriteRef(Sink, Immediate.Get());
		}
	}

	/// A run of immediate cells, also kept in the constant pool.
	template <typename TCell>
	void CellRange(Verse::TOperandRange<Verse::TWriteBarrier<TCell>> Range, ERole)
	{
		Sink.Uv(static_cast<uint32>(Range.Num));
		for (int32 Index = 0; Index < Range.Num; ++Index)
		{
			const Verse::VValue Element = Procedure.GetConstantsBegin()[Range.Index + Index].Get().Follow();
			if (!Element.IsCell())
			{
				Writer.Refuse(TEXT("an element of a cell operand range is not a cell"));
				Sink.Uv(0);
				continue;
			}
			Writer.WriteRef(Sink, &Element.AsCell());
		}
	}

	/// Labels are self-relative, so this must be handed the label where it lives, never a copy.
	void Label(const Verse::FLabelOffset& Label, ERole)
	{
		const uint8* Target = reinterpret_cast<const uint8*>(Label.GetLabeledPC());
		const int64 Offset = Target - reinterpret_cast<const uint8*>(Procedure.GetOpsBegin());
		const int32 Index = Offset >= 0 && Offset <= Map.NumOpBytes ? Map.At(static_cast<uint32>(Offset)) : INDEX_NONE;
		if (Index == INDEX_NONE)
		{
			Writer.Refuse(FString::Printf(TEXT("a label targets byte %lld, which no op starts at"), static_cast<long long>(Offset)));
		}
		Sink.Uv(Index == INDEX_NONE ? 0 : static_cast<uint32>(Index));
	}

	void LabelRange(Verse::TOperandRange<Verse::FLabelOffset> Range, ERole Role)
	{
		Sink.Uv(static_cast<uint32>(Range.Num));
		for (int32 Index = 0; Index < Range.Num; ++Index)
		{
			Label(Procedure.GetLabelsBegin()[Range.Index + Index], Role);
		}
	}

	void LiveRange(const Verse::BytecodeAnalysis::FLiveRange& Range, ERole);

	void FailureContextId(Verse::FFailureContextId Id, ERole) { Sink.Uv(Id.Id); }

	void AssetPath(const FTopLevelAssetPath& Path, ERole)
	{
		Writer.WriteSid(Sink, FUtf8String(Path.GetPackageName().ToString()));
		Writer.WriteSid(Sink, FUtf8String(Path.GetAssetName().ToString()));
	}

	void Bool(bool bValue, ERole) { Sink.U8(bValue ? 1 : 0); }
	void I32(int32 Value, ERole) { Sink.Sv(Value); }
	void U32(uint32 Value, ERole) { Sink.Uv(Value); }
	void ClassKind(Verse::VClass::EKind Value, ERole) { Sink.Uv(static_cast<uint8>(Value)); }
	void ClassFlags(Verse::VClass::EFlags Value, ERole) { Sink.Uv(static_cast<uint16>(Value)); }
};

/// A live range is an inclusive pair of byte offsets, each the start of an op, with two sentinels: the
/// whole procedure is [0, UINT32_MAX] and a dead range has Begin past End. Both carry over as op
/// indices -- the whole procedure as [0, last op], a dead one as [op count, 0].
AUTORTFM_DISABLE void WriteLiveRange(FSink& Sink, const FOpMap& Map, const Verse::BytecodeAnalysis::FLiveRange& Range)
{
	const uint32 Max = TNumericLimits<uint32>::Max();
	const uint32 NumOps = static_cast<uint32>(Map.Starts.Num());
	const uint32 First = Range.Begin == Max ? NumOps : static_cast<uint32>(Map.Containing(Range.Begin));
	const uint32 Last = Range.End == Max ? (NumOps > 0 ? NumOps - 1 : 0) : static_cast<uint32>(Map.Containing(Range.End));
	Sink.Uv(First);
	Sink.Uv(Last);
}

void FOpEncoder::LiveRange(const Verse::BytecodeAnalysis::FLiveRange& Range, ERole)
{
	WriteLiveRange(Sink, Map, Range);
}

void FVbcWriter::EncodeCell(int32 Index)
{
	Current = Index;
	TArray<uint8> Bytes;
	FSink Sink{Bytes};

	if (UObject* Object = Slots[Index].Object)
	{
		EncodeObject(Sink, *Object);
		Slots[Index].Bytes = MoveTemp(Bytes);
		return;
	}

	Verse::VCell& Cell = *Slots[Index].Cell;
	if (&Cell == &Verse::GlobalFalse())
	{
		Sink.U8(Kind::False);
	}
	else if (&Cell == &Verse::GlobalTrue())
	{
		Sink.U8(Kind::True);
	}
	else if (Verse::VPackage* Package = Cell.DynamicCast<Verse::VPackage>())
	{
		if (IsBuiltInPackage(*Package))
		{
			Sink.U8(Kind::BuiltinPackage);
		}
		else
		{
			Sink.U8(Kind::Package);
			EncodePackage(Sink, *Package);
		}
	}
	else if (Verse::VUniqueString* Name = Cell.DynamicCast<Verse::VUniqueString>())
	{
		Sink.U8(Kind::Name);
		WriteSid(Sink, Name->AsStringView());
	}
	else if (Verse::VMutableArray* MutableArray = Cell.DynamicCast<Verse::VMutableArray>())
	{
		Sink.U8(Kind::MutableArray);
		EncodeArray(Sink, *MutableArray);
	}
	else if (Verse::VArray* Array = Cell.DynamicCast<Verse::VArray>())
	{
		Sink.U8(Kind::Array);
		EncodeArray(Sink, *Array);
	}
	else if (Cell.IsA<Verse::VPersistentMap>())
	{
		Refuse(TEXT("a persistent map has no kind in format version 1"));
		Sink.U8(0);
	}
	else if (Verse::VMapBase* MapBase = Cell.DynamicCast<Verse::VMapBase>())
	{
		Sink.U8(Cell.IsA<Verse::VMutableMap>() ? Kind::MutableMap : Kind::Map);
		Sink.Uv(MapBase->Num());
		for (TPair<Verse::VValue, Verse::VValue> Pair : *MapBase)
		{
			WriteValue(Sink, Pair.Key, TEXT("a map key"));
			WriteValue(Sink, Pair.Value, TEXT("a map value"));
		}
	}
	else if (Verse::VOption* Option = Cell.DynamicCast<Verse::VOption>())
	{
		Sink.U8(Kind::Option);
		WriteValue(Sink, Option->GetValue(), TEXT("an option's value"));
	}
	else if (Verse::VHeapInt* HeapInt = Cell.DynamicCast<Verse::VHeapInt>())
	{
		Sink.U8(Kind::HeapInt);
		Sink.U8(HeapInt->GetSign() ? 1 : 0);
		TArray<uint8> Magnitude;
		for (uint32 Word = 0; Word < HeapInt->GetLength(); ++Word)
		{
			const Verse::VHeapInt::Digit Digit = HeapInt->GetDigit(Word);
			for (int32 Shift = 0; Shift < 32; Shift += 8)
			{
				Magnitude.Add(static_cast<uint8>(Digit >> Shift));
			}
		}
		while (Magnitude.Num() > 0 && Magnitude.Last() == 0)
		{
			Magnitude.Pop();
		}
		Sink.Uv(Magnitude.Num());
		Bytes.Append(Magnitude);
	}
	else if (Verse::VRational* Rational = Cell.DynamicCast<Verse::VRational>())
	{
		Sink.U8(Kind::Rational);
		WriteValue(Sink, Rational->Numerator.Get(), TEXT("a rational's numerator"));
		WriteValue(Sink, Rational->Denominator.Get(), TEXT("a rational's denominator"));
	}
	else if (Verse::VProcedure* Procedure = Cell.DynamicCast<Verse::VProcedure>())
	{
		Sink.U8(Kind::Procedure);
		EncodeProcedure(Sink, *Procedure);
	}
	else if (Verse::VNativeProcedure* Native = Cell.DynamicCast<Verse::VNativeProcedure>())
	{
		Sink.U8(Kind::NativeProcedure);
		const FUtf8String* Key = NativeBindingKeys.Find(Native);
		if (!Key)
		{
			UnkeyedNatives.Add(FString(Native->Name ? Native->Name->AsStringView() : FUtf8StringView()) + TEXT(" ") + Describe(Index));
		}
		WriteSid(Sink, Key ? FUtf8StringView(*Key) : FUtf8StringView());
		WriteNameOrEmpty(Sink, Native->Name.Get());
		Sink.Uv(Native->NumPositionalParameters);
	}
	else if (Verse::VFunction* Function = Cell.DynamicCast<Verse::VFunction>())
	{
		Sink.U8(Kind::Function);
		const Verse::VValue Callee = Function->Procedure.Get().Follow();
		if (!Callee.IsCell())
		{
			Refuse(TEXT("a function's procedure is not a cell"));
			Sink.Uv(0);
		}
		else
		{
			WriteRef(Sink, &Callee.AsCell());
		}
		WriteValue(Sink, Function->Self.Get(), TEXT("a function's self"));
		WriteRef(Sink, Function->ParentScope.Get());
	}
	else if (Verse::VScope* Scope = Cell.DynamicCast<Verse::VScope>())
	{
		Sink.U8(Kind::Scope);
		WriteRef(Sink, Scope->ParentScope.Get());
		Sink.Uv(Scope->NumCaptures);
		for (uint32 Capture = 0; Capture < Scope->NumCaptures; ++Capture)
		{
			WriteValue(Sink, Scope->Captures[Capture].Get(), TEXT("a scope's capture"));
		}
	}
	else if (Verse::VClass* Class = Cell.DynamicCast<Verse::VClass>())
	{
		Sink.U8(Kind::Class);
		EncodeClass(Sink, *Class);
	}
	else if (Verse::VArchetype* Archetype = Cell.DynamicCast<Verse::VArchetype>())
	{
		Sink.U8(Kind::Archetype);
		EncodeArchetype(Sink, *Archetype);
	}
	else if (Verse::VAccessSpecifier* Access = Cell.DynamicCast<Verse::VAccessSpecifier>())
	{
		Sink.U8(Kind::AccessSpecifier);
		Sink.U8(static_cast<uint8>(Access->Kind));
		Verse::VArray* Paths = Access->ScopePaths.Get();
		const uint32 NumPaths = Paths ? Paths->Num() : 0;
		Sink.Uv(NumPaths);
		for (uint32 Path = 0; Path < NumPaths; ++Path)
		{
			const Verse::VValue Element = Paths->GetValue(Path).Follow();
			Verse::VArrayBase* Text = Element.IsCell() ? Element.AsCell().DynamicCast<Verse::VArrayBase>() : nullptr;
			if (!Text)
			{
				Refuse(TEXT("an access specifier's scope path is not a string"));
			}
			WriteArrayText(Sink, Text, TEXT("an access specifier's scope path"));
		}
	}
	else if (Verse::VAccessor* Accessor = Cell.DynamicCast<Verse::VAccessor>())
	{
		// Slot s holds the getter of s + 1 parameters and the setter of s + 2.
		Sink.U8(Kind::Accessor);
		const uint32 NumAccessors = Reach<FAccessorCountOf>(*Accessor);
		Sink.Uv(NumAccessors);
		for (uint32 Slot = 0; Slot < NumAccessors; ++Slot)
		{
			WriteNameOrEmpty(Sink, Accessor->FindGetter(Slot + 1).Get());
		}
		Sink.Uv(NumAccessors);
		for (uint32 Slot = 0; Slot < NumAccessors; ++Slot)
		{
			WriteNameOrEmpty(Sink, Accessor->FindSetter(Slot + 2).Get());
		}
	}
	else if (Verse::VEnumeration* Enumeration = Cell.DynamicCast<Verse::VEnumeration>())
	{
		Sink.U8(Kind::Enumeration);
		WriteArrayText(Sink, Reach<FNamedBaseNameOf>(*Enumeration).Get(), TEXT("an enumeration's name"));
		Sink.Uv(static_cast<uint32>(Enumeration->NumEnumerators));
		for (int32 Enumerator = 0; Enumerator < Enumeration->NumEnumerators; ++Enumerator)
		{
			WriteRef(Sink, &Enumeration->GetEnumeratorChecked(Enumerator));
		}
	}
	else if (Verse::VEnumerator* Enumerator = Cell.DynamicCast<Verse::VEnumerator>())
	{
		Sink.U8(Kind::Enumerator);
		WriteRef(Sink, Enumerator->GetEnumeration());
		WriteNameOrEmpty(Sink, Enumerator->GetName());
		Sink.Uv(static_cast<uint32>(Enumerator->GetIntValue()));
	}
	else if (Cell.IsA<Verse::VUnion>() || Cell.IsA<Verse::VUnionVariant>() || Cell.IsA<Verse::VUnionVariantTag>())
	{
		Refuse(TEXT("a union cell has no kind in format version 1"));
		Sink.U8(0);
	}
	else if (Cell.IsA<Verse::VTask>())
	{
		Refuse(TEXT("a task has no kind in format version 1"));
		Sink.U8(0);
	}
	else if (Cell.IsA<Verse::VNativeStruct>())
	{
		Refuse(TEXT("a native struct has no kind in format version 1"));
		Sink.U8(0);
	}
	else if (Verse::VValueObject* ValueObject = Cell.DynamicCast<Verse::VValueObject>())
	{
		Sink.U8(Kind::ValueObject);
		EncodeValueObject(Sink, *ValueObject);
	}
	else if (!EncodeType(Sink, Cell))
	{
		Refuse(FString::Printf(TEXT("a cell of C++ type %s has no kind in format version 1"),
		                       Cell.GetCppClassInfo() ? Cell.GetCppClassInfo()->Name : TEXT("?")));
		Sink.U8(0);
	}

	Slots[Index].Bytes = MoveTemp(Bytes);
}

bool FVbcWriter::EncodeType(FSink& Sink, Verse::VCell& Cell)
{
	if (Verse::VIntType* IntType = Cell.DynamicCast<Verse::VIntType>())
	{
		Sink.U8(Kind::IntType);
		WriteValue(Sink, IntType->GetMin(), TEXT("an int type's lower bound"));
		WriteValue(Sink, IntType->GetMax(), TEXT("an int type's upper bound"));
		return true;
	}
	if (Verse::VFloatType* FloatType = Cell.DynamicCast<Verse::VFloatType>())
	{
		Sink.U8(Kind::FloatType);
		Sink.U8(2);
		Sink.F64(FloatType->GetMin().AsDouble());
		Sink.U8(2);
		Sink.F64(FloatType->GetMax().AsDouble());
		return true;
	}
	if (Verse::VTupleType* TupleType = Cell.DynamicCast<Verse::VTupleType>())
	{
		Sink.U8(Kind::TupleType);
		const TArrayView<Verse::TWriteBarrier<Verse::VValue>> Elements = TupleType->GetElements();
		Sink.Uv(Elements.Num());
		for (Verse::TWriteBarrier<Verse::VValue>& Element : Elements)
		{
			WriteValue(Sink, Element.Get(), TEXT("a tuple type's element"));
		}
		return true;
	}
	if (Verse::VMapType* MapType = Cell.DynamicCast<Verse::VMapType>())
	{
		Sink.U8(Kind::MapType);
		WriteValue(Sink, MapType->KeyType.Get(), TEXT("a map type's key type"));
		WriteValue(Sink, MapType->ValueType.Get(), TEXT("a map type's value type"));
		return true;
	}

	auto ElementType = [&](uint8 TypeKind, Verse::VValue Element) {
		Sink.U8(TypeKind);
		WriteValue(Sink, Element, TEXT("a type's element type"));
		return true;
	};
	if (Verse::VArrayType* ArrayType = Cell.DynamicCast<Verse::VArrayType>())
	{
		return ElementType(Kind::ArrayType, ArrayType->ElementType.Get());
	}
	if (Verse::VOptionType* OptionType = Cell.DynamicCast<Verse::VOptionType>())
	{
		return ElementType(Kind::OptionType, OptionType->ValueType.Get());
	}
	if (Verse::VPointerType* PointerType = Cell.DynamicCast<Verse::VPointerType>())
	{
		return ElementType(Kind::PointerType, PointerType->ValueType.Get());
	}

	// A simple type is its code; the few that are built from other types -- type, generator,
	// weak_map, concrete, castable -- are followed by those, in the order the engine declares them.
	// format.md lists these codes without the trailing values.
	auto Simple = [&](uint8 Code, std::initializer_list<Verse::VValue> Parts) {
		Sink.U8(Kind::SimpleType);
		Sink.U8(Code);
		for (Verse::VValue Part : Parts)
		{
			WriteValue(Sink, Part, TEXT("a type's component"));
		}
		return true;
	};
	if (Cell.IsA<Verse::VAnyType>())
	{
		return Simple(0, {});
	}
	if (Cell.IsA<Verse::VVoidType>())
	{
		return Simple(1, {});
	}
	if (Cell.IsA<Verse::VComparableType>())
	{
		return Simple(2, {});
	}
	if (Cell.IsA<Verse::VLogicType>())
	{
		return Simple(3, {});
	}
	if (Cell.IsA<Verse::VRationalType>())
	{
		return Simple(4, {});
	}
	if (Cell.IsA<Verse::VChar8Type>())
	{
		return Simple(5, {});
	}
	if (Cell.IsA<Verse::VChar32Type>())
	{
		return Simple(6, {});
	}
	if (Cell.IsA<Verse::VRangeType>())
	{
		return Simple(7, {});
	}
	if (Verse::VTypeType* TypeType = Cell.DynamicCast<Verse::VTypeType>())
	{
		return Simple(8, {TypeType->PositiveType.Get()});
	}
	if (Verse::VGeneratorType* Generator = Cell.DynamicCast<Verse::VGeneratorType>())
	{
		return Simple(10, {Generator->ElementType.Get()});
	}
	if (Verse::VWeakMapType* WeakMap = Cell.DynamicCast<Verse::VWeakMapType>())
	{
		return Simple(11, {WeakMap->KeyType.Get(), WeakMap->ValueType.Get()});
	}
	if (Cell.IsA<Verse::VReferenceType>())
	{
		return Simple(13, {});
	}
	if (Verse::VConcreteType* Concrete = Cell.DynamicCast<Verse::VConcreteType>())
	{
		return Simple(15, {Concrete->SuperType.Get()});
	}
	if (Verse::VCastableType* Castable = Cell.DynamicCast<Verse::VCastableType>())
	{
		return Simple(16, {Castable->SuperType.Get()});
	}
	if (Cell.IsA<Verse::VFunctionType>())
	{
		return Simple(17, {});
	}
	if (Cell.IsA<Verse::VPersistableType>())
	{
		return Simple(18, {});
	}
	return false;
}

void FVbcWriter::EncodeObject(FSink& Sink, UObject& Object)
{
	if (IsModule(Object))
	{
		Sink.U8(Kind::Module);
		const FUtf8String* Path = ModulePaths.Find(&Object);
		if (!Path)
		{
			Refuse(TEXT("a module no package definition names"));
		}
		WriteSid(Sink, Path ? FUtf8StringView(*Path) : FUtf8StringView());
		WriteSid(Sink, FUtf8String(Object.GetName()));
		return;
	}

	// A module-level object of a native-represented class is an engine object, and its fields are the
	// engine properties its class's shape names. The shape's constants are the class's own.
	UVerseClass* VerseClass = Cast<UVerseClass>(Object.GetClass());
	Verse::VClass* Class = VerseClass ? VerseClass->Class.Get() : nullptr;
	const Verse::VValue ShapeValue = VerseClass ? VerseClass->Shape.GetRaw().Follow() : Verse::VValue();
	Verse::VShape* Shape = ShapeValue.IsCell() ? ShapeValue.AsCell().DynamicCast<Verse::VShape>() : nullptr;
	if (!Class || !Shape)
	{
		Refuse(FString::Printf(TEXT("an engine object of class %s has no Verse class with a layout"), *Object.GetClass()->GetName()));
		Sink.U8(0);
		return;
	}
	Sink.U8(Kind::ValueObject);
	WriteRef(Sink, Class);
	TArray<TPair<Verse::VUniqueString*, Verse::VValue>> Fields;
	for (auto It = Shape->CreateFieldsIterator(); It; ++It)
	{
		if (It->Value.Type == Verse::EFieldType::Constant)
		{
			continue;
		}
		if (!It->Value.IsProperty())
		{
			Refuse(TEXT("an engine object's field is not an engine property"));
			continue;
		}
		const Verse::VValue Value = UVerseClass::PeekField(Context, &Object, &It->Value);
		KeepAlive(Value);
		Fields.Emplace(It->Key.Get(), Value);
	}
	Sink.Uv(Fields.Num());
	for (const TPair<Verse::VUniqueString*, Verse::VValue>& Field : Fields)
	{
		WriteNameOrEmpty(Sink, Field.Key);
		WriteValue(Sink, Field.Value, TEXT("an engine object's field"));
	}
}

void FVbcWriter::EncodePackage(FSink& Sink, Verse::VPackage& Package)
{
	WriteSid(Sink, Package.GetName().AsStringView());
	WriteSid(Sink, Package.GetRootPath().AsStringView());
	const uint32 NumDefinitions = Package.NumDefinitions();
	Sink.Uv(NumDefinitions);
	for (uint32 Definition = 0; Definition < NumDefinitions; ++Definition)
	{
		const FUtf8StringView Path = Package.GetDefinitionName(Definition).AsStringView();
		const uint32 PathSid = Sid(Path);
		Sink.Uv(PathSid);
		const Verse::VValue Value = Package.GetDefinition(Definition);
		if (Value.IsUObject() && IsModule(*Value.AsUObject()))
		{
			ModulePaths.FindOrAdd(Value.AsUObject(), FUtf8String(Path));
		}
		CurrentDefinition = static_cast<int32>(PathSid);
		WriteValue(Sink, Value, TEXT("a package definition"));
		CurrentDefinition = INDEX_NONE;
	}
}

void FVbcWriter::EncodeArray(FSink& Sink, Verse::VArrayBase& Array)
{
	const uint32 Num = Array.Num();
	switch (Array.GetArrayType())
	{
		case Verse::EArrayType::None:
			Sink.U8(0);
			break;
		case Verse::EArrayType::VValue:
			Sink.U8(1);
			Sink.Uv(Num);
			for (uint32 Index = 0; Index < Num; ++Index)
			{
				WriteValue(Sink, Array.GetValue(Index), TEXT("an array element"));
			}
			break;
		case Verse::EArrayType::Int32:
			Sink.U8(2);
			Sink.Uv(Num);
			for (uint32 Index = 0; Index < Num; ++Index)
			{
				Sink.Sv(Array.GetData<int32>()[Index]);
			}
			break;
		case Verse::EArrayType::Char8:
			Sink.U8(3);
			Sink.Str(FUtf8StringView(Array.GetData<UTF8CHAR>(), Num));
			break;
		case Verse::EArrayType::Char32:
			Sink.U8(4);
			Sink.Uv(Num);
			for (uint32 Index = 0; Index < Num; ++Index)
			{
				Sink.Uv(static_cast<uint32>(Array.GetData<UTF32CHAR>()[Index]));
			}
			break;
		default:
			Refuse(TEXT("an array of an element kind the format does not have"));
			Sink.U8(0);
			break;
	}
}

void FVbcWriter::EncodeClass(FSink& Sink, Verse::VClass& Class)
{
	const Verse::VClass::EFlags Flags = Reach<FClassFlagsOf>(Class);
	Sink.U8(static_cast<uint8>(Class.GetKind()));
	Sink.Uv(static_cast<uint16>(Flags));

	FUtf8String BaseName;
	ArrayText(Reach<FNamedBaseNameOf>(Class).Get(), BaseName);
	Verse::VPackage* ClassPackage = Reach<FNamedPackageOf>(Class).Get();
	const FString PackageName = ClassPackage ? FString(ClassPackage->GetName().AsStringView()) : FString(TEXT("(none)"));
	TPair<int32, int32>& Tally = ClassFlagTally.FindOrAdd(PackageName);
	++Tally.Value;
	if (EnumHasAnyFlags(Flags, Verse::VClass::EFlags::EmulateCaseInsensitiveOverrides))
	{
		++Tally.Key;
	}

	// An `@import_as` type is the one kind of native type the format has no room for; the UClass or
	// UScriptStruct the engine makes for every Verse class is its own business.
	if (UField* NativeType = Class.GetUEType<UField>())
	{
		if (!NativeType->IsA<UVerseClass>() && !NativeType->IsA<UVerseStruct>())
		{
			Refuse(FString::Printf(TEXT("the class reflects the engine type %s (@import_as)"), *NativeType->GetPathName()));
		}
	}

	WriteRef(Sink, Reach<FNamedPackageOf>(Class).Get());
	WriteArrayText(Sink, Reach<FNamedRelativePathOf>(Class).Get(), TEXT("a class's relative path"));
	WriteSid(Sink, BaseName);

	Verse::VArray* Attributes = Reach<FNamedAttributesOf>(Class).Get();
	Verse::VArray* AttributeIndices = Reach<FNamedAttributeIndicesOf>(Class).Get();
	if (!Attributes != !AttributeIndices)
	{
		Refuse(TEXT("a class has attributes without attribute indices, or the reverse"));
	}
	Sink.U8(Attributes && AttributeIndices ? 1 : 0);
	if (Attributes && AttributeIndices)
	{
		Sink.Uv(Attributes->Num());
		for (uint32 Index = 0; Index < Attributes->Num(); ++Index)
		{
			WriteValue(Sink, Attributes->GetValue(Index), TEXT("a class attribute"));
		}
		Sink.Uv(AttributeIndices->Num());
		for (uint32 Index = 0; Index < AttributeIndices->Num(); ++Index)
		{
			const Verse::VValue Element = AttributeIndices->GetValue(Index).Follow();
			if (!Element.IsInt32() || Element.AsInt32() < 0)
			{
				Refuse(TEXT("a class attribute index is not a small non-negative int"));
			}
			Sink.Uv(Element.IsInt32() ? static_cast<uint32>(FMath::Max(Element.AsInt32(), 0)) : 0);
		}
	}

	const TArrayView<Verse::TWriteBarrier<Verse::VClass>> Inherited = Class.GetInherited();
	Sink.Uv(Inherited.Num());
	for (Verse::TWriteBarrier<Verse::VClass>& Base : Inherited)
	{
		WriteRef(Sink, Base.Get());
	}
	WriteRef(Sink, &Class.GetArchetype());
	WriteRef(Sink, Reach<FClassConstructorOf>(Class).Get());

	Verse::VFunction* Blocks = Reach<FClassBlocksOf>(Class).Get();
	const bool bIsClass = Class.GetKind() == Verse::VClass::EKind::Class;
	if (bIsClass != (Blocks != nullptr))
	{
		Refuse(bIsClass ? TEXT("a class has no blocks function") : TEXT("a struct or interface has a blocks function"));
	}
	WriteRef(Sink, bIsClass ? Blocks : nullptr);
	Sink.U8(Class.IsNativeBound() ? 1 : 0);
}

void FVbcWriter::EncodeArchetype(FSink& Sink, Verse::VArchetype& Archetype)
{
	// Class and NextArchetype are never bound for an instantiation-expression archetype, so an
	// unbound slot here is "none" rather than an error.
	auto WriteOptionalCell = [&](Verse::VRestValue& Slot, const TCHAR* What) {
		if (Slot.IsRoot() || Slot.IsUninitialized())
		{
			Sink.Uv(0);
			return;
		}
		const Verse::VValue Value = Slot.GetRaw().Follow();
		if (Value.IsCell())
		{
			WriteRef(Sink, &Value.AsCell());
			return;
		}
		if (!Value.IsPlaceholder() && !Value.IsUninitialized())
		{
			Refuse(FString::Printf(TEXT("%s is not a cell"), What));
		}
		Sink.Uv(0);
	};
	WriteOptionalCell(Archetype.Class, TEXT("an archetype's class"));
	WriteOptionalCell(Archetype.NextArchetype, TEXT("an archetype's next archetype"));

	Sink.Uv(Archetype.NumEntries);
	for (Verse::VArchetype::VEntry& Entry : Archetype.GetEntries())
	{
		WriteNameOrEmpty(Sink, Entry.Name.Get());
		WriteRef(Sink, Entry.Access.Get());
		WriteValue(Sink, Entry.Type.Get(), TEXT("an archetype entry's type"));
		WriteValue(Sink, Entry.Value.Get(), TEXT("an archetype entry's value"));
		Sink.U8(static_cast<uint8>(Entry.Flags));
	}
}

void FVbcWriter::EncodeValueObject(FSink& Sink, Verse::VValueObject& Object)
{
	Verse::VClass& Class = Object.GetClass();
	if (Class.IsNativeRepresentation())
	{
		Refuse(TEXT("a value object of a native-represented class"));
	}
	WriteRef(Sink, &Class);

	// The object's own slots, in the shape's order. A shape constant belongs to the class.
	Verse::VEmergentType* EmergentType = Object.GetEmergentType();
	Verse::VShape* Shape = EmergentType ? EmergentType->Shape.Get() : nullptr;
	TArray<TPair<Verse::VUniqueString*, uint64>> Fields;
	if (Shape)
	{
		for (auto It = Shape->CreateFieldsIterator(); It; ++It)
		{
			if (It->Value.Type == Verse::EFieldType::Offset)
			{
				Fields.Emplace(It->Key.Get(), It->Value.Index);
			}
			else if (It->Value.Type != Verse::EFieldType::Constant)
			{
				Refuse(TEXT("a value object field held in a native property"));
			}
		}
	}
	Verse::VRestValue* Data = Fields.Num() > 0
		? (static_cast<Verse::VObject&>(Object).*MemberPointer(FObjectFieldDataOf{}))(*EmergentType->CppClassInfo)
		: nullptr;
	Sink.Uv(Fields.Num());
	for (const TPair<Verse::VUniqueString*, uint64>& Field : Fields)
	{
		WriteNameOrEmpty(Sink, Field.Key);
		WriteRestValue(Sink, Data[Field.Value], TEXT("a value object's field"));
	}
}

void FVbcWriter::EncodeProcedure(FSink& Sink, Verse::VProcedure& Procedure)
{
	FOpMap Map;
	Map.NumOpBytes = Procedure.NumOpBytes;
	for (uint8* At = reinterpret_cast<uint8*>(Procedure.GetOpsBegin()); At < reinterpret_cast<uint8*>(Procedure.GetOpsEnd());)
	{
		Verse::FOp* Op = reinterpret_cast<Verse::FOp*>(At);
		const uint32 Size = GodotVerse::Vbc::OpSize(*Op);
		if (Size == 0)
		{
			Refuse(FString::Printf(TEXT("opcode %u is not in ops.json; the rest of the procedure cannot be walked"),
			                       static_cast<uint32>(Op->Opcode)));
			break;
		}
		Map.Ops.Add(Op);
		Map.Starts.Add(static_cast<uint32>(At - reinterpret_cast<uint8*>(Procedure.GetOpsBegin())));
		At += Size;
	}

	WriteNameOrEmpty(Sink, Procedure.Name.Get());
	WriteNameOrEmpty(Sink, Procedure.FilePath.Get());
	Sink.Uv(Procedure.bCanAccessEpicInternal ? 1 : 0);
	Sink.Uv(Procedure.NumRegisters);
	Sink.Uv(Procedure.NumPositionalParameters);

	Sink.Uv(Procedure.NumNamedParameters);
	for (Verse::FNamedParam* Param = Procedure.GetNamedParamsBegin(); Param != Procedure.GetNamedParamsEnd(); ++Param)
	{
		WriteNameOrEmpty(Sink, Param->Name.Get());
		Sink.Uv(Param->Index.Index);
	}

	Sink.Uv(Procedure.NumConstants);
	for (uint32 Constant = 0; Constant < Procedure.NumConstants; ++Constant)
	{
		WriteValue(Sink, Procedure.GetConstantsBegin()[Constant].Get(), TEXT("a constant"));
	}

	FParkRow Row;
	Row.Cell = Current;
	Row.Name = Procedure.Name ? FUtf8String(Procedure.Name->AsStringView()) : FUtf8String();
	Row.File = Procedure.FilePath ? FUtf8String(Procedure.FilePath->AsStringView()) : FUtf8String();
	Row.Ops = Map.Ops.Num();

	Sink.Uv(Map.Ops.Num());
	for (Verse::FOp* Op : Map.Ops)
	{
		FOpEncoder Encoder{*this, Procedure, Map, Sink};
		GodotVerse::Vbc::EncodeOp(Encoder, *Op);
		const uint32 Opcode = static_cast<uint32>(Op->Opcode);
		NumNewClassOps += Opcode == NewClassOpcode ? 1 : 0;
		if (Opcode < GodotVerse::Vbc::OpCount && GodotVerse::Vbc::OpMayPark[Opcode])
		{
			++Row.MayPark;
			Row.MayParkOnRegister += Encoder.bRegisterUse ? 1 : 0;
		}
	}
	ParkRows.Add(MoveTemp(Row));

	// The engine covers a frame whose resume position p satisfies Begin < p <= End, both byte offsets
	// of op boundaries; format.md tests the index i of the op the frame is stopped in, whose resume
	// position is the start of op i + 1. Begin < start(i + 1) is i >= op at Begin, and start(i + 1) <=
	// End is i <= op at End - 1, so first is the op at Begin and last is the op before End.
	Sink.Uv(Procedure.NumUnwindEdges);
	for (Verse::FUnwindEdge* Edge = Procedure.GetUnwindEdgesBegin(); Edge != Procedure.GetUnwindEdgesEnd(); ++Edge)
	{
		const int32 First = Map.At(static_cast<uint32>(Edge->Begin));
		const int32 End = Map.At(static_cast<uint32>(Edge->End));
		if (First == INDEX_NONE || End == INDEX_NONE || End <= First)
		{
			Refuse(FString::Printf(TEXT("an unwind edge (%d, %d] does not span whole ops"), Edge->Begin, Edge->End));
		}
		Sink.Uv(First == INDEX_NONE ? 0 : static_cast<uint32>(First));
		Sink.Uv(End == INDEX_NONE || End <= 0 ? 0 : static_cast<uint32>(End - 1));
		FOpEncoder Encoder{*this, Procedure, Map, Sink};
		Encoder.Label(Edge->OnUnwind, ERole::Jump);
	}

	Sink.Uv(Procedure.NumOpLocations);
	for (Verse::FOpLocation* Location = Procedure.GetOpLocationsBegin(); Location != Procedure.GetOpLocationsEnd(); ++Location)
	{
		const int32 Op = Map.At(static_cast<uint32>(Location->Begin));
		if (Op == INDEX_NONE)
		{
			Refuse(FString::Printf(TEXT("a source location starts at byte %d, which no op starts at"), Location->Begin));
		}
		Sink.Uv(Op == INDEX_NONE ? 0 : static_cast<uint32>(Op));
		Sink.Uv(Location->Location.Line);
	}

	Sink.Uv(Procedure.NumRegisterNames);
	for (Verse::FRegisterName* Register = Procedure.GetRegisterNamesBegin(); Register != Procedure.GetRegisterNamesEnd(); ++Register)
	{
		Sink.Uv(Register->Index.Index);
		WriteNameOrEmpty(Sink, Register->Name.Get());
		WriteLiveRange(Sink, Map, Register->LiveRange);
	}

	++NumProcedures;
}

/// `(/user@localhost/gameplay:)player` for the sidecar's `gameplay/player`: the module is part of
/// the scope, as FindClassInPackage decorates it.
AUTORTFM_DISABLE FUtf8String DecorateScriptClass(FUtf8StringView Key)
{
	FUtf8String Scope(UTF8TEXT("/user@localhost"));
	FUtf8String Leaf(Key);
	int32 LastSlash = INDEX_NONE;
	if (Leaf.FindLastChar(UTF8CHAR('/'), LastSlash))
	{
		Scope += UTF8TEXT("/");
		Scope += Leaf.Left(LastSlash);
		Leaf.RightChopInline(LastSlash + 1);
	}
	return FUtf8String(UTF8TEXT("(")) + Scope + UTF8TEXT(":)") + Leaf;
}

AUTORTFM_DISABLE Verse::VClass* FindClassDecorated(FUtf8StringView Decorated)
{
	for (uint32 Index = 0; Index < Verse::GlobalProgram->NumPackages(); ++Index)
	{
		if (Verse::VClass* Class = Verse::GlobalProgram->GetPackage(Index).LookupDefinition<Verse::VClass>(Decorated))
		{
			return Class;
		}
	}
	return nullptr;
}

bool FVbcWriter::Run(int32 Generation, TArray<uint8>& OutFile, FString& OutReport)
{
	CollectNativeBindingKeys();

	TArray<uint8> PackagesBytes;
	FSink Packages{PackagesBytes};
	TArray<Verse::VPackage*> Written;
	for (uint32 Index = 0; Index < Verse::GlobalProgram->NumPackages(); ++Index)
	{
		Verse::VPackage& Package = Verse::GlobalProgram->GetPackage(Index);
		if (!IsBuiltInPackage(Package))
		{
			Written.Add(&Package);
			SlotFor(&Package, nullptr);
		}
	}
	Packages.Uv(Written.Num());
	for (Verse::VPackage* Package : Written)
	{
		WriteRef(Packages, Package);
	}

	TArray<uint8> WellKnownBytes;
	FSink WellKnown{WellKnownBytes};
	Verse::VClass* TaskClass = nullptr;
	if (Verse::VEmergentType* TaskType = Verse::VTask::EmergentType.Get())
	{
		Verse::VType* Type = TaskType->Type.Get();
		TaskClass = Type ? Type->DynamicCast<Verse::VClass>() : nullptr;
	}
	Verse::VEnumerator* AccessorEnumerator = Verse::VAccessorRef::AccessorEnum.Get();
	TArray<TPair<const UTF8CHAR*, Verse::VCell*>> Roles;
	if (TaskClass)
	{
		Roles.Emplace(UTF8TEXT("task_class"), TaskClass);
	}
	else
	{
		Errors.Add(TEXT("the program has no task class to name as `task_class`"));
		++NumRefusals;
	}
	if (AccessorEnumerator)
	{
		Roles.Emplace(UTF8TEXT("accessor_enumerator"), AccessorEnumerator);
	}
	else
	{
		Errors.Add(TEXT("the program has no accessor enumerator to name as `accessor_enumerator`"));
		++NumRefusals;
	}
	WellKnown.Uv(Roles.Num());
	for (const TPair<const UTF8CHAR*, Verse::VCell*>& Role : Roles)
	{
		WriteSid(WellKnown, Role.Key);
		WriteRef(WellKnown, Role.Value);
	}

	struct FIndexEntry
	{
		uint8 Origin;
		FUtf8String Name;
		Verse::VClass* Class;
	};
	TArray<FIndexEntry> Entries;
	if (const TSharedPtr<const GodotVerse::FAnalysisSnapshot>& Snapshot = GodotVerse::GetAnalysisSnapshot())
	{
		for (const TPair<FUtf8String, GodotVerse::FAnalysisSnapshot::FClass>& Pair : Snapshot->Classes)
		{
			if (!Pair.Value.bInPublishedProgram)
			{
				continue;
			}
			if (Verse::VClass* Class = FindClassDecorated(DecorateScriptClass(Pair.Key)))
			{
				Entries.Add({0, Pair.Key, Class});
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("the sidecar's class `%s` is not in the program"), *FString(Pair.Key)));
				++NumRefusals;
			}
		}
	}
	const TPair<uint8, FUtf8StringView> Prefixes[] = {
		{1, UTF8TEXT("(/Godot.org/Godot:)")},
		{2, UTF8TEXT("(/Godot.org/Bindings:)")},
	};
	for (Verse::VPackage* Package : Written)
	{
		for (uint32 Definition = 0; Definition < Package->NumDefinitions(); ++Definition)
		{
			const FUtf8StringView Path = Package->GetDefinitionName(Definition).AsStringView();
			for (const TPair<uint8, FUtf8StringView>& Prefix : Prefixes)
			{
				if (!Path.StartsWith(Prefix.Value))
				{
					continue;
				}
				const FUtf8StringView Leaf = Path.RightChop(Prefix.Value.Len());
				int32 Unused;
				if (Leaf.FindChar(UTF8CHAR(':'), Unused) || Leaf.FindChar(UTF8CHAR('('), Unused))
				{
					continue;
				}
				const Verse::VValue Value = Package->GetDefinition(Definition);
				if (Verse::VClass* Class = Value.IsCell() ? Value.AsCell().DynamicCast<Verse::VClass>() : nullptr)
				{
					Entries.Add({Prefix.Key, FUtf8String(Leaf), Class});
				}
			}
		}
	}
	TArray<uint8> IndexBytes;
	FSink Index{IndexBytes};
	Index.Uv(Entries.Num());
	for (const FIndexEntry& Entry : Entries)
	{
		Index.U8(Entry.Origin);
		WriteSid(Index, Entry.Name);
		WriteRef(Index, Entry.Class);
	}

	for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
	{
		EncodeCell(Slot);
	}
	Current = INDEX_NONE;

	FSink File{OutFile};
	OutFile.Append(reinterpret_cast<const uint8*>("VBC1"), 4);
	File.Uv(FormatVersion);
	File.Uv(VH_ABI_VERSION);
	File.Str(UTF8TEXT(VH_BUILD_HOST_ID));
	File.Str(UTF8TEXT(VH_BUILD_ENGINE_COMMIT));
	File.Str(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(GodotVerse::Vbc::OpSchemaDigest)));
	File.Uv(static_cast<uint64>(FMath::Max(Generation, 0)));
	File.Uv(Strings.Num());
	for (const FUtf8String& Text : Strings)
	{
		File.Str(Text);
	}
	File.Uv(Slots.Num());
	for (const FSlot& Slot : Slots)
	{
		OutFile.Append(Slot.Bytes);
	}
	OutFile.Append(PackagesBytes);
	OutFile.Append(WellKnownBytes);
	OutFile.Append(IndexBytes);
	File.U8(EndMarker);

	int64 TotalOps = 0;
	int64 TotalMayPark = 0;
	int64 TotalOnRegister = 0;
	for (const FParkRow& Row : ParkRows)
	{
		TotalOps += Row.Ops;
		TotalMayPark += Row.MayPark;
		TotalOnRegister += Row.MayParkOnRegister;
	}
	Algo::StableSortBy(ParkRows, [](const FParkRow& Row) { return -Row.MayPark; });
	int32 FlaggedClasses = 0;
	int32 AllClasses = 0;
	for (const TPair<FString, TPair<int32, int32>>& Package : ClassFlagTally)
	{
		FlaggedClasses += Package.Value.Key;
		AllClasses += Package.Value.Value;
	}
	OutReport = FString::Printf(
		TEXT("summary: %d procedures, %lld ops, %lld may_park ops, %lld of them read a register operand ")
		TEXT("(the rest read only constants, which are never unbound); %d NewClass ops; ")
		TEXT("%d of %d classes carry EmulateCaseInsensitiveOverrides (4096)\n"),
		ParkRows.Num(), TotalOps, TotalMayPark, TotalOnRegister, NumNewClassOps, FlaggedClasses, AllClasses);
	OutReport += TEXT("classes with flag 4096, per package:\n");
	for (const TPair<FString, TPair<int32, int32>>& Package : ClassFlagTally)
	{
		OutReport += FString::Printf(TEXT("  %s: %d of %d\n"), *Package.Key, Package.Value.Key, Package.Value.Value);
	}
	OutReport += FString::Printf(TEXT("native procedures reached that no definitions table holds: %d\n"), UnkeyedNatives.Num());
	for (const FString& Native : UnkeyedNatives)
	{
		OutReport += TEXT("  ") + Native + TEXT("\n");
	}
	OutReport += TEXT("may_park\tmay_park_on_register\tops\tcell\tprocedure\tfile\n");
	for (const FParkRow& Row : ParkRows)
	{
		OutReport += FString::Printf(TEXT("%d\t%d\t%d\t%d\t%s\t%s\n"), Row.MayPark, Row.MayParkOnRegister, Row.Ops, Row.Cell,
		                             *FString(Row.Name), *FString(Row.File));
	}
	return NumRefusals == 0;
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::WriteProgramVbc(const FString& OutDir, int32 Generation, FString& OutSummary, FUtf8String& OutError)
{
	if (!Verse::GlobalProgram)
	{
		OutError = UTF8TEXT("there is no Verse program to write; nothing was compiled");
		return false;
	}

	// Inside the VM because reading an engine object's property builds Verse values. The writer runs
	// no Verse, so none of EnterVerse's content-scope handling applies.
	TArray<uint8> File;
	FString Report;
	bool bWritten = false;
	int32 NumProcedures = 0;
	int32 NumNewClassOps = 0;
	int32 NumUnkeyedNatives = 0;
	int32 NumRefusals = 0;
	TArray<FString> Errors;
	Verse::FRunningContext Context = Verse::FRunningContextPromise{};
	Context.EnterVM([&] {
		FVbcWriter Writer(Context);
		bWritten = Writer.Run(Generation, File, Report);
		NumProcedures = Writer.NumProcedures;
		NumNewClassOps = Writer.NumNewClassOps;
		NumUnkeyedNatives = Writer.UnkeyedNatives.Num();
		NumRefusals = Writer.NumRefusals;
		Errors = MoveTemp(Writer.Errors);
	});

	if (!bWritten)
	{
		FString Message = FString::Printf(TEXT("program.vbc was not written: %d thing(s) in the program have no encoding in format version 1"),
		                                  NumRefusals);
		for (const FString& Error : Errors)
		{
			Message += TEXT("\n  ") + Error;
		}
		if (NumRefusals > Errors.Num())
		{
			Message += FString::Printf(TEXT("\n  ... and %d more"), NumRefusals - Errors.Num());
		}
		OutError = FUtf8String(Message);
		return false;
	}

	const FString Path = FPaths::Combine(OutDir, TEXT("program.vbc"));
	IFileManager::Get().MakeDirectory(*OutDir, /*Tree*/ true);
	if (!FFileHelper::SaveArrayToFile(File, *Path))
	{
		OutError = FUtf8String(FString::Printf(TEXT("could not write %s"), *Path));
		return false;
	}
	const FString ReportPath = FPaths::Combine(OutDir, TEXT("program.vbc.report.txt"));
	if (!FFileHelper::SaveStringToFile(Report, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FUtf8String(FString::Printf(TEXT("could not write %s"), *ReportPath));
		return false;
	}

	OutSummary = FString::Printf(TEXT("program.vbc: %d bytes, %d procedures, %d NewClass ops, %d native procedure(s) with no binding key"),
	                             File.Num(), NumProcedures, NumNewClassOps, NumUnkeyedNatives);
	return true;
}

#endif // VH_HOST_KIND == VH_HOST_KIND_COOKER
