// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostMarshal.h"

// Before the generated layout, which names the ABI's variant tags without including it.
#include "HostScript.h"
#include "GodotClasses.h"
#include "GodotMathLayout.gen.h"
#include "HostRuntime.h"
#include "HostScriptState.h"
#include "UObject/Package.h"
#include "VerseVM/Inline/VVMEnumerationInline.h"
#include "VerseVM/Inline/VVMRefInline.h"
#include "VerseVM/Inline/VVMValueInline.h"
#include "VerseVM/Inline/VVMValueObjectInline.h"
#include "VerseVM/Inline/VVMVerseClassInline.h"
#include "VerseVM/VVMArray.h"
#include "VerseVM/VVMClass.h"
#include "VerseVM/VVMContext.h"
#include "VerseVM/VVMEnumerator.h"
#include "VerseVM/VVMFalse.h"
#include "VerseVM/VVMFloat.h"
#include "VerseVM/VVMGlobalProgram.h"
#include "VerseVM/VVMInt.h"
#include "VerseVM/VVMMutableArray.h"
#include "VerseVM/VVMNamedType.h"
#include "VerseVM/VVMNativeConverter.h"
#include "VerseVM/VVMNativeStruct.h"
#include "VerseVM/VVMOpResult.h"
#include "VerseVM/VVMOption.h"
#include "VerseVM/VVMPackage.h"
#include "VerseVM/VVMProgram.h"
#include "VerseVM/VVMUniqueString.h"
#include "VerseVM/VVMValueObject.h"
#include "VerseVM/VVMVerseClass.h"

namespace GodotVerse {

namespace {

/// The decorated key `rid`'s one field is stored under, the way ReadStructComponents builds one.
/// Named once because it is read in one direction and written in the other.
AUTORTFM_DISABLE FUtf8String RidFieldKey()
{
    return FUtf8String(UTF8TEXT("(")) + GodotVersePath + UTF8TEXT("/rid:)Id");
}

/// The decorated shape key of one of a mirrored struct's fields -- `(/Godot.org/Godot/vector2:)X`.
AUTORTFM_DISABLE FUtf8String StructFieldKey(const FStructLayout& Layout, const char* Field)
{
    return FUtf8String(UTF8TEXT("(")) + GodotVersePath + UTF8TEXT("/") + Layout.verse_name
        + UTF8TEXT(":)") + Field;
}

/// How many scalars a struct occupies on the wire, counting through its nested fields.
///
/// Not the field count: a transform3d has two fields and twelve components, and it is components
/// that cross.
AUTORTFM_DISABLE int32 StructFieldCount(const FStructLayout& Layout)
{
    int32 Count = 0;
    for (int32 Index = 0; Index < Layout.field_count; ++Index)
    {
        const FStructField& Field = Layout.fields[Index];
        if (Field.nested_tag == 0)
        {
            ++Count;
        }
        else if (const FStructLayout* Nested = FindStructLayoutByTag(Field.nested_tag))
        {
            Count += StructFieldCount(*Nested);
        }
    }
    return Count;
}

/// Appends a struct's scalar components to OutItems, walking into nested fields.
///
/// Recursive because Godot's math types are: a transform3d is a basis and a vector3, and the basis
/// is three vector3s. The wire carries the leaves in this order and nothing else, so the walk here
/// and the packer gen_verse_api.py emits have to agree -- which is why both come from one layout.
AUTORTFM_DISABLE TResult<void> ReadStructComponents(Verse::FRunningContext Context,
                                                    Verse::VValueObject& Struct,
                                                    const FStructLayout& Layout,
                                                    TArray<vh_value>& OutItems)
{
    for (int32 Index = 0; Index < Layout.field_count; ++Index)
    {
        const FStructField& Field = Layout.fields[Index];
        Verse::VUniqueString& Key = Verse::VUniqueString::New(Context, FUtf8StringView(StructFieldKey(Layout, Field.name)));
        const Verse::FOpResult Read = Struct.LoadField(Context, Key);
        if (!Read.IsReturn())
        {
            return EHostFailure::MissingField;
        }

        if (Field.nested_tag != 0)
        {
            const FStructLayout* const Nested = FindStructLayoutByTag(Field.nested_tag);
            if (!Nested)
            {
                return EHostFailure::Unconvertible;
            }
            Verse::VValueObject* const Inner = Read.Value.DynamicCast<Verse::VValueObject>();
            if (!Inner)
            {
                return EHostFailure::TypeMismatch;
            }
            const TResult<void> Nest = ReadStructComponents(Context, *Inner, *Nested, OutItems);
            if (!Nest)
            {
                return Nest;
            }
            continue;
        }

        vh_value Item{};
        if (Field.is_int)
        {
            if (!Read.Value.IsInt())
            {
                return EHostFailure::TypeMismatch;
            }
            Item.Type = VH_TYPE_INT;
            Item.Int = Read.Value.AsInt().AsInt64();
        }
        else
        {
            if (!Read.Value.IsFloat())
            {
                return EHostFailure::TypeMismatch;
            }
            Item.Type = VH_TYPE_FLOAT;
            Item.Float = Read.Value.AsFloat().AsDouble();
        }
        OutItems.Add(Item);
    }
    return TResult<void>::Ok();
}

/// Reads Layout's fields off a struct value into a fresh block of OutStorage, in Layout's order, and
/// points OutValue at it. Fails if any field is missing or is not a number, which would otherwise
/// hand the consumer a tuple it cannot rebuild.
AUTORTFM_DISABLE TResult<void> ReadStructValue(Verse::FRunningContext Context,
                                               Verse::VValueObject& Struct,
                                               const FStructLayout& Layout,
                                               GodotVerse::FFieldStorage& OutStorage,
                                               vh_value& OutValue)
{
    const int32 BlockIndex = OutStorage.Blocks.AddDefaulted();
    OutStorage.Blocks[BlockIndex].Reserve(StructFieldCount(Layout));
    const TResult<void> Read = ReadStructComponents(Context, Struct, Layout, OutStorage.Blocks[BlockIndex]);
    if (!Read)
    {
        return Read;
    }

    OutValue.Type = VH_TYPE_TUPLE;
    OutValue.VariantTag = Layout.variant_tag;
    OutValue.Seq.Items = OutStorage.Blocks[BlockIndex].GetData();
    OutValue.Seq.Count = OutStorage.Blocks[BlockIndex].Num();
    return TResult<void>::Ok();
}

AUTORTFM_DISABLE TResult<void> ReadArrayValue(Verse::FRunningContext Context,
                                              const Verse::VArrayBase& Array,
                                              int32 Tag,
                                              int32 ElementTag,
                                              GodotVerse::FFieldStorage& OutStorage,
                                              vh_value& OutValue)
{
    const int32 Count = (int32)Array.Num();
    const FStructLayout* const Layout = FindStructLayoutByPackedTag(Tag);

    // Reserved for the array's own block plus one per struct element, so that filling it never moves
    // a block an element's vh_value already points into.
    OutStorage.Blocks.Reserve(OutStorage.Blocks.Num() + 1 + (Layout ? Count : 0));
    OutStorage.Strings.Reserve(Tag == VH_VARIANT_PACKED_STRING_ARRAY ? Count : 0);

    const int32 BlockIndex = OutStorage.Blocks.AddDefaulted();
    OutStorage.Blocks[BlockIndex].Reserve(Count);

    for (int32 Index = 0; Index < Count; ++Index)
    {
        const Verse::VValue Element = Array.GetValue((uint32)Index);
        vh_value Item{};

        if (Layout)
        {
            Verse::VValueObject* const Struct = Element.DynamicCast<Verse::VValueObject>();
            if (!Struct)
            {
                return EHostFailure::TypeMismatch;
            }
            const TResult<void> Read = ReadStructValue(Context, *Struct, *Layout, OutStorage, Item);
            if (!Read)
            {
                return Read;
            }
        }
        else if (Tag == VH_VARIANT_PACKED_INT64_ARRAY)
        {
            if (!Element.IsInt())
            {
                return EHostFailure::TypeMismatch;
            }
            Item.Type = VH_TYPE_INT;
            Item.Int = Element.AsInt().AsInt64();
        }
        else if (Tag == VH_VARIANT_PACKED_FLOAT64_ARRAY)
        {
            if (!Element.IsFloat())
            {
                return EHostFailure::TypeMismatch;
            }
            Item.Type = VH_TYPE_FLOAT;
            Item.Float = Element.AsFloat().AsDouble();
        }
        else if (Tag == VH_VARIANT_PACKED_STRING_ARRAY)
        {
            const Verse::VArrayBase* const Text = Element.DynamicCast<Verse::VArrayBase>();
            if (!Text)
            {
                return EHostFailure::TypeMismatch;
            }
            const int32 StringIndex = OutStorage.Strings.Add(FUtf8String(Text->AsStringView()));
            Item.Type = VH_TYPE_STRING;
            Item.String.Utf8 = reinterpret_cast<const char*>(*OutStorage.Strings[StringIndex]);
            Item.String.Len = OutStorage.Strings[StringIndex].Len();
        }
        else if (Tag == VH_VARIANT_ARRAY && ElementTag == VH_VARIANT_BOOL)
        {
            if (!Element.IsLogic())
            {
                return EHostFailure::TypeMismatch;
            }
            Item.Type = VH_TYPE_LOGIC;
            Item.Logic = Element.AsBool() ? 1 : 0;
        }
        else
        {
            return EHostFailure::Unconvertible;
        }

        OutStorage.Blocks[BlockIndex].Add(Item);
    }

    OutValue.Type = VH_TYPE_ARRAY;
    OutValue.VariantTag = Tag;
    OutValue.Seq.Items = OutStorage.Blocks[BlockIndex].GetData();
    OutValue.Seq.Count = Count;
    return TResult<void>::Ok();
}

/// A fresh struct value of Class, with Layout's fields taken from Items.
///
/// The archetype is built from Layout's fields rather than taken from the class, and that is the
/// whole of the difficulty here. A field the class declares with an initializer -- which every field
/// of `vector2` has -- is *raised to the shape* as a `Constant`, shared by every instance, and a
/// constant has no per-instance slot to write: `VObject::SetField` reaches `VERSE_UNREACHABLE` on one
/// (`Inline/VVMObjectInline.h:73`). An archetype of `ObjectField` entries is what asks for the slots
/// instead, and `CreateField` is what marks each as present before it is written. This is the same
/// sequence `VNativeRef::FromNativeStruct` uses to hand a native struct to ordinary Verse code
/// (`VVMNativeRef.cpp:492-513`).
///
/// `NewVObject` rather than a lower-level allocation because it is what marks a struct deeply mutable
/// (`VVMClass.cpp:333-336`); an object built any other way does not compare or freeze like one.

/// Builds one mirrored struct from Count of the components at Items, consuming them in order.
///
/// The cursor is threaded through rather than indexed from zero per field, because a nested field
/// takes as many components as its own layout says -- a basis takes nine of a transform3d's twelve
/// and the origin takes the rest.
AUTORTFM_DISABLE TResult<Verse::VValue> NewStructFrom(Verse::FRunningContext Context,
                                                      const FStructLayout& Layout,
                                                      const vh_value* Items,
                                                      int32 ItemCount,
                                                      int32& Cursor)
{
    Verse::VClass* const Class =
        FindMirroredVClass(Context, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Layout.verse_name)));
    if (!Class)
    {
        return EHostFailure::NotPublished;
    }

    TArray<Verse::VUniqueString*> Keys;
    TArray<Verse::VArchetype::VEntry> Entries;
    Keys.Reserve(Layout.field_count);
    Entries.Reserve(Layout.field_count);
    for (int32 Index = 0; Index < Layout.field_count; ++Index)
    {
        Verse::VUniqueString& Key =
            Verse::VUniqueString::New(Context, FUtf8StringView(StructFieldKey(Layout, Layout.fields[Index].name)));
        Keys.Add(&Key);
        Entries.Add(Verse::VArchetype::VEntry::ObjectField(Context, Key));
    }

    Verse::VArchetype& Archetype = Verse::VArchetype::New(Context, Verse::VValue(), Entries);
    Verse::VValueObject& Struct = Class->NewVObject(Context, Archetype);

    for (int32 Index = 0; Index < Layout.field_count; ++Index)
    {
        const FStructField& Field = Layout.fields[Index];
        Verse::VValue FieldValue;

        if (Field.nested_tag != 0)
        {
            const FStructLayout* const Nested = FindStructLayoutByTag(Field.nested_tag);
            if (!Nested)
            {
                return EHostFailure::Unconvertible;
            }
            const TResult<Verse::VValue> Built = NewStructFrom(Context, *Nested, Items, ItemCount, Cursor);
            if (!Built)
            {
                return Built;
            }
            FieldValue = Built.GetValue();
        }
        else
        {
            if (Cursor >= ItemCount)
            {
                return EHostFailure::TypeMismatch;
            }
            const vh_value& Item = Items[Cursor++];
            const double Number = Item.Type == VH_TYPE_FLOAT
                ? Item.Float
                : (Item.Type == VH_TYPE_INT ? (double)Item.Int : 0.0);
            FieldValue = Field.is_int
                ? Verse::VValue(Verse::VInt(Context, (int64)Number))
                : Verse::VValue(Verse::VFloat(Number));
        }

        if (!Struct.CreateField(Context, *Keys[Index])
            || !Struct.SetField(Context, *Keys[Index], FieldValue).IsReturn())
        {
            return EHostFailure::ConstructionFailed;
        }
    }
    return Verse::VValue(Struct);
}

AUTORTFM_DISABLE int64 HandleOf(Verse::VValue Value)
{
    UObject* Wrapper = Value.ExtractUObject();
    verse::vh_object* Shadow = Wrapper ? Cast<verse::vh_object>(Wrapper) : nullptr;
    return Shadow ? Shadow->Handle.Get() : 0;
}

/// The VM's class for an already-decorated name, which is FindMirroredVClass without the assumption
/// that the name is Godot's.
///
/// A struct a *project* declares lives at its own verse path -- `(/user@localhost:)strike_report`,
/// or `(/user@localhost/gameplay:)strike_report` inside a module -- so the mirrored spelling cannot
/// reach it. The verse path rather than the package name is what goes in the decoration: a
/// generation's package is named afresh on every publish while its verse path stays pinned, which is
/// OQ-12's answer and the reason the statics reader looks definitions up this way too.
AUTORTFM_DISABLE Verse::VClass* FindVClassByDecoratedName(FUtf8StringView DecoratedName)
{
    if (!Verse::GlobalProgram || DecoratedName.IsEmpty())
    {
        return nullptr;
    }
    for (uint32 Index = 0; Index < Verse::GlobalProgram->NumPackages(); ++Index)
    {
        if (Verse::VClass* Class = Verse::GlobalProgram->GetPackage(Index).LookupDefinition<Verse::VClass>(DecoratedName))
        {
            return Class;
        }
    }
    return nullptr;
}

/// The VM's enumeration of that decorated name, looked up the way FindMirroredVClass looks up a
/// class: by walking the published packages, since nothing indexes them together.
AUTORTFM_DISABLE Verse::VEnumeration* FindVEnumeration(FUtf8StringView DecoratedName)
{
    if (!Verse::GlobalProgram || DecoratedName.IsEmpty())
    {
        return nullptr;
    }
    for (uint32 Index = 0; Index < Verse::GlobalProgram->NumPackages(); ++Index)
    {
        if (Verse::VEnumeration* Enumeration =
                Verse::GlobalProgram->GetPackage(Index).LookupDefinition<Verse::VEnumeration>(DecoratedName))
        {
            return Enumeration;
        }
    }
    return nullptr;
}

/// The UClass a declared reference type names, wherever the class was declared.
///
/// Three packages can declare one and each answers to a different lookup: the mirror's own, the
/// project's (which is what `@global_class` registers with Godot), and the generated bindings'.
/// Null for a class none of them carries, which a nested class is and so is one left behind by a
/// retired generation.
AUTORTFM_DISABLE UClass* DeclaredReferenceClass(const FMemberType& Declared)
{
    VH_EXHAUSTIVE_SWITCH_BEGIN
    switch (Declared.ReferenceOrigin)
    {
    case EClassOrigin::Mirrored:
        return FindMirroredClass(FUtf8StringView(Declared.ReferenceName));
    case EClassOrigin::Script:
        return FindGodotClass(FUtf8StringView(Declared.ReferenceQualifiedName));
    case EClassOrigin::Binding:
        return FindBindingClass(FUtf8StringView(Declared.ReferenceName));
    // Also what a binding reads back out of a sidecar as, which records it as Other.
    case EClassOrigin::Other:
        return FindBindingClass(FUtf8StringView(Declared.ReferenceName));
    }
    VH_EXHAUSTIVE_SWITCH_END
    return nullptr;
}

/// Godot's RID from a `rid` value, which names its own class exactly as a math struct does.
///
/// It is not in the layout table and must not be: a math struct crosses as a *component array*
/// under its own variant tag, and a RID crosses as a **scalar** -- VH_TYPE_INT with
/// VH_VARIANT_RID, the number in `Int`. Same discovery, different encoding, so it is its own arm
/// rather than a layout row. Putting it in the table would change what VH_VARIANT_RID means on the
/// wire, which is an ABI major for nothing Godot wants.
AUTORTFM_DISABLE TResult<void> ReadRidStruct(Verse::FRunningContext Context,
                                             Verse::VValue Value,
                                             vh_value& OutValue)
{
    Verse::VValueObject* const Struct = Value.DynamicCast<Verse::VValueObject>();
    if (!Struct || !Struct->GetClass().GetBaseName().AsStringView().Equals(
                       FUtf8StringView(UTF8TEXT("rid"))))
    {
        return EHostFailure::TypeMismatch;
    }

    // The decorated key, the way ReadStructComponents builds one -- a field is stored under
    // `(/Godot.org/Godot/rid:)Id`, so a namesake struct of the author's own has none of them and
    // declines here rather than being read as a RID.
    const FUtf8String KeyText = RidFieldKey();
    Verse::VUniqueString& Key = Verse::VUniqueString::New(Context, FUtf8StringView(KeyText));
    const Verse::FOpResult Read = Struct->LoadField(Context, Key);
    if (!Read.IsReturn())
    {
        return EHostFailure::MissingField;
    }
    if (!Read.Value.IsInt())
    {
        return EHostFailure::TypeMismatch;
    }

    OutValue.Type = VH_TYPE_INT;
    OutValue.VariantTag = VH_VARIANT_RID;
    OutValue.Int = Read.Value.AsInt().AsInt64();
    return TResult<void>::Ok();
}

/// The `rid` a RID arriving from Godot becomes.
///
/// NewVObject rather than a lower-level allocation, for the reason WireToValue's user-struct arm
/// gives: it is what marks a struct deeply mutable, and one built any other way does not compare
/// or freeze like a struct.
AUTORTFM_DISABLE TResult<Verse::VValue> NewRidValue(Verse::FRunningContext Context, int64 Id)
{
    Verse::VClass* const StructClass =
        FindMirroredVClass(Context, FUtf8StringView(UTF8TEXT("rid")));
    if (!StructClass)
    {
        return EHostFailure::NotPublished;
    }

    const FUtf8String KeyText = RidFieldKey();
    Verse::VUniqueString& Key = Verse::VUniqueString::New(Context, FUtf8StringView(KeyText));

    TArray<Verse::VArchetype::VEntry> Entries;
    Entries.Add(Verse::VArchetype::VEntry::ObjectField(Context, Key));
    Verse::VArchetype& Archetype = Verse::VArchetype::New(Context, Verse::VValue(), Entries);
    Verse::VValueObject& Struct = StructClass->NewVObject(Context, Archetype);

    if (!Struct.CreateField(Context, Key)
        || !Struct.SetField(Context, Key, Verse::VValue(Verse::VInt(Context, Id))).IsReturn())
    {
        return EHostFailure::ConstructionFailed;
    }
    return Verse::VValue(Struct);
}

/// The kind a Verse value says it is, for a caller that has no declaration to ask. Only the kinds a
/// value can name itself as are answered; everything else is Other.
///
/// **The order is load-bearing and not obvious.** `true` is an option around `false`, so a cell
/// holding a Godot object reads as a logic if it is asked before the object test; and a string is a
/// `VArrayBase` of Char8/Char32, so it has to be settled before anything that treats an array as an
/// array. A math struct and a `rid` are told apart by the class the value names.
AUTORTFM_DISABLE EDeclaredKind SelfDescribedKind(Verse::VValue Value)
{
    if (UObject* const Wrapper = Value.ExtractUObject())
    {
        if (Cast<verse::vh_object>(Wrapper))
        {
            return EDeclaredKind::Reference;
        }
    }
    if (Value.IsInt())
    {
        return EDeclaredKind::Int;
    }
    if (Value.IsFloat())
    {
        return EDeclaredKind::Float;
    }
    if (const Verse::VArrayBase* const Array = Value.DynamicCast<Verse::VArrayBase>())
    {
        const Verse::EArrayType ArrayType = Array->GetArrayType();
        if (ArrayType == Verse::EArrayType::Char8 || ArrayType == Verse::EArrayType::Char32)
        {
            return EDeclaredKind::String;
        }
    }
    if (Verse::VValueObject* const Struct = Value.DynamicCast<Verse::VValueObject>())
    {
        const FUtf8StringView Name = Struct->GetClass().GetBaseName().AsStringView();
        if (FindStructLayout(Name))
        {
            return EDeclaredKind::MathStruct;
        }
        if (Name.Equals(FUtf8StringView(UTF8TEXT("rid"))))
        {
            return EDeclaredKind::Rid;
        }
    }
    if (Value.IsLogic())
    {
        return EDeclaredKind::Logic;
    }
    return EDeclaredKind::Other;
}

} // namespace

AUTORTFM_DISABLE Verse::VClass* FindMirroredVClass(Verse::FRunningContext Context, FUtf8StringView ClassName)
{
    if (!Verse::GlobalProgram)
    {
        return nullptr;
    }
    const FUtf8String Decorated = FUtf8String(UTF8TEXT("(")) + GodotVersePath + UTF8TEXT(":)") + FUtf8String(ClassName);
    for (uint32 Index = 0; Index < Verse::GlobalProgram->NumPackages(); ++Index)
    {
        if (Verse::VClass* Class = Verse::GlobalProgram->GetPackage(Index).LookupDefinition<Verse::VClass>(FUtf8StringView(Decorated)))
        {
            return Class;
        }
    }
    return nullptr;
}

AUTORTFM_DISABLE TResult<Verse::VValue> NewStructValue(Verse::FRunningContext Context,
                                                       Verse::VClass& Class,
                                                       const FStructLayout& Layout,
                                                       const vh_value* Items,
                                                       int32 ItemCount)
{
    if (ItemCount != StructFieldCount(Layout))
    {
        return EHostFailure::TypeMismatch;
    }
    int32 Cursor = 0;
    return NewStructFrom(Context, Layout, Items, ItemCount, Cursor);
}

AUTORTFM_DISABLE TResult<void> ValueToWire(Verse::FRunningContext Context,
                                           Verse::VValue Value,
                                           const FMemberType& Declared,
                                           GodotVerse::FFieldStorage& OutStorage,
                                           vh_value& OutValue)
{
    VH_EXHAUSTIVE_SWITCH_BEGIN
    switch (Declared.Kind)
    {
    // `variant` first, because none of the tests below would recognise one: it is a VNativeStruct
    // boxing the 22 lanes, which is neither an option, nor a logic, nor a VValueObject. The
    // declaration is the only thing that says so, which is the general rule this function is built
    // on arriving at its widest case.
    case EDeclaredKind::Variant:
    {
        // DynamicCast before FNativeConverter, whose own FromVValue is a StaticCast: the declared
        // type says what this should be and a value that is not one must decline rather than
        // reinterpret whatever cell it found.
        if (!Value.DynamicCast<Verse::VNativeStruct>())
        {
            return EHostFailure::TypeMismatch;
        }
        Verse::TFromVValue<verse::variant> Boxed{};
        if (!Verse::FNativeConverter::FromVValue(Context, Value, Boxed).IsReturn())
        {
            return EHostFailure::Unconvertible;
        }
        OutStorage.Blocks.Reserve(OutStorage.Blocks.Num() + 1);
        OutValue = GodotVerse::VariantToWire(Boxed.GetValue(), OutStorage.Text,
                                             OutStorage.Blocks.AddDefaulted_GetRef());
        return TResult<void>::Ok();
    }

    // A `rid` is a struct whose description says VH_TYPE_INT, so it has to be unwrapped here:
    // nothing below recognises it, and the plain int arm would find a VValueObject where it wants
    // an int. Same discovery `MakeVariant` uses -- one ReadRidStruct, not two that can disagree.
    case EDeclaredKind::Rid:
        return ReadRidStruct(Context, Value, OutValue);

    // Everything else is read off the value, going back to the declaration only where the value
    // cannot say -- an empty option, an empty array, a struct's field order.
    case EDeclaredKind::Reference:
    case EDeclaredKind::OtherClass:
    case EDeclaredKind::Option:
    case EDeclaredKind::Container:
    case EDeclaredKind::TypedContainer:
    case EDeclaredKind::MathStruct:
    case EDeclaredKind::UserStruct:
    case EDeclaredKind::Other:
    case EDeclaredKind::Logic:
    case EDeclaredKind::Int:
    case EDeclaredKind::Float:
    case EDeclaredKind::Char:
    case EDeclaredKind::String:
    case EDeclaredKind::Enum:
    case EDeclaredKind::Array:
    case EDeclaredKind::Map:
    case EDeclaredKind::Tuple:
        break;
    }
    VH_EXHAUSTIVE_SWITCH_END

    // A reference, before the logic test rather than after it, because Verse's two spellings
    // collide: `true` is an option around `false`, and an empty option *is* `false`. A set
    // option wrapping a wrapper object is the one of the three the value alone identifies; an
    // empty one has to be told what the author declared, and anything else falls through to
    // the logic the cell equally well is.
    int64 ReferenceHandle = 0;
    bool bIsReference = false;
    if (const Verse::VOption* Option = Value.DynamicCast<Verse::VOption>())
    {
        ReferenceHandle = HandleOf(Option->GetValue());
        bIsReference = ReferenceHandle != 0;
    }
    else if (Value.IsFalse())
    {
        bIsReference = !Declared.ReferenceName.IsEmpty();
    }
    else if (Declared.Described.VariantTag == VH_VARIANT_OBJECT)
    {
        // A *bare* object, which a member never is -- an exported reference must be optional,
        // because the inspector can leave a slot empty -- but a signal payload and a method
        // parameter both are. Without this a `signal(node2d)` emitted nothing and said the
        // payload had no representation, which is true of no object at all.
        ReferenceHandle = HandleOf(Value);
        bIsReference = ReferenceHandle != 0;
    }

    if (bIsReference)
    {
        // A handle the consumer rebuilds an object from, and for the empty case the empty
        // option -- which is a reference holding nothing, not a member that failed to read.
        OutValue.VariantTag = VH_VARIANT_OBJECT;
        if (ReferenceHandle != 0)
        {
            OutValue.Type = VH_TYPE_INT;
            OutValue.Int = ReferenceHandle;
        }
        else
        {
            OutValue.Type = VH_TYPE_OPTION;
            OutValue.Option = nullptr;
        }
    }
    else if (Value.IsLogic())
    {
        OutValue.Type = VH_TYPE_LOGIC;
        OutValue.Logic = Value.AsBool() ? 1 : 0;
    }
    else if (Value.IsInt())
    {
        OutValue.Type = VH_TYPE_INT;
        OutValue.Int = Value.AsInt().AsInt64();
    }
    else if (Value.IsFloat())
    {
        OutValue.Type = VH_TYPE_FLOAT;
        OutValue.Float = Value.AsFloat().AsDouble();
    }
    else if (const Verse::VEnumerator* Enumerator = Value.DynamicCast<Verse::VEnumerator>())
    {
        // An enum member holds an enumerator, which is a cell and not a number -- so this is the
        // read, and `IsInt` above never sees one. The ordinal is what crosses, because that is
        // what Godot stores; the names reach only the inspector's dropdown, which is also why
        // reordering a Verse enum silently reinterprets every scene already saved.
        OutValue.Type = VH_TYPE_INT;
        OutValue.VariantTag = VH_VARIANT_INT;
        OutValue.Int = Enumerator->GetIntValue();
    }
    else if (Declared.Described.Type == VH_TYPE_REF)
    {
        // A reference wrapper the script is handing back. Its id is what crosses; the table entry
        // it names is still claimed by whatever Verse object holds it.
        const verse::godot_ref* Wrapper = Cast<verse::godot_ref>(Value.ExtractUObject());
        if (!Wrapper)
        {
            return EHostFailure::TypeMismatch;
        }
        OutValue.Type = VH_TYPE_REF;
        OutValue.VariantTag = Declared.Described.VariantTag;
        OutValue.Ref = Wrapper->Ref.Get();
    }
    else if (Verse::VValueObject* Struct = Value.DynamicCast<Verse::VValueObject>())
    {
        // A mirrored struct: vector2, color. The fields are read by name, and the names come
        // from the declared type -- the value carries its field keys but not which order a
        // Godot Vector2 wants them in, and positions are the whole of what crosses.
        if (!Declared.Struct)
        {
            return EHostFailure::Unconvertible;
        }
        return ReadStructValue(Context, *Struct, *Declared.Struct, OutStorage, OutValue);
    }
    else if (const Verse::VArrayBase* Array = Value.DynamicCast<Verse::VArrayBase>())
    {
        // Verse `string` is `[]char`, so a string arrives as an array of char8 -- and so does
        // every other array, which is why the char case is settled first. VArrayBase rather than
        // VArray because a `var` of a container type holds a VMutableArray: the mutability lives
        // in the container itself, not in a reference around it.
        //
        // An *empty* array cannot be told apart this way, since it carries no element type, so
        // that one case goes back to what the author declared.
        const Verse::EArrayType ArrayType = Array->GetArrayType();
        const GodotVerse::FExportDesc& Desc = Declared.Described;
        const bool bIsString = ArrayType == Verse::EArrayType::Char8
            || ArrayType == Verse::EArrayType::Char32
            || Desc.Type == VH_TYPE_STRING;

        if (bIsString)
        {
            OutStorage.Text = FUtf8String(Array->AsStringView());
            OutValue.Type = VH_TYPE_STRING;
            OutValue.String.Utf8 = reinterpret_cast<const char*>(*OutStorage.Text);
            OutValue.String.Len = OutStorage.Text.Len();
        }
        else if (Desc.Type != VH_TYPE_ARRAY)
        {
            return EHostFailure::TypeMismatch;
        }
        else
        {
            return ReadArrayValue(Context, *Array, Desc.VariantTag, Desc.ElementVariantTag, OutStorage, OutValue);
        }
    }
    else
    {
        return EHostFailure::Unconvertible;
    }

    return TResult<void>::Ok();
}

AUTORTFM_DISABLE UClass* FindReferenceClass(int32 VariantTag)
{
    for (const verse_math::reference_type& Reference : verse_math::reference_types)
    {
        if (Reference.variant_tag == VariantTag)
        {
            return FindMirroredClass(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Reference.verse_name)));
        }
    }
    return nullptr;
}

AUTORTFM_DISABLE TResult<UObject*> NewReferenceWrapper(UClass* NativeClass, int64 Id)
{
    if (!NativeClass)
    {
        return EHostFailure::NotPublished;
    }
    UObject* Wrapper = NewObject<UObject>(GetTransientPackage(), NativeClass);
    verse::godot_ref* Shadow = Cast<verse::godot_ref>(Wrapper);
    if (!Shadow)
    {
        return EHostFailure::TypeMismatch;
    }
    Shadow->Ref.Init(Id, Shadow);
    return Wrapper;
}

AUTORTFM_DISABLE Verse::VValue ReferenceOption(Verse::FRunningContext Context, UObject* Referenced)
{
    return Referenced ? Verse::VValue(Verse::VOption::New(Context, Verse::VValue(Referenced)))
                      : Verse::VValue(Verse::GlobalFalse());
}

AUTORTFM_DISABLE TResult<Verse::VValue> NewArrayValue(Verse::FRunningContext Context,
                                                      bool bMutable,
                                                      int32 Tag,
                                                      int32 ElementTag,
                                                      const vh_value* Items,
                                                      int32 ItemCount)
{
    const FStructLayout* const Layout = FindStructLayoutByPackedTag(Tag);
    Verse::VClass* const StructClass =
        Layout ? FindMirroredVClass(Context, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Layout->verse_name))) : nullptr;
    if (Layout && !StructClass)
    {
        return EHostFailure::NotPublished;
    }

    TArray<Verse::VValue> Elements;
    Elements.Reserve(ItemCount);
    for (int32 Index = 0; Index < ItemCount; ++Index)
    {
        const vh_value& Item = Items[Index];
        if (Layout)
        {
            const TResult<Verse::VValue> Element = NewStructValue(Context, *StructClass, *Layout, Item.Seq.Items, Item.Seq.Count);
            if (!Element)
            {
                return Element;
            }
            Elements.Add(Element.GetValue());
        }
        else if (Tag == VH_VARIANT_PACKED_INT64_ARRAY)
        {
            Elements.Add(Verse::VValue(Verse::VInt(Context, Item.Type == VH_TYPE_FLOAT ? (int64)Item.Float : Item.Int)));
        }
        else if (Tag == VH_VARIANT_PACKED_FLOAT64_ARRAY)
        {
            Elements.Add(Verse::VValue(Verse::VFloat(Item.Type == VH_TYPE_INT ? (double)Item.Int : Item.Float)));
        }
        else if (Tag == VH_VARIANT_PACKED_STRING_ARRAY)
        {
            if (Item.Type != VH_TYPE_STRING)
            {
                return EHostFailure::TypeMismatch;
            }
            const FUtf8StringView Utf8(reinterpret_cast<const UTF8CHAR*>(Item.String.Utf8), Item.String.Len);
            // An element's mutability follows its container's, which is not obvious and is load
            // bearing. Reading a `var` container hands out an immutable snapshot, and
            // VMutableArray::FreezeImpl makes one by freezing each element in turn -- so an element
            // has to be freezable. A VArray is not: every VArrayBase constructor sets the
            // deeply-mutable flag (VVMArrayBase.h:288-370) and nothing ever clears it, while VArray
            // has no FreezeImpl, so freezing one is the fatal "VCell subtype 'VArray' without
            // FreezeImpl override" rather than the no-op it looks like it should be.
            Elements.Add(bMutable ? Verse::VValue(Verse::VMutableArray::New(Context, Utf8))
                                  : Verse::VValue(Verse::VArray::New(Context, Utf8)));
        }
        else if (Tag == VH_VARIANT_ARRAY && ElementTag == VH_VARIANT_BOOL)
        {
            Elements.Add(Verse::VValue::FromBool(Item.Type == VH_TYPE_LOGIC ? Item.Logic != 0 : Item.Int != 0));
        }
        else
        {
            return EHostFailure::Unconvertible;
        }
    }

    const auto Init = [&Elements](uint32 Index) { return Elements[(int32)Index]; };
    if (bMutable)
    {
        Verse::VMutableArray& Array =
            Verse::VMutableArray::New(Context, 0, (uint32)ItemCount, Verse::EArrayType::VValue);
        for (const Verse::VValue& Element : Elements)
        {
            Array.AddValue(Context, Element);
        }
        return Verse::VValue(Array);
    }
    return Verse::VValue(Verse::VArray::New(Context, (uint32)ItemCount, Init));
}

AUTORTFM_DISABLE TResult<void> WireToValue(Verse::FRunningContext Context,
                                           const vh_value& Value,
                                           const FMemberType& Declared,
                                           Verse::VValue& OutValue)
{
    const GodotVerse::FExportDesc& Desc = Declared.Described;

    // A reference arrives as a handle naming a Godot object, so the Verse wrapper has to be built
    // here -- and the declared type is the whole of what says which class to build.
    if (!Declared.ReferenceName.IsEmpty())
    {
        const int64 Handle = Value.Type == VH_TYPE_INT ? Value.Int : 0;
        if (Handle == 0)
        {
            // Null, and it is spellable whatever class the parameter names: an empty option needs
            // no class to build. This used to sit behind the origin test below, so `SomeMethod(?mover)`
            // refused Godot's own null with "Cannot convert argument 1 from Nil to Object" -- a
            // sentence about a value that was exactly what the signature asked for.
            //
            // A bare `node2d` still refuses, which reaches the caller as VH_ERR_ARGUMENT rather than
            // as a runtime error: passing null where the signature does not allow it is the caller's
            // mistake.
            if (!Declared.bReferenceIsOption)
            {
                return EHostFailure::NullNotOptional;
            }
            OutValue = ReferenceOption(Context, nullptr);
            return TResult<void>::Ok();
        }
        // The object the handle *is*, not an instance of the class the signature named (R-SCN-6).
        // A parameter declared `node2d` receiving a node that carries a script is handed that
        // script's own object, which is what makes `if (M := mob[Body])` inside the handler work --
        // the whole point of the cast. The declared class is then the *lower* bound, and a handle
        // whose object does not meet it is VH_ERR_ARGUMENT rather than a raise.
        UClass* const DeclaredClass = DeclaredReferenceClass(Declared);
        UObject* const Referenced = GodotVerse::ObjectForHandle(Handle, DeclaredClass);
        if (!DeclaredClass)
        {
            return EHostFailure::NotPublished;
        }
        if (!Referenced || !Referenced->IsA(DeclaredClass))
        {
            return EHostFailure::TypeMismatch;
        }
        // Wrapped only where the declaration asked for an option. Handing a `?node2d` to a parameter
        // declared `node2d` is what made every method on it unreachable: the script had an option
        // where it had written a node, and the first `.GetName()` died inside the interpreter rather
        // than failing to compile.
        OutValue = Declared.bReferenceIsOption ? ReferenceOption(Context, Referenced) : Verse::VValue(Referenced);
        return TResult<void>::Ok();
    }

    // An enum parameter takes its ordinal, bounded by the enum the author declared rather than
    // clamped into it -- an ordinal with no enumerator is a caller that disagrees with the script
    // about the enum, which is worth reporting rather than silently reinterpreting.
    const auto EnumeratorFromWire = [&Value, &Declared, &OutValue]() -> TResult<void> {
        if (Value.Type != VH_TYPE_INT)
        {
            return EHostFailure::TypeMismatch;
        }
        if (Value.Int < 0 || Value.Int >= Declared.EnumeratorCount)
        {
            return EHostFailure::EnumOrdinalOutOfRange;
        }
        Verse::VEnumeration* const Enumeration = FindVEnumeration(FUtf8StringView(Declared.EnumerationName));
        if (!Enumeration)
        {
            return EHostFailure::NotPublished;
        }
        if (Value.Int >= Enumeration->NumEnumerators)
        {
            return EHostFailure::EnumOrdinalOutOfRange;
        }
        OutValue = Verse::VValue(Enumeration->GetEnumeratorChecked((int32)Value.Int));
        return TResult<void>::Ok();
    };

    VH_EXHAUSTIVE_SWITCH_BEGIN
    switch (Declared.Kind)
    {
    // `variant`: any Godot value at all, so nothing about the wire value has to be checked -- the
    // lanes take whatever arrived, including Godot's own null, which is the nil tag. Boxed by
    // FNativeConverter, which is what VNI's generated glue calls for a native struct parameter.
    case EDeclaredKind::Variant:
        OutValue = Verse::FNativeConverter::ToVValue(Context, GodotVerse::VariantFromWire(Value));
        return TResult<void>::Ok();

    // A reference wrapper: the id is the whole of the value, and the Verse object exists to hold
    // it and to release it when collected. Built through the UObject path rather than as a VM cell
    // for exactly that reason -- a cell has no destructor, and an id nobody releases is a leak.
    case EDeclaredKind::Container:
    {
        const int64 Id = Value.Type == VH_TYPE_REF ? Value.Ref : (Value.Type == VH_TYPE_INT ? Value.Int : 0);
        const TResult<UObject*> Wrapper = NewReferenceWrapper(FindReferenceClass(Desc.VariantTag), Id);
        if (!Wrapper)
        {
            return Wrapper.GetFailure();
        }
        OutValue = Verse::VValue(Wrapper.GetValue());
        return TResult<void>::Ok();
    }

    // The mirror image of ValueToWire's arm: a RID arrives as a plain int under its own variant
    // tag, and the `rid` struct it becomes has to be built here, where the scalar arms below would
    // hand the declaration a bare int and typecheck it against a struct.
    case EDeclaredKind::Rid:
    {
        if (Value.Type != VH_TYPE_INT)
        {
            return EHostFailure::TypeMismatch;
        }
        const TResult<Verse::VValue> Built = NewRidValue(Context, Value.Int);
        if (!Built)
        {
            return Built.GetFailure();
        }
        OutValue = Built.GetValue();
        return TResult<void>::Ok();
    }

    // A struct the project declares, arriving as one argument per field. The mirrored math types
    // below take the same tuple lane and a different builder: theirs is a flat run of scalars laid
    // out by a generated table, and this one is a field list read off the semantic program, so its
    // fields go through this very function and can be anything a field can be.
    case EDeclaredKind::UserStruct:
    {
        if (!Declared.UserStruct.IsValid())
        {
            break;
        }
        const FUserStructLayout& Layout = *Declared.UserStruct;
        if (Value.Type != VH_TYPE_TUPLE || Value.Seq.Count != Layout.FieldKeys.Num())
        {
            return EHostFailure::TypeMismatch;
        }
        Verse::VClass* const StructClass = FindVClassByDecoratedName(FUtf8StringView(Layout.DecoratedName));
        if (!StructClass)
        {
            return EHostFailure::NotPublished;
        }

        TArray<Verse::VUniqueString*> Keys;
        TArray<Verse::VArchetype::VEntry> Entries;
        Keys.Reserve(Layout.FieldKeys.Num());
        Entries.Reserve(Layout.FieldKeys.Num());
        for (const FUtf8String& Key : Layout.FieldKeys)
        {
            Verse::VUniqueString& Unique = Verse::VUniqueString::New(Context, FUtf8StringView(Key));
            Keys.Add(&Unique);
            Entries.Add(Verse::VArchetype::VEntry::ObjectField(Context, Unique));
        }

        // NewVObject rather than a lower-level allocation, for the reason NewStructFrom gives: it is
        // what marks a struct deeply mutable, and one built any other way does not compare or freeze
        // like a struct.
        Verse::VArchetype& Archetype = Verse::VArchetype::New(Context, Verse::VValue(), Entries);
        Verse::VValueObject& Struct = StructClass->NewVObject(Context, Archetype);

        for (int32 Index = 0; Index < Layout.FieldKeys.Num(); ++Index)
        {
            Verse::VValue FieldValue;
            const TResult<void> Field = WireToValue(Context, Value.Seq.Items[Index], Layout.FieldTypes[Index], FieldValue);
            if (!Field)
            {
                return Field;
            }
            if (!Struct.CreateField(Context, *Keys[Index])
                || !Struct.SetField(Context, *Keys[Index], FieldValue).IsReturn())
            {
                return EHostFailure::ConstructionFailed;
            }
        }
        OutValue = Verse::VValue(Struct);
        return TResult<void>::Ok();
    }

    case EDeclaredKind::MathStruct:
    {
        if (Declared.Struct == nullptr)
        {
            break;
        }
        Verse::VClass* const StructClass =
            FindMirroredVClass(Context, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Declared.Struct->verse_name)));
        if (!StructClass)
        {
            return EHostFailure::NotPublished;
        }
        if (Value.Type != VH_TYPE_TUPLE)
        {
            return EHostFailure::TypeMismatch;
        }
        const TResult<Verse::VValue> Built = NewStructValue(Context, *StructClass, *Declared.Struct, Value.Seq.Items, Value.Seq.Count);
        if (!Built)
        {
            return Built.GetFailure();
        }
        OutValue = Built.GetValue();
        return TResult<void>::Ok();
    }

    case EDeclaredKind::Array:
    {
        // A Godot Array or packed array arrives as a reference id, because that is what every
        // container is on this wire now. A Verse array is a value, so the contents have to be read
        // out before one can be built -- which is the whole difference between the two, and the
        // reason a script sees `[]float` where Godot has a PackedFloat32Array.
        GodotVerse::FCallArena ContentsArena;
        vh_value Contents{};
        const vh_value* Source = &Value;
        if (Value.Type == VH_TYPE_REF)
        {
            GodotVerse::FHostState& Host = GodotVerse::GetHost();
            if (!Host.Godot.RefContents)
            {
                return EHostFailure::CallbackMissing;
            }
            if (Host.Godot.RefContents(Host.Godot.Ctx, Value.Ref, &ContentsArena, &Contents) != VH_CALL_OK)
            {
                return EHostFailure::CallbackFailed;
            }
            Source = &Contents;
        }
        if (Source->Type != VH_TYPE_ARRAY)
        {
            return EHostFailure::TypeMismatch;
        }
        // Immutable: a parameter is a fresh binding the callee cannot assign through, so there is
        // no `var` container to match the way a member write has to.
        const TResult<Verse::VValue> Built = NewArrayValue(Context, /*bMutable*/ false, Desc.VariantTag, Desc.ElementVariantTag,
                                                           Source->Seq.Items, Source->Seq.Count);
        if (!Built)
        {
            return Built.GetFailure();
        }
        OutValue = Built.GetValue();
        return TResult<void>::Ok();
    }

    // An option around an enum is given the enumerator bare, as it always has been.
    case EDeclaredKind::Enum:
    case EDeclaredKind::Option:
        if (Declared.EnumeratorCount > 0)
        {
            return EnumeratorFromWire();
        }
        break;

    // Reference and OtherClass were answered by the handle test above; the rest are read by the
    // wire value's own type below.
    case EDeclaredKind::Reference:
    case EDeclaredKind::OtherClass:
    case EDeclaredKind::TypedContainer:
    case EDeclaredKind::Other:
    case EDeclaredKind::Logic:
    case EDeclaredKind::Int:
    case EDeclaredKind::Float:
    case EDeclaredKind::Char:
    case EDeclaredKind::String:
    case EDeclaredKind::Map:
    case EDeclaredKind::Tuple:
        break;
    }
    VH_EXHAUSTIVE_SWITCH_END

    switch (Value.Type)
    {
    case VH_TYPE_LOGIC:
        OutValue = Verse::VValue::FromBool(Value.Logic != 0);
        return TResult<void>::Ok();
    case VH_TYPE_INT:
        // A float parameter handed an int is widened rather than refused: Godot spells 0 as an
        // integer Variant whatever the receiving type, so refusing would make `Process(0)`
        // unreachable from GDScript.
        if (Desc.Type == VH_TYPE_FLOAT)
        {
            OutValue = Verse::VValue(Verse::VFloat((double)Value.Int));
        }
        else
        {
            OutValue = Verse::VValue(Verse::VInt(Context, Value.Int));
        }
        return TResult<void>::Ok();
    case VH_TYPE_FLOAT:
        OutValue = Verse::VValue(Verse::VFloat(Value.Float));
        return TResult<void>::Ok();
    case VH_TYPE_STRING:
        OutValue = Verse::VValue(Verse::VArray::New(
            Context, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Value.String.Utf8), Value.String.Len)));
        return TResult<void>::Ok();
    default:
        return EHostFailure::Unconvertible;
    }
}

AUTORTFM_DISABLE TResult<void> ReadMathStruct(Verse::FRunningContext Context,
                                              Verse::VValue Value,
                                              FFieldStorage& OutStorage,
                                              vh_value& OutValue)
{
    Verse::VValueObject* const Struct = Value.DynamicCast<Verse::VValueObject>();
    if (!Struct)
    {
        return EHostFailure::TypeMismatch;
    }
    const FStructLayout* const Layout =
        FindStructLayout(Struct->GetClass().GetBaseName().AsStringView());
    if (!Layout)
    {
        return EHostFailure::TypeMismatch;
    }
    return ReadStructValue(Context, *Struct, *Layout, OutStorage, OutValue);
}

AUTORTFM_DISABLE TResult<void> ReadSelfDescribingValue(Verse::FRunningContext Context,
                                                       Verse::VValue Value,
                                                       FFieldStorage& OutStorage,
                                                       vh_value& OutValue)
{
    VH_EXHAUSTIVE_SWITCH_BEGIN
    switch (SelfDescribedKind(Value))
    {
    case EDeclaredKind::Reference:
        OutValue.Type = VH_TYPE_INT;
        OutValue.VariantTag = VH_VARIANT_OBJECT;
        OutValue.Int = Cast<verse::vh_object>(Value.ExtractUObject())->Handle.Get();
        return TResult<void>::Ok();
    case EDeclaredKind::Int:
        OutValue.Type = VH_TYPE_INT;
        OutValue.VariantTag = VH_VARIANT_INT;
        OutValue.Int = Value.AsInt().AsInt64();
        return TResult<void>::Ok();
    case EDeclaredKind::Float:
        OutValue.Type = VH_TYPE_FLOAT;
        OutValue.VariantTag = VH_VARIANT_FLOAT;
        OutValue.Float = Value.AsFloat().AsDouble();
        return TResult<void>::Ok();
    case EDeclaredKind::String:
        OutStorage.Text = FUtf8String(Value.DynamicCast<Verse::VArrayBase>()->AsStringView());
        OutValue.Type = VH_TYPE_STRING;
        OutValue.VariantTag = VH_VARIANT_STRING;
        OutValue.String.Utf8 = reinterpret_cast<const char*>(*OutStorage.Text);
        OutValue.String.Len = OutStorage.Text.Len();
        return TResult<void>::Ok();
    case EDeclaredKind::MathStruct:
        return ReadMathStruct(Context, Value, OutStorage, OutValue);
    // A `rid` is as self-describing as a `vector2` -- it names its own class -- and only its
    // encoding differs.
    case EDeclaredKind::Rid:
        return ReadRidStruct(Context, Value, OutValue);
    case EDeclaredKind::Logic:
        OutValue.Type = VH_TYPE_LOGIC;
        OutValue.VariantTag = VH_VARIANT_BOOL;
        OutValue.Logic = Value.AsBool() ? 1 : 0;
        return TResult<void>::Ok();
    case EDeclaredKind::Other:
    case EDeclaredKind::Char:
    case EDeclaredKind::Enum:
    case EDeclaredKind::Array:
    case EDeclaredKind::Map:
    case EDeclaredKind::Tuple:
    case EDeclaredKind::Option:
    case EDeclaredKind::Variant:
    case EDeclaredKind::UserStruct:
    case EDeclaredKind::Container:
    case EDeclaredKind::TypedContainer:
    case EDeclaredKind::OtherClass:
        return EHostFailure::Unconvertible;
    }
    VH_EXHAUSTIVE_SWITCH_END
    return EHostFailure::Unconvertible;
}

} // namespace GodotVerse
