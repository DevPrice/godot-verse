// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostInstances.h"

#include "AutoRTFM.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "GodotClasses.h"
#include "HostCallbacks.h"
#include "HostDebug.h"
#include "HostMarshal.h"
#include "HostPeers.h"
#include "HostResult.h"
#include "HostRuntime.h"
#include "HostScript.h"
#include "HostScriptState.h"
#include "HostSignals.h"
#include "HostTypeModel.h"
#include "HostVerseEntry.h"
#include "Templates/Function.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "VerseContentScope.h"
#include "VerseTask.h"
#include "VerseValue.h"
#include "VerseVM/Inline/VVMEnumerationInline.h"
#include "VerseVM/Inline/VVMRefInline.h"
#include "VerseVM/Inline/VVMValueInline.h"
#include "VerseVM/Inline/VVMValueObjectInline.h"
#include "VerseVM/Inline/VVMVerseClassInline.h"
#include "VerseVM/VVMArray.h"
#include "VerseVM/VVMClass.h"
#include "VerseVM/VVMContext.h"
#include "VerseVM/VVMEnumerator.h"
#include "VerseVM/VVMExecutionContext.h"
#include "VerseVM/VVMFalse.h"
#include "VerseVM/VVMFloat.h"
#include "VerseVM/VVMGlobalProgram.h"
#include "VerseVM/VVMInt.h"
#include "VerseVM/VVMMutableArray.h"
#include "VerseVM/VVMNamedType.h"
#include "VerseVM/VVMNativeConverter.h"
#include "VerseVM/VVMNativeFunction.h"
#include "VerseVM/VVMNativeRef.h"
#include "VerseVM/VVMOpResult.h"
#include "VerseVM/VVMOption.h"
#include "VerseVM/VVMPackage.h"
#include "VerseVM/VVMProgram.h"
#include "VerseVM/VVMRef.h"
#include "VerseVM/VVMRestValue.h"
#include "VerseVM/VVMShape.h"
#include "VerseVM/VVMTaskGroup.h"
#include "VerseVM/VVMUniqueString.h"
#include "VerseVM/VVMValueObject.h"
#include "VerseVM/VVMVerseClass.h"
#include "VerseVM/VVMVerseFunction.h"

using GodotVerse::ContentScopeOuter;
using GodotVerse::DescribeMemberType;
using GodotVerse::EClassOrigin;
using GodotVerse::EDeclaredKind;
using GodotVerse::EHostFailure;
using GodotVerse::EnterVerse;
using GodotVerse::FindGodotClass;
using GodotVerse::FindMirroredClass;
using GodotVerse::FMemberType;
using GodotVerse::FStructLayout;
using GodotVerse::FVerseEntry;
using GodotVerse::NewArrayValue;
using GodotVerse::NewStructValue;
using GodotVerse::ReferenceOption;
using GodotVerse::ScriptVersePath;
using GodotVerse::ValueToWire;
using GodotVerse::WireToValue;

struct GodotVerse::FInstance
{
    TStrongObjectPtr<UObject> Object;

    /// The Godot object this instance is bound to. Kept so the identity registry can drop its row
    /// when the instance goes away; it is never reached through as a handle.
    int64 Handle = 0;

    /// This instance's own task scope (R-ASYNC-4). Every entry that runs this object's code pushes
    /// its guard, so a task `spawn`ed from one of its methods lands in *its* task group and nobody
    /// else's -- which is what makes a raise here cancel this node's suspended work and leave the
    /// rest of the project running.
    ///
    /// Terminated and dropped at ReleaseInstance; replaced rather than revived once terminated
    /// (phase-5-design.md D24).
    TSharedPtr<verse::FContentScope> Scope;

    /// Set by the first call into the object, after which a non-var member can no longer be
    /// given a value. See WriteInstanceField.
    bool bSealed = false;

    /// The `@export_signal` event bindings this instance made, so ReleaseInstance can drop them.
    ///
    /// Godot's own connection needs no disconnect -- the Callable is owned by this same node, so it
    /// dies with it -- but the binding row holds a strong pointer to the event and a reference id,
    /// and neither of those has anything else to end it.
    TArray<int64> EventBindings;
};

namespace {
/// This instance's scope, made if it has none and replaced if the last one was terminated.
///
/// Replaced rather than un-terminated: Epic's own ContentScopeRepository hands out a fresh scope,
/// and what a raise costs is exactly this instance's suspended work. The replacement happens here,
/// at the next entry, rather than at the next frame boundary.
AUTORTFM_DISABLE TSharedRef<verse::FContentScope> ScopeFor(GodotVerse::FInstance& Instance)
{
    if (!Instance.Scope.IsValid() || Instance.Scope->WasTerminated())
    {
        Instance.Scope = verse::MakeContentScope(ContentScopeOuter());
    }
    return Instance.Scope.ToSharedRef();
}

/// EnterVerse, with one instance's own task scope active for the duration (R-ASYNC-4).
///
/// The guard is pushed *outside* EnterVM on purpose: FRunningContext::EnterVM_Internal reads the
/// active scope to decide which task group a `spawn` inside the body joins, and it declines to run
/// the body at all when that scope was terminated.
template <typename TBody>
AUTORTFM_DISABLE void EnterVerseOn(Verse::FRunningContext& Context, GodotVerse::FInstance& Instance, TBody&& Body)
{
    FVerseEntry Entry;
    verse::FContentScopeGuard Guard(ScopeFor(Instance));
    Context.EnterVM(Forward<TBody>(Body));
}

} // namespace

namespace {

/// The module-qualified name of a script class: `player` at the package root, `gameplay/player`
/// in a module. This is what every ClassNameUtf8 in the ABI carries.
///
/// Read back out of the UClass's own name, which is the Verse name mangled by
/// VNamedType::AppendMangledName: the module path with each `/` replaced by a separator, then the
/// class's own name. The separator is `-` for a Source package like this one (`_` is for VNI), and
/// no Verse identifier can contain either one, so undoing it is a substitution rather than a parse.
///
/// UVerseClass::PackageRelativeVersePath would be the direct answer and is not usable: the line
/// that sets it from the AST is commented out in VVMClass.cpp, so under VerseVM it is empty.
/// `concurrency`, or `left/widget` for a class in a module -- the name a shape key is built from.
///
/// Read off the Verse type rather than off the UClass, because **FName does not preserve case
/// outside an editor build**. `WITH_CASE_PRESERVING_NAME` is 1 for the editor and 0 for the runtime
/// host, so there `GetName()` answers the casing of whichever name was interned *first*: a script
/// class called `concurrency` comes back as `Concurrency`, because `/Verse.org/Concurrency` is a
/// module in the standard library and was loaded before it. The key then matches nothing, every
/// field on that class reads as absent, and an exported game silently loses its members -- while the
/// same code in the editor is correct. `AppendMangledName` builds the same string from the Verse
/// side, where the name is a UTF-8 array and its case is its own.
AUTORTFM_DISABLE FUtf8String QualifiedClassName(const UClass* Class)
{
    if (!Class)
    {
        return FUtf8String();
    }
    if (const UVerseClass* const VerseClass = Cast<UVerseClass>(Class))
    {
        if (const Verse::VClass* const VClass = VerseClass->Class.Get())
        {
            TUtf8StringBuilder<256> Builder;
            VClass->AppendMangledName(Builder, UTF8CHAR('/'));
            return FUtf8String(Builder.ToView());
        }
    }
    FString Name = Class->GetName();
    Name.ReplaceCharInline(TEXT('-'), TEXT('/'));
    return FUtf8String(Name);
}

/// The decorated shape key for a member of Object's own class. Factored out because the read and
/// write paths must agree on it exactly.
AUTORTFM_DISABLE FUtf8String ShapeKeyFor(FUtf8StringView ClassName, FUtf8StringView FieldName)
{
    return FUtf8String(UTF8TEXT("(")) + ScriptVersePath + UTF8TEXT("/")
        + FUtf8String(ClassName) + UTF8TEXT(":)") + FUtf8String(FieldName);
}

/// The shape entry for a member, and the class that declares it.
///
/// A shape keys a data member by its *declaring* class -- `(/user@localhost/base_entity:)Health`,
/// never the class the object happens to be. One script class deriving from another is what makes
/// those differ, and before that was tested the object's own name was always the right qualifier.
/// The walk stops where the names stop resolving, which is the first class outside the script
/// package: a mirrored class' members are Godot's own properties and are not in this shape at all.
AUTORTFM_DISABLE const Verse::VShape::VEntry* FindShapeField(Verse::FRunningContext Context,
                                                             UObject* Object,
                                                             FUtf8StringView FieldName,
                                                             FUtf8String& OutDeclaringClass)
{
    Verse::VShape& Shape = UVerseClass::GetShapeForLoadField(Context, Object->GetClass());
    for (const UClass* Cursor = Object->GetClass(); Cursor != nullptr; Cursor = Cursor->GetSuperClass())
    {
        const FUtf8String ClassName = QualifiedClassName(Cursor);
        Verse::VUniqueString& Name =
            Verse::VUniqueString::New(Context, FUtf8StringView(ShapeKeyFor(ClassName, FieldName)));
        if (const Verse::VShape::VEntry* Field = Shape.GetField(Name))
        {
            OutDeclaringClass = ClassName;
            return Field;
        }
    }
    return nullptr;
}

/// Reads FieldName off Object: NoSuchMember for a field the shape does not carry, Unset for one
/// holding no value yet, and the converter's reason for a Verse type with no vh_value counterpart.
AUTORTFM_DISABLE GodotVerse::TResult<void> ReadFieldOf(UObject* Object,
                                                      FUtf8StringView FieldName,
                                                      vh_value& OutValue,
                                                      GodotVerse::FFieldStorage& OutStorage)
{
    if (!Object)
    {
        return EHostFailure::InstanceReleased;
    }

    OutValue = vh_value{};
    OutValue.VariantTag = VH_VARIANT_NIL;
    OutStorage.Text.Reset();
    OutStorage.Blocks.Reset();
    OutStorage.Strings.Reset();

    GodotVerse::TResult<void> Read = EHostFailure::Halted;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    EnterVerse(Context, [&] {
        // The shape lookup is done here rather than through LoadField's by-name overload, which
        // passes Shape.GetField straight into PeekField -- and PeekField asserts on the null that
        // a missing field returns. Asking the shape first is what makes "no such member" an
        // answer instead of a crash.
        //
        // The shape keys a data member by its *declaring* class's decorated name --
        // `(/user@localhost/exports:)Speed`, never a bare `Speed` -- the same decoration
        // FindGodotClass applies to class names and VerseScriptInstance applies to methods.
        // GetClassExports only ever harvests a class's own members, so the object's own class is
        // always the right qualifier.
        FUtf8String DeclaringClass;
        const Verse::VShape::VEntry* Field = FindShapeField(Context, Object, FieldName, DeclaringClass);
        if (Field == nullptr)
        {
            Read = EHostFailure::NoSuchMember;
            return;
        }

        // PeekField over LoadField: an unset member reads as uninitialized here, where LoadField
        // would raise a Verse runtime error. An inspector asking for a value it may not get is
        // not an error condition.
        //
        // A `var` is stored as a reference, and PeekField hands the reference back rather than
        // what it points at -- deliberately, since that is what an assignment needs. Both spellings
        // have to be followed to reach a value: FPropertyVar answers PeekField with a fresh
        // VNativeRef, and a var with no native representation holds a VRef in its VRestValue slot.
        Verse::VValue Value = Field->Type == Verse::EFieldType::FPropertyVar
            ? Verse::VNativeRef::Peek(Context, Object, Field->UProperty)
            : UVerseClass::PeekField(Context, Object, Field);
        if (Verse::VRef* Ref = Value.DynamicCast<Verse::VRef>())
        {
            Value = Ref->Get(Context);
        }
        if (Value.IsUninitialized())
        {
            Read = EHostFailure::Unset;
            return;
        }

        Read = ValueToWire(Context, Value, DescribeMemberType(DeclaringClass, FieldName), OutStorage, OutValue);
    });
    return Read;
}

} // namespace

namespace {

/// Whether ClassName declares FieldName as a `var`, per the semantic program.
///
/// The shape cannot answer this reliably -- its EFieldType says where a field is stored, not what
/// Verse permits -- so the question goes back to the definition that declared it.
AUTORTFM_DISABLE bool IsVarMember(FUtf8StringView ClassName, FUtf8StringView FieldName)
{
    return DescribeMemberType(ClassName, FieldName).bIsVar;
}

/// Assigning writes *through* a var's reference, the way `set X = ...` does. Initializing writes
/// over the storage itself, the way the constructor does, which is the only way to give a non-var
/// a value -- and is wrong for a var, because it would replace the reference with a bare value and
/// leave the next `set` dereferencing something that is not a ref.
enum class EFieldWrite : uint8
{
    Assign,
    Initialize,
};

/// The same lookup for a free function, which is what an extension method is: `operator'.ToString'`
/// lives in the module beside the class rather than in the class, so no amount of asking the object
/// for a member finds it.
AUTORTFM_DISABLE Verse::VFunction* FindVFunctionByDecoratedName(FUtf8StringView DecoratedName)
{
    if (!Verse::GlobalProgram || DecoratedName.IsEmpty())
    {
        return nullptr;
    }
    for (uint32 Index = 0; Index < Verse::GlobalProgram->NumPackages(); ++Index)
    {
        if (Verse::VFunction* Function =
                Verse::GlobalProgram->GetPackage(Index).LookupDefinition<Verse::VFunction>(DecoratedName))
        {
            return Function;
        }
    }
    return nullptr;
}

} // namespace

namespace {

/// Handle -> the script instance bound to it. R-SCN-6's identity half: a node carrying a Verse
/// script has to cross into Verse as *that script's own object*, or `player[GetNode("Player")]`
/// fails on exactly the case the cast exists for -- a fresh mirror wrapper's class is `node`, and
/// no downcast to a script class can succeed against one.
TMap<int64, GodotVerse::FInstance*> GInstancesByHandle;

} // namespace

namespace {

using FFieldValueBuilder =
    TFunctionRef<GodotVerse::TResult<Verse::VValue>(Verse::FRunningContext Context, Verse::VValue Current)>;

AUTORTFM_DISABLE GodotVerse::TResult<void> WriteFieldWith(UObject* Object,
                                                          FUtf8StringView FieldName,
                                                          EFieldWrite Mode,
                                                          FFieldValueBuilder MakeValue)
{
    if (!Object)
    {
        return EHostFailure::InstanceReleased;
    }

    if (Mode == EFieldWrite::Assign && !IsVarMember(QualifiedClassName(Object->GetClass()), FieldName))
    {
        return EHostFailure::NotAssignable;
    }

    GodotVerse::TResult<void> Wrote = EHostFailure::Halted;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    EnterVerse(Context, [&] {
        FUtf8String DeclaringClass;
        const Verse::VShape::VEntry* Field = FindShapeField(Context, Object, FieldName, DeclaringClass);
        if (Field == nullptr)
        {
            Wrote = EHostFailure::NoSuchMember;
            return;
        }

        // A Constant entry lives in the shape itself rather than in the object, so it is shared by
        // every instance and cannot be assigned to.
        if (!Field->IsProperty())
        {
            Wrote = EHostFailure::NotAssignable;
            return;
        }

        // A member's storage already holds a value in the exact representation the compiled code
        // expects, and matching it is the whole job: the VM does not re-check a slot it is told
        // holds a string, so a plausible-looking value of the wrong cell type reads back fine and
        // dies later inside the interpreter. That is why the builder is handed Current.
        Verse::VRestValue* const Slot = Field->Type == Verse::EFieldType::FVerseProperty
            ? Field->UProperty->ContainerPtrToValuePtr<Verse::VRestValue>(Object)
            : nullptr;
        const Verse::VValue Current = Slot ? Slot->Get(Context)
                                           : Verse::VNativeRef::Peek(Context, Object, Field->UProperty);

        const GodotVerse::TResult<Verse::VValue> Built = MakeValue(Context, Current);
        if (!Built)
        {
            Wrote = Built.GetFailure();
            return;
        }
        const Verse::VValue NewValue = Built.GetValue();
        if (NewValue.IsUninitialized())
        {
            Wrote = EHostFailure::ConstructionFailed;
            return;
        }

        if (Slot)
        {
            // A var of a scalar type puts a VRef box in the slot and the value inside it; a var of
            // a container type puts the mutable container straight in. Writing over the box is what
            // the interpreter later dies on with "Unexpected ref type".
            if (Verse::VRef* Ref = Current.DynamicCast<Verse::VRef>())
            {
                Ref->Set(Context, NewValue);
            }
            else
            {
                Slot->Set(Context, NewValue);
            }
            Wrote = GodotVerse::TResult<void>::Ok();
        }
        else
        {
            // Both FProperty and FPropertyVar are native storage behind an FProperty, and
            // VNativeRef::Set is the write for either -- for a var it is the assignment, and for a
            // non-var it is what the interpreter itself uses to initialize one.
            const Verse::FOpResult Result = Verse::VNativeRef::New(Context, Object, Field->UProperty).Set(Context, NewValue);
            Wrote = Result.IsReturn() ? GodotVerse::TResult<void>::Ok()
                                      : GodotVerse::TResult<void>(EHostFailure::ConstructionFailed);
        }
    });
    return Wrote;
}

/// Writes the object an optional reference member should hold, or nothing for null.
///
/// The caller is the one that has checked Referenced against the member's declared class. Neither
/// the shape nor the slot will: a slot told it holds a `?sprite2d` takes whatever object is put in
/// it, and the mistake surfaces the first time compiled code calls a method that is not there.
AUTORTFM_DISABLE GodotVerse::TResult<void> WriteReferenceField(UObject* Object,
                                                               FUtf8StringView FieldName,
                                                               EFieldWrite Mode,
                                                               UObject* Referenced)
{
    return WriteFieldWith(Object, FieldName, Mode,
        [Referenced](Verse::FRunningContext Context, Verse::VValue) -> GodotVerse::TResult<Verse::VValue> {
            return ReferenceOption(Context, Referenced);
        });
}

AUTORTFM_DISABLE GodotVerse::TResult<void> WriteFieldOf(UObject* Object,
                                                        FUtf8StringView FieldName,
                                                        const vh_value& Value,
                                                        EFieldWrite Mode)
{
    if (!Object)
    {
        return EHostFailure::InstanceReleased;
    }

    // A reference arrives as a handle, which names a Godot object and not a Verse one -- so the
    // wrapper has to be built here, and only the declared type says what to build. A member typed
    // as one of the project's own classes is refused: the object it should hold is the one that
    // node's own instance already is, and WriteInstanceFieldInstance is the way to hand that over.
    if (Value.VariantTag == VH_VARIANT_OBJECT)
    {
        const FMemberType Declared = DescribeMemberType(QualifiedClassName(Object->GetClass()), FieldName);
        if (Declared.ReferenceOrigin != EClassOrigin::Mirrored || !Declared.bReferenceIsOption)
        {
            return EHostFailure::TypeMismatch;
        }
        // Built before the VM scope is entered, because constructing it runs the class's Verse
        // constructor through UVerseClass::PostInitInstance, which takes a context of its own.
        const int64 Handle = Value.Type == VH_TYPE_INT ? Value.Int : 0;
        UClass* const DeclaredClass = FindMirroredClass(FUtf8StringView(Declared.ReferenceName));
        UObject* Referenced = Handle != 0 ? GodotVerse::ObjectForHandle(Handle, DeclaredClass) : nullptr;
        if (Handle != 0 && (!Referenced || !Referenced->IsA(DeclaredClass)))
        {
            return EHostFailure::TypeMismatch;
        }
        return WriteReferenceField(Object, FieldName, Mode, Referenced);
    }

    // An enum arrives as its ordinal, which is indistinguishable from any other int. The bound is
    // taken from the enum the author declared rather than from the VM's own enumerator count, which
    // is not the same number. An ordinal outside it is refused rather than clamped: a scene saved
    // against a longer version of the enum will carry one, and it has no enumerator to become.
    if (Value.Type == VH_TYPE_INT)
    {
        const int32 EnumeratorCount = DescribeMemberType(QualifiedClassName(Object->GetClass()), FieldName).EnumeratorCount;
        if (EnumeratorCount > 0 && (Value.Int < 0 || Value.Int >= EnumeratorCount))
        {
            return EHostFailure::EnumOrdinalOutOfRange;
        }
    }

    // A struct and an array both arrive as a sequence of numbers that says nothing about what it is:
    // the field names of the one and the element type of the other are carried only by the declared
    // type, which is where DescribeExportType already worked them out for the export list.
    if (Value.Type == VH_TYPE_TUPLE || Value.Type == VH_TYPE_ARRAY)
    {
        const FMemberType Declared = DescribeMemberType(QualifiedClassName(Object->GetClass()), FieldName);
        if (Declared.Described.Type != Value.Type)
        {
            return EHostFailure::TypeMismatch;
        }

        if (Value.Type == VH_TYPE_TUPLE)
        {
            const FStructLayout* const Layout = Declared.Struct;
            if (!Layout)
            {
                return EHostFailure::Unconvertible;
            }
            return WriteFieldWith(Object, FieldName, Mode,
                [&Value, Layout](Verse::FRunningContext Context, Verse::VValue Current) -> GodotVerse::TResult<Verse::VValue> {
                    // The class is taken from the struct already in the slot, which is the
                    // discipline every write here follows: the new value cannot be of a class the
                    // compiled code was not already expecting.
                    Verse::VRef* const Box = Current.DynamicCast<Verse::VRef>();
                    const Verse::VValue Inner = Box ? Box->Get(Context) : Current;
                    Verse::VValueObject* const Struct = Inner.DynamicCast<Verse::VValueObject>();
                    if (!Struct)
                    {
                        return EHostFailure::TypeMismatch;
                    }
                    return NewStructValue(Context, Struct->GetClass(), *Layout, Value.Seq.Items, Value.Seq.Count);
                });
        }

        const int32 Tag = Declared.Described.VariantTag;
        const int32 ElementTag = Declared.Described.ElementVariantTag;
        return WriteFieldWith(Object, FieldName, Mode,
            [&Value, Tag, ElementTag](Verse::FRunningContext Context, Verse::VValue Current) -> GodotVerse::TResult<Verse::VValue> {
                Verse::VRef* const Box = Current.DynamicCast<Verse::VRef>();
                const Verse::VValue Inner = Box ? Box->Get(Context) : Current;
                return NewArrayValue(
                    Context, Inner.IsCellOfType<Verse::VMutableArray>(), Tag, ElementTag, Value.Seq.Items, Value.Seq.Count);
            });
    }

    return WriteFieldWith(Object, FieldName, Mode,
        [&Value](Verse::FRunningContext Context, Verse::VValue Current) -> GodotVerse::TResult<Verse::VValue> {
            switch (Value.Type)
            {
            case VH_TYPE_LOGIC:
                return Verse::VValue::FromBool(Value.Logic != 0);
            case VH_TYPE_INT:
            {
                // An enum member holds an enumerator rather than a number, and the enumeration it
                // belongs to is reachable only from the enumerator already in the slot -- the usual
                // rule here, that the new value's kind comes from the one it replaces. WriteFieldOf
                // has already bounded the ordinal against the enum the author declared.
                Verse::VRef* const Box = Current.DynamicCast<Verse::VRef>();
                const Verse::VValue Inner = Box ? Box->Get(Context) : Current;
                if (Verse::VEnumerator* Enumerator = Inner.DynamicCast<Verse::VEnumerator>())
                {
                    Verse::VEnumeration* const Enumeration = Enumerator->GetEnumeration();
                    if (!Enumeration || Value.Int < 0 || Value.Int >= Enumeration->NumEnumerators)
                    {
                        return EHostFailure::EnumOrdinalOutOfRange;
                    }
                    return Verse::VValue(Enumeration->GetEnumeratorChecked((int32)Value.Int));
                }
                return Verse::VValue(Verse::VInt(Context, Value.Int));
            }
            case VH_TYPE_FLOAT:
                return Verse::VValue(Verse::VFloat(Value.Float));
            case VH_TYPE_STRING:
            {
                // Verse hangs the mutability of a container off the container, not off a reference
                // around it: `var Label:string` holds a VMutableArray where a plain one holds a
                // VArray.
                const FUtf8StringView Utf8(reinterpret_cast<const UTF8CHAR*>(Value.String.Utf8), Value.String.Len);
                Verse::VRef* const Box = Current.DynamicCast<Verse::VRef>();
                const Verse::VValue Inner = Box ? Box->Get(Context) : Current;
                return Inner.IsCellOfType<Verse::VMutableArray>()
                    ? Verse::VValue(Verse::VMutableArray::New(Context, Utf8))
                    : Verse::VValue(Verse::VArray::New(Context, Utf8));
            }
            default:
                return EHostFailure::Unconvertible;
            }
        });
}

} // namespace

AUTORTFM_DISABLE GodotVerse::TResult<void> GodotVerse::WriteInstanceField(FInstance* Instance,
                                                                          FUtf8StringView FieldName,
                                                                          const vh_value& Value)
{
    if (!Instance || !Instance->Object.IsValid())
    {
        return EHostFailure::InstanceReleased;
    }
    return WriteFieldOf(Instance->Object.Get(), FieldName, Value,
                        Instance->bSealed ? EFieldWrite::Assign : EFieldWrite::Initialize);
}

AUTORTFM_DISABLE GodotVerse::TResult<void> GodotVerse::WriteInstanceFieldInstance(FInstance* Instance,
                                                                                  FUtf8StringView FieldName,
                                                                                  const FInstance* Value)
{
    if (!Instance || !Instance->Object.IsValid())
    {
        return EHostFailure::InstanceReleased;
    }

    UObject* Referenced = Value && Value->Object.IsValid() ? Value->Object.Get() : nullptr;
    const FMemberType Declared = DescribeMemberType(QualifiedClassName(Instance->Object->GetClass()), FieldName);
    if (Declared.ReferenceName.IsEmpty())
    {
        return EHostFailure::TypeMismatch;
    }

    // The class check the slot will not do. A mirrored member accepts an instance too, and should:
    // a `?node2d` assigned a node that carries a script is better off holding that script's own
    // object than a second wrapper around the same handle, which would give one node two identities.
    if (Referenced)
    {
        UClass* MemberClass = Declared.ReferenceOrigin == EClassOrigin::Script
            ? FindGodotClass(FUtf8StringView(Declared.ReferenceQualifiedName))
            : FindMirroredClass(FUtf8StringView(Declared.ReferenceName));
        if (!MemberClass)
        {
            return EHostFailure::NotPublished;
        }
        if (!Referenced->GetClass()->IsChildOf(MemberClass))
        {
            return EHostFailure::TypeMismatch;
        }
    }

    return WriteReferenceField(Instance->Object.Get(), FieldName,
                               Instance->bSealed ? EFieldWrite::Assign : EFieldWrite::Initialize, Referenced);
}

AUTORTFM_DISABLE GodotVerse::TResult<void> GodotVerse::ReadInstanceField(const FInstance* Instance,
                                                                         FUtf8StringView FieldName,
                                                                         vh_value& OutValue,
                                                                         FFieldStorage& OutStorage)
{
    if (!Instance || !Instance->Object.IsValid())
    {
        return EHostFailure::InstanceReleased;
    }
    return ReadFieldOf(Instance->Object.Get(), FieldName, OutValue, OutStorage);
}

AUTORTFM_DISABLE UObject* GodotVerse::NewDefaultsObject(FUtf8StringView ClassName)
{
    // Nothing under here gets a Godot object -- not this instance, and not whatever its member
    // initializers construct, which is the half that matters. See FSuppressMintScope.
    return NewHostObject(FindGodotClass(ClassName), FHostPeer::Suppressed());
}

AUTORTFM_DISABLE TSharedPtr<const GodotVerse::FFieldValue> GodotVerse::ReadDefaultFieldOf(UObject* Defaults, FUtf8StringView FieldName)
{
    if (!Defaults)
    {
        return nullptr;
    }
    TSharedRef<GodotVerse::FFieldValue> Read = MakeShared<GodotVerse::FFieldValue>();
    if (!ReadFieldOf(Defaults, FieldName, Read->Value, Read->Storage))
    {
        return nullptr;
    }
    return Read;
}

AUTORTFM_DISABLE bool GodotVerse::ReadClassDefaultField(FUtf8StringView ClassName,
                                                        FUtf8StringView FieldName,
                                                        TSharedPtr<const FFieldValue>& OutValue)
{
    OutValue.Reset();

    if (GetAnalysisSnapshot())
    {
        if (const FAnalysisSnapshot::FClass* const Found = GetAnalysisSnapshot()->Classes.Find(FUtf8String(ClassName)))
        {
            if (const TSharedPtr<const FFieldValue>* const Cached = Found->Defaults.Find(FUtf8String(FieldName)))
            {
                OutValue = *Cached;
                return OutValue.IsValid();
            }
        }
    }

    // A member that is not an `@export`, which the snapshot does not carry: the inspector only ever
    // asks about the ones it was given, so reading every member of every class would be work for
    // nobody. Still answerable, but only from outside a build -- the read enters the VM.
    if (IsBackgroundCheckRunning())
    {
        return false;
    }
    TStrongObjectPtr<UObject> Defaults(NewDefaultsObject(ClassName));
    OutValue = ReadDefaultFieldOf(Defaults.Get(), FieldName);
    return OutValue.IsValid();
}

AUTORTFM_DISABLE UObject* GodotVerse::PeekFieldObject(UObject* Object, FUtf8StringView FieldName)
{
    if (!Object)
    {
        return nullptr;
    }
    UObject* Found = nullptr;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    EnterVerse(Context, [&] {
        FUtf8String DeclaringClass;
        const Verse::VShape::VEntry* Field = FindShapeField(Context, Object, FieldName, DeclaringClass);
        if (Field == nullptr)
        {
            return;
        }
        Verse::VValue Value = Field->Type == Verse::EFieldType::FPropertyVar
            ? Verse::VNativeRef::Peek(Context, Object, Field->UProperty)
            : UVerseClass::PeekField(Context, Object, Field);
        if (Verse::VRef* Ref = Value.DynamicCast<Verse::VRef>())
        {
            Value = Ref->Get(Context);
        }
        Found = Value.ExtractUObject();
    });
    return Found;
}

AUTORTFM_DISABLE GodotVerse::FInstance* GodotVerse::Instantiate(FUtf8StringView ClassName, int64 Handle)
{
    UClass* NativeClass = FindGodotClass(ClassName);
    if (!NativeClass)
    {
        ReportError(FUtf8String(UTF8TEXT("Could not instantiate ")) + FUtf8String(ClassName)
                    + UTF8TEXT(": no such class deriving from object at ") + ScriptVersePath);
        return nullptr;
    }

    // The instance's own task scope exists before its constructor runs, so a `spawn` from a class
    // body or from an inherited initializer joins this node's task group like every later call
    // does. Made eagerly rather than at the first spawn: FContentScopeGuard has no hook that could
    // say "a task was just started", and UVerseClass::PostInitInstance below is already a VM entry
    // that would have to pick a scope. What it costs is measured in phase-5-design.md 14.
    TSharedRef<verse::FContentScope> Scope = verse::MakeContentScope(ContentScopeOuter());

    UObject* Instance = nullptr;
    {
        verse::FContentScopeGuard Guard(Scope);
        // The node Godot already made is this instance's peer, and the block clause on vh_object
        // adopts it rather than minting a second one -- which is the whole of docs/phase-4b-
        // design.md 4.3, and the failure that would not have announced itself.
        //
        // UVerseClass::PostInitInstance runs the Verse constructor from inside NewObject, so fields
        // are initialised by the time this returns.
        Instance = NewHostObject(NativeClass, FHostPeer::Adopt(Handle));
    }
    if (!Instance)
    {
        return nullptr;
    }

    verse::vh_object* Shadow = CastChecked<verse::vh_object>(Instance);
    Shadow->Handle.Set(Handle, Shadow);

    // Before the instance is handed back, so a Ready() that emits already has a bound signal.
    TArray<int64> EventBindings;
    BindSignals(Instance, ClassName, Handle, EventBindings);

    FInstance* Made = new FInstance{TStrongObjectPtr<UObject>(Instance), Handle, Scope};
    Made->EventBindings = MoveTemp(EventBindings);
    GInstancesByHandle.Add(Handle, Made);
    return Made;
}

AUTORTFM_DISABLE void GodotVerse::ReleaseInstance(FInstance* Instance)
{
    if (Instance)
    {
        // Only if this instance is still the one registered: a node freed and its id reused would
        // be a Godot bug, but a double release here would take the live row with it.
        if (FInstance** Bound = GInstancesByHandle.Find(Instance->Handle); Bound && *Bound == Instance)
        {
            GInstancesByHandle.Remove(Instance->Handle);
        }

        // R-ASYNC-5: the instance dying is what cancels its tasks, which is GDScript's own trigger
        // (~GDScriptInstance clears pending_func_states). Leaving the tree is not -- pooling and
        // re-parenting remove and re-add nodes constantly, and what stalls an await then is the
        // source no longer emitting.
        //
        // Godot defers the real free to _flush_delete_queue, so a `queue_free`d node's tasks run
        // until then. That window is GDScript's too, and is documented rather than closed.
        if (Instance->Scope.IsValid() && !Instance->Scope->WasTerminated())
        {
            Instance->Scope->Terminate();
        }

        // The `@export_signal` connections this instance made. Godot's side of each dies with the
        // node -- the Callable is owned by this same object -- but the binding row holds a strong
        // pointer to the event and a reference id, and nothing else would ever end those. A row
        // kept here is a GC root per scripted node, which is a leak that looks entirely normal.
        ReleaseEventBindings(Instance->EventBindings);
    }
    delete Instance;
}

namespace {
AUTORTFM_DISABLE FVerseFunction LookupMethod(const GodotVerse::FInstance* Instance, FUtf8StringView DecoratedName)
{
    if (!Instance || !Instance->Object.IsValid())
    {
        return FVerseFunction(EDefaultConstructVerseFunction::UnsafeDoNotUse);
    }
    const verse::FExecutionContext Context = verse::FExecutionContext::GetActiveContext();
    return FVerseFunction(Context, Instance->Object.Get(), DecoratedName);
}
}

/// Whether the script actually implements this virtual, rather than inheriting the empty body the
/// mirrored class gives it.
///
/// Every one of Godot's virtuals is a member of the class that declares it, with a default body, so
/// a plain "does it resolve" test is true for every instance. Comparing against what the *base*
/// resolves to is what distinguishes an override from the inherited no-op, and it decides whether
/// Godot puts this node in the per-frame process list at all.
///
/// Two things here are not obvious. The base is found by walking the superclass chain rather than
/// asked of a fixed root, because since Phase 4 the declaring class is `node`, or `canvas_item`, or
/// `control` -- wherever Godot declares the virtual -- rather than the one native root.
///
/// And the comparison is on the **procedure**, not the function cell: a method is stored once per
/// shape and `Bind` makes a fresh VFunction around it every time a field is loaded, so two loads
/// are two cells whatever they run. Comparing cells made every method look overridden.
AUTORTFM_DISABLE bool GodotVerse::InstanceHasFunction(const FInstance* Instance, FUtf8StringView DecoratedName)
{
    FVerseFunction Resolved = LookupMethod(Instance, DecoratedName);
    if (!Resolved.IsValid() || !Instance->Object.IsValid())
    {
        return false;
    }
    Verse::VCell* const Own = Resolved.Function->Procedure.Get().ExtractCell();
    if (!Own)
    {
        return true;
    }

    const verse::FExecutionContext Context = verse::FExecutionContext::GetActiveContext();
    for (UClass* Super = Instance->Object->GetClass()->GetSuperClass(); Super != nullptr;
         Super = Super->GetSuperClass())
    {
        const FVerseFunction Base(Context, Super->GetDefaultObject(), DecoratedName);
        if (Base.IsValid())
        {
            return Base.Function->Procedure.Get().ExtractCell() != Own;
        }
    }
    return true;
}

AUTORTFM_DISABLE int32 GodotVerse::InstanceCall(FInstance* Instance,
                                                FUtf8StringView DecoratedName,
                                                const vh_value* Args,
                                                int32 ArgCount,
                                                vh_value& OutResult,
                                                FFieldStorage& OutStorage)
{
    OutResult = vh_value{};
    OutStorage.Text.Reset();
    OutStorage.Blocks.Reset();
    OutStorage.Strings.Reset();

    if (!Instance || !Instance->Object.IsValid())
    {
        return StatusFor(EHostFailure::InstanceReleased);
    }

    // The first entry into this instance is the earliest point at which Godot will accept a connect
    // to one of the script's own signals, so it is where an `@export_signal` event member's
    // connection is made. A no-op for every instance that declares none, and for every call after
    // the first.
    EnsureEventConnections(Instance->EventBindings);

    // The signature, for the parameter and result types. Asked of the semantic program rather than
    // of the VM because that is the only view that carries declared types -- the bytecode has
    // erased them by the time a VValue exists.
    const FUtf8String ClassName = QualifiedClassName(Instance->Object->GetClass());
    TArray<FMethodDesc> Methods;
    if (!GetClassMethods(FUtf8StringView(ClassName), Methods))
    {
        return StatusFor(EHostFailure::NoSuchClass);
    }
    const FMethodDesc* Method = Methods.FindByPredicate(
        [DecoratedName](const FMethodDesc& Candidate) { return FUtf8StringView(Candidate.DecoratedName).Equals(DecoratedName); });
    if (!Method)
    {
        return StatusFor(EHostFailure::NoSuchMethod);
    }

    // One shape cannot be settled yet: a single *struct* parameter is satisfied by one Godot
    // argument per field, and which parameters are structs is not known until the declared types
    // are read below. Everything else is decided here, as it always was.
    const bool bArityMayBeStructPack = Method->Params.Num() == 1 && ArgCount != 1;
    if (!bArityMayBeStructPack && (ArgCount < Method->RequiredParamCount || ArgCount > Method->Params.Num()))
    {
        return StatusFor(EHostFailure::WrongArgumentCount);
    }

    FVerseFunction Resolved = LookupMethod(Instance, DecoratedName);
    if (!Resolved.IsValid())
    {
        return StatusFor(EHostFailure::NoSuchMethod);
    }

    // Parameter descriptions are not carried on FMethodDesc, which holds only what crosses the ABI,
    // so they come from the same recorded table the method itself came from -- the analysis
    // snapshot in an editor host, the cook's table in a runtime host, which has no semantic program
    // and can never have one.
    //
    // **Not from the live semantic program, which is a different program by the time this runs.**
    // IR generation rewrites the one the build was holding: a method answering a struct gets a
    // *coerced* override generated beside it (IRGenerator.cpp, MaybeCreateCoercedFunctionDefinition),
    // and that generated function decorates to the same name with one synthetic `Argument`
    // parameter added. Walking the class live therefore found a one-parameter signature for a
    // no-parameter method, the arity check below refused the call as VH_ERR_NOT_FOUND, and
    // `Control.get_minimum_size()` on a script overriding `_GetMinimumSize` quietly answered
    // Godot's default (0, 0) -- with nothing said anywhere. The snapshot is taken before IR
    // generation runs, which is the only description of what the author actually declared.
    TArray<FMemberType> ParamTypes;
    FMemberType ResultTypeDesc;
    RecordedMethodTypes(ClassName, DecoratedName, ParamTypes, ResultTypeDesc);
    if (ParamTypes.Num() != Method->Params.Num())
    {
        return StatusFor(EHostFailure::SignatureNotRecorded);
    }

    // **N Godot arguments satisfy one struct parameter, one per field.**
    //
    // This is the inbound half of a struct signal payload. The payload decomposes into one argument
    // per field on the way out, which is what gives the connect dialog real names (R-SIG-1), so
    // Godot invokes the handler with that many while the handler declares the struct. Verse already
    // reads a multi-parameter function as satisfying a one-tuple-parameter callback for the same
    // reason -- a function's parameter *is* its tuple -- so this is that rule extended to the one
    // Verse spelling that carries names.
    //
    // In InstanceCall rather than in the callback path so there is one rule rather than one per
    // caller -- but note what that does *not* buy. A direct `node.call("OnReported", 1, 2, 3)` never
    // reaches here: Object::call checks arity against the script's method list first, which reports
    // the one declared parameter, and answers "Expected 1 argument(s)" on Godot's side. A Callable
    // invocation is the difference, arriving through vh_callback_invoke, which is not arity-checked.
    // Measured, after the opposite was asserted and the test said otherwise.
    vh_value PackedStructArg{};
    if (ParamTypes.Num() == 1 && ParamTypes[0].UserStruct.IsValid()
        && ParamTypes[0].UserStruct->FieldKeys.Num() == ArgCount)
    {
        PackedStructArg.Type = VH_TYPE_TUPLE;
        // Borrowed for the duration of the call, like every other pointer on this wire, and the
        // tuple lane is already `const vh_value*` -- so a field can be anything a field can be,
        // rather than the scalars a mirrored math struct is limited to.
        PackedStructArg.Seq.Items = Args;
        PackedStructArg.Seq.Count = ArgCount;
        Args = &PackedStructArg;
        ArgCount = 1;
    }
    else if (ArgCount < Method->RequiredParamCount || ArgCount > Method->Params.Num())
    {
        // The deferred half of the check above. Reached only for a one-parameter method that turned
        // out not to be a struct taking this many fields.
        return StatusFor(EHostFailure::WrongArgumentCount);
    }

    // After the arity is settled, so a call that was never going to run does not seal the instance
    // against the construction-time writes it still permits.
    Instance->bSealed = true;

    // R-DIAG-5's boundary row. Named after the procedure the call resolved to rather than after
    // the decorated name asked for, so an inherited body reports the file and line it is actually
    // in. Inert, and builds no string at all, while profiling is off.
    const GodotVerse::FProfileScope ProfileScope(
        GodotVerse::ProfileSignature(Resolved.Function.Get(), FUtf8StringView(ClassName)));

    int32 Status = VH_OK;
    // Set inside the VM, read outside it. EnterVM is allowed to decline to run its functor --
    // a terminated content scope is one reason and there may be others -- and a call that did
    // not happen must never come back as one that did. That confusion is the whole reason this
    // defect was silent for a phase.
    bool bBodyRan = false;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    const verse::FExecutionContext ExecContext = verse::FExecutionContext::GetActiveContext();

    const AutoRTFM::ETransactionResult TransactionResult = AutoRTFM::Transact([&] {
        // Open, inside the transaction. TVerseFunction's own operator() is AUTORTFM_OPEN and this
        // is the same reason: raising a Verse runtime error from closed code trips
        // AutoRTFM::UnreachableIfClosed in FContext::RaiseVerseRuntimeError and takes the process
        // down, rather than unwinding the way a raise is supposed to.
        AutoRTFM::Open([&] {
        EnterVerseOn(Context, *Instance, [&] {
            bBodyRan = true;
            Verse::VFunction::Args Converted;
            Converted.Reserve(ArgCount);
            for (int32 Index = 0; Index < ArgCount; ++Index)
            {
                Verse::VValue Value;
                const TResult<void> Crossed = WireToValue(Context, Args[Index], ParamTypes[Index], Value);
                if (!Crossed)
                {
                    Status = StatusFor(Crossed.GetFailure());
                    return;
                }
                Converted.Add(Value);
            }

            const Verse::FOpResult OpResult = Resolved.Function->Invoke(Context, MoveTemp(Converted));
            switch (OpResult.Kind)
            {
            case Verse::FOpResult::Return:
                // A void method still returns -- of false, which is Verse's empty tuple -- so the
                // declared result type is what decides whether there is a value to read, not the
                // presence of one.
                if (ResultTypeDesc.Described.Type != VH_TYPE_VOID)
                {
                    const TResult<void> Answered = ValueToWire(Context, OpResult.Value, ResultTypeDesc, OutStorage, OutResult);
                    if (!Answered)
                    {
                        Status = StatusFor(Answered.GetFailure());
                    }
                }
                break;

            case Verse::FOpResult::Fail:
                // A <decides> method that ran and declined. Distinct from VH_ERR_NOT_FOUND, which
                // would say there had been nothing to call.
                Status = StatusFor(EHostFailure::Declined);
                break;

            case Verse::FOpResult::Yield:
                // A <suspends> method started a task instead of completing. Nothing is wrong and
                // there is no value; the task runs on under vh_tick.
                break;

            default:
                Status = StatusFor(EHostFailure::Aborted);
                break;
            }
        });
        });
    });

    // A raise unwinds to the root failure context and aborts the transaction, which is what drops
    // the Godot writes this call had deferred. The transaction result is the only place that is
    // visible from here: the raise itself does not return through us.
    if (TransactionResult != AutoRTFM::ETransactionResult::Committed)
    {
        return StatusFor(EHostFailure::Aborted);
    }
    return bBodyRan ? Status : StatusFor(EHostFailure::Halted);
}

AUTORTFM_DISABLE int32 GodotVerse::InstanceToString(FInstance* Instance,
                                                    vh_value& OutResult,
                                                    FFieldStorage& OutStorage)
{
    OutResult = vh_value{};
    OutStorage.Text.Reset();
    OutStorage.Blocks.Reset();
    OutStorage.Strings.Reset();

    if (!Instance || !Instance->Object.IsValid())
    {
        return StatusFor(EHostFailure::InstanceReleased);
    }

    // From the snapshot, so this costs no analysis and never waits -- Godot asks for an object's
    // text from the remote inspector and from `print`, neither of which is a moment to block on.
    const FUtf8String ClassName = QualifiedClassName(Instance->Object->GetClass());
    const FAnalysisSnapshot::FClass* const Found =
        GetAnalysisSnapshot() ? GetAnalysisSnapshot()->Classes.Find(ClassName) : nullptr;
    if (!Found || Found->ToStringDecorated.IsEmpty())
    {
        return StatusFor(EHostFailure::NoSuchMethod);
    }

    Verse::VFunction* const Function = FindVFunctionByDecoratedName(FUtf8StringView(Found->ToStringDecorated));
    if (!Function)
    {
        return StatusFor(EHostFailure::NoSuchMethod);
    }

    // The result is read as a `string`, which is the only thing Godot has anywhere to put it. The
    // match above was on the name and the receiver, not the result type, so a project that declares
    // `(X:c).ToString():int` reaches here -- and ValueToWire declines it, which the consumer turns
    // back into Godot's own representation. Wrong rather than refused at the declaration, and
    // harmless, which is why it is not worth a diagnostic of its own.
    FMemberType StringType;
    StringType.Kind = EDeclaredKind::String;
    StringType.Described.Type = VH_TYPE_STRING;

    int32 Status = VH_OK;
    bool bBodyRan = false;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};

    const AutoRTFM::ETransactionResult TransactionResult = AutoRTFM::Transact([&] {
        // Open inside the transaction, for the reason InstanceCall states: a Verse runtime error
        // raised from closed code trips AutoRTFM::UnreachableIfClosed instead of unwinding.
        AutoRTFM::Open([&] {
        EnterVerseOn(Context, *Instance, [&] {
            bBodyRan = true;
            // The two parameters an extension method actually has. The receiver is not `Self` here
            // -- it is an ordinary first argument -- and the second is the call's own argument
            // list, which for a no-argument ToString is the empty tuple.
            Verse::VFunction::Args Converted;
            Converted.Reserve(2);
            Converted.Add(Verse::VValue(Instance->Object.Get()));
            Converted.Add(Verse::VValue(Verse::GlobalFalse()));

            const Verse::FOpResult OpResult = Function->Invoke(Context, MoveTemp(Converted));
            switch (OpResult.Kind)
            {
            case Verse::FOpResult::Return:
                if (const TResult<void> Answered = ValueToWire(Context, OpResult.Value, StringType, OutStorage, OutResult);
                    !Answered)
                {
                    Status = StatusFor(Answered.GetFailure());
                }
                break;

            case Verse::FOpResult::Fail:
                Status = StatusFor(EHostFailure::Declined);
                break;

            default:
                Status = StatusFor(EHostFailure::Aborted);
                break;
            }
        });
        });
    });

    if (TransactionResult != AutoRTFM::ETransactionResult::Committed)
    {
        return StatusFor(EHostFailure::Aborted);
    }
    if (!bBodyRan)
    {
        return StatusFor(EHostFailure::Halted);
    }
    return Status;
}

AUTORTFM_DISABLE bool GodotVerse::DescribeBoundFunction(Verse::VFunction* Function, int64& OutHandle, FUtf8String& OutDecorated)
{
    if (!Function)
    {
        return false;
    }
    UObject* const Self = Function->Self.Get().ExtractUObject();
    verse::vh_object* const Shadow = Cast<verse::vh_object>(Self);
    if (!Shadow)
    {
        return false;
    }

    const FUtf8String ClassName = QualifiedClassName(Self->GetClass());
    TArray<GodotVerse::FMethodDesc> Methods;
    if (!GodotVerse::GetClassMethods(FUtf8StringView(ClassName), Methods))
    {
        return false;
    }

    // The *procedure*, not the function cell. A method is stored once per shape and `Bind` makes a
    // fresh VFunction around it every time the field is loaded, so two loads of one method are two
    // cells; what they share is the code they run.
    Verse::VCell* const Wanted = Function->Procedure.Get().ExtractCell();
    if (!Wanted)
    {
        return false;
    }

    const verse::FExecutionContext ExecContext = verse::FExecutionContext::GetActiveContext();
    for (const GodotVerse::FMethodDesc& Method : Methods)
    {
        const FVerseFunction Candidate(ExecContext, Self, FUtf8StringView(Method.DecoratedName));
        if (Candidate.IsValid() && Candidate.Function->Procedure.Get().ExtractCell() == Wanted)
        {
            OutHandle = Shadow->Handle.Get();
            OutDecorated = Method.DecoratedName;
            return true;
        }
    }
    return false;
}

AUTORTFM_DISABLE int64 GodotVerse::MakeCallableFor(const FVerseValue& Callback)
{
    Verse::VFunction* const Function = Callback.GetValue().DynamicCast<Verse::VFunction>();
    int64 OwnerHandle = 0;
    FUtf8String Decorated;
    if (!DescribeBoundFunction(Function, OwnerHandle, Decorated))
    {
        ReportError(Function
            ? UTF8TEXT("MakeCallable was given a Verse function that is not a method bound to a live "
                       "script instance. Only a bound method can be made into a Callable today "
                       "(OQ-16): Godot's own unbound spelling is anchored to the script resource and "
                       "is its known leak.")
            : UTF8TEXT("MakeCallable was given a value that is not a Verse function."));
        return 0;
    }

    FHostState& Host = GetHost();
    if (!Host.Godot.MakeCallable)
    {
        return 0;
    }

    const int64 Id = AddCallback(FCallbackTarget{OwnerHandle, Decorated});
    const int64 Ref = Host.Godot.MakeCallable(Host.Godot.Ctx, Id, OwnerHandle);
    if (Ref == 0)
    {
        RemoveCallback(Id);
        VH_UNREPORTED("MakeCallableFor: Godot minted no Callable for a bound method");
    }
    return Ref;
}

AUTORTFM_DISABLE void GodotVerse::ReleaseCallback(int64 CallbackId)
{
    RemoveCallback(CallbackId);
}

AUTORTFM_DISABLE int32 GodotVerse::InvokeCallback(int64 CallbackId,
                                                  const vh_value* Args,
                                                  int32 ArgCount,
                                                  vh_value& OutResult,
                                                  FFieldStorage& OutStorage)
{
    const TResult<FCallbackTarget> Found = FindCallback(CallbackId);
    if (!Found)
    {
        return StatusFor(Found.GetFailure());
    }
    const FCallbackTarget& Target = Found.GetValue();

    // A Callable the host minted to feed a suspended task rather than to call a script method. It
    // resumes inside this emission, which is where GDScript resumes a coroutine too.
    if (Target.AwaitToken != 0)
    {
        const TResult<void> Delivered = DeliverToAwaiter(Target.AwaitToken, Args, ArgCount);
        return Delivered ? VH_OK : StatusFor(Delivered.GetFailure());
    }

    // The permanent connection an `@export_signal` event member holds. Before the instance lookup
    // below for the same reason the await branch is: this Callable feeds an event rather than
    // calling a method, so there is no decorated name to resolve.
    if (Target.EventSignalId != 0)
    {
        const TResult<void> Delivered = DeliverToEvent(Target.EventSignalId, Args, ArgCount);
        return Delivered ? VH_OK : StatusFor(Delivered.GetFailure());
    }

    FInstance** const Bound = GInstancesByHandle.Find(Target.OwnerHandle);
    if (!Bound || !*Bound)
    {
        // The node was freed. Godot's own is_valid() should have caught this first; answering
        // rather than raising is what keeps a late emission from taking the frame down.
        return StatusFor(EHostFailure::UnknownId);
    }

    // A foreign signal's subscriber: nothing declares that signal's payload, so the arguments cross
    // as the one container Godot itself would have put them in and the handler takes a godot_array.
    if (Target.bArgsAsArray)
    {
        FHostState& Host = GetHost();
        if (!Host.Godot.NewRef || !Host.Godot.RefSet)
        {
            return StatusFor(EHostFailure::GodotUnavailable);
        }
        const int64 Ref = Host.Godot.NewRef(Host.Godot.Ctx, VH_VARIANT_ARRAY);
        if (Ref == 0)
        {
            return StatusFor(EHostFailure::GodotUnavailable);
        }
        for (int32 Index = 0; Index < ArgCount; ++Index)
        {
            vh_value Key{};
            Key.Type = VH_TYPE_INT;
            Key.Int = Index;
            Host.Godot.RefSet(Host.Godot.Ctx, Ref, &Key, &Args[Index]);
        }
        // Ownership of the fresh reference passes to the `godot_array` WireToValue builds for the
        // parameter, whose BeginDestroy releases it -- so nothing here releases it on the way out.
        vh_value Packed{};
        Packed.Type = VH_TYPE_REF;
        Packed.VariantTag = VH_VARIANT_ARRAY;
        Packed.Ref = Ref;
        return InstanceCall(*Bound, FUtf8StringView(Target.DecoratedName), &Packed, 1, OutResult, OutStorage);
    }

    return InstanceCall(*Bound, FUtf8StringView(Target.DecoratedName), Args, ArgCount, OutResult, OutStorage);
}

AUTORTFM_DISABLE int32 GodotVerse::PeakTasksOfAnyInstance()
{
    int32 Peak = 0;
    for (const TPair<int64, GodotVerse::FInstance*>& Pair : GInstancesByHandle)
    {
        const GodotVerse::FInstance* const Instance = Pair.Value;
        if (!Instance || !Instance->Scope.IsValid())
        {
            continue;
        }
        if (Verse::VTaskGroup* const Group = Instance->Scope->GetTaskGroup())
        {
            Peak = FMath::Max(Peak, static_cast<int32>(Group->GetNumActive()));
        }
    }
    return Peak;
}

AUTORTFM_DISABLE UObject* GodotVerse::InstanceObjectForHandle(int64 Handle)
{
    if (FInstance** Bound = GInstancesByHandle.Find(Handle))
    {
        if (*Bound && (*Bound)->Object.IsValid())
        {
            return (*Bound)->Object.Get();
        }
    }
    return nullptr;
}
