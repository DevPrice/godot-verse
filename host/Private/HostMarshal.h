// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/StringView.h"
#include "HostScript.h"
#include "HostTypeModel.h"
#include "verse_host_abi.h"

class UClass;
class UObject;

namespace Verse {
struct VClass;
}

/// Values across the wire in both directions: a Verse value to the vh_value its declaration says it
/// crosses as, and a vh_value back to the Verse value a declaration accepts. What a value cannot say
/// about itself -- an empty option, an empty array's element kind, a struct's field order -- comes
/// from the FMemberType the type model described.
namespace GodotVerse {

/// The UClass behind a mirrored Godot class -- node2d, texture2d -- which is what a reference to
/// one has to be built from.
///
/// Looked up across every package in the program rather than in a named one. A script's class lives
/// in the package the host itself compiles, whose name FindGodotClass can spell; a mirrored class
/// comes from the package VNI built alongside the module, whose VM name is assembled out of the
/// mount point and the C++ module name -- two things this file would be guessing at. The decorated
/// name identifies the class on its own, so the package it is found in does not need predicting.
AUTORTFM_DISABLE Verse::VClass* FindMirroredVClass(Verse::FRunningContext Context, FUtf8StringView ClassName);

AUTORTFM_DISABLE Verse::VValue NewStructValue(Verse::FRunningContext Context,
                                              Verse::VClass& Class,
                                              const FStructLayout& Layout,
                                              const vh_value* Items,
                                              int32 ItemCount);

/// Builds a vh_value from a Verse value, given what its declaration says it is.
///
/// The declaration is not optional context. A read cannot tell a `?node2d` holding nothing from a
/// `logic` holding false, because Verse spells an empty option and false with the same cell; an
/// empty array carries no element type; and a struct's field order is nowhere in the value. Taking
/// the description rather than a member name is what lets a method's return value and a member
/// read share one conversion.
AUTORTFM_DISABLE bool ValueToWire(Verse::FRunningContext Context,
                                  Verse::VValue Value,
                                  const FMemberType& Declared,
                                  FFieldStorage& OutStorage,
                                  vh_value& OutValue);

/// What an optional reference member holds: an option around the object, or Verse's `false` for one
/// holding nothing.
/// The Verse class that wraps a reference of this Godot type.
AUTORTFM_DISABLE UClass* FindReferenceClass(int32 VariantTag);

/// One reference wrapper, holding the id and owning it from here on.
///
/// A UObject rather than a VM cell: the id has to be released when Verse drops the value, and only
/// a UObject is told when that happens. godot_ref::BeginDestroy is the other half.
AUTORTFM_DISABLE UObject* NewReferenceWrapper(UClass* NativeClass, int64 Id);

AUTORTFM_DISABLE Verse::VValue ReferenceOption(Verse::FRunningContext Context, UObject* Referenced);

/// A Verse array holding Items, built as the Godot container Tag names it.
///
/// Mutability is taken from the array already in the slot, for the same reason a string's is: Verse
/// hangs it off the container, so a `var` holds a VMutableArray where a plain member holds a VArray,
/// and writing the wrong one leaves storage the interpreter later dies on. The element storage kind
/// is always VValue -- the narrower EArrayType cases are an optimisation the VM reads back through
/// GetValue either way, and an empty array in the slot has no kind to copy.
AUTORTFM_DISABLE Verse::VValue NewArrayValue(Verse::FRunningContext Context,
                                            bool bMutable,
                                            int32 Tag,
                                            int32 ElementTag,
                                            const vh_value* Items,
                                            int32 ItemCount);

/// Builds the value to write, given the one already in the slot. An uninitialized return means the
/// value has no representation in this member and nothing is written.
/// Builds a Verse value of the declared type from the wire.
///
/// The write path for *members* takes the class and the mutability off the value already in the
/// slot, which is the safest source when there is one. A method argument has no slot: nothing has
/// been assigned yet, so everything -- the struct's class, the array's element kind, the enum an
/// ordinal belongs to -- has to come from the declaration. That is the whole difference between
/// this and WriteFieldOf's builders, and it is why they are not one function.
///
/// Returns false for a wire value the declared type cannot accept, which is VH_ERR_ARGUMENT to the
/// caller rather than a runtime error: the script is not at fault for how it was called.
AUTORTFM_DISABLE bool WireToValue(Verse::FRunningContext Context,
                                  const vh_value& Value,
                                  const FMemberType& Declared,
                                  Verse::VValue& OutValue);

/// Builds a wire value from a Verse value that is one of the mirrored math structs -- `vector2`,
/// `color`, `transform3d` -- identified from the value alone rather than from a declaration.
///
/// Every other caller knows what it is reading and passes an FMemberType; the debugger does not,
/// because a stopped frame carries no declaration and reaching for one would mean asking the
/// semantic program from inside the interpreter. What makes it answerable anyway is that a struct
/// value names its own class: `VNamedType::GetBaseName()` is the key the generated layout table is
/// keyed by.
///
/// False for anything that is not one of them -- including a class of the author's that happens to
/// share a name, because the fields are read by their *decorated* keys
/// (`(/Godot.org/Godot/vector2:)X`) and a namesake has none of them.
AUTORTFM_DISABLE bool ReadMathStruct(Verse::FRunningContext Context,
                                     Verse::VValue Value,
                                     FFieldStorage& OutStorage,
                                     vh_value& OutValue);

/// Builds a wire value from *any* Verse value that identifies itself, which is ReadMathStruct's
/// question asked of every type rather than only of the math structs: an int, a float, a logic, a
/// string, a Godot object and the 16 math structs. False for everything else.
///
/// Two callers, and they want it for the same reason from opposite directions. The debugger has a
/// register and no declaration. `Variant(Value:any)` has an argument whose declared type is `any`,
/// which says nothing by construction -- the type dispatch Verse cannot do at compile time, done
/// here at run time instead.
///
/// **The order of the tests is load-bearing and not obvious.** `true` is an option around `false`,
/// so a cell holding a Godot object reads as a logic if it is asked before the object test; and a
/// string is a `VArrayBase` of Char8/Char32, so it has to be settled before anything that treats an
/// array as an array. What cannot be settled at all is what the value does not know about itself:
/// an empty array cannot say what it holds, a `false` cannot say whether it is a logic or an empty
/// option, and a `[]int` cannot say which of Godot's three integer packings was meant. Those are
/// the lanes that keep a named builder.
AUTORTFM_DISABLE bool ReadSelfDescribingValue(Verse::FRunningContext Context,
                                              Verse::VValue Value,
                                              FFieldStorage& OutStorage,
                                              vh_value& OutValue);

} // namespace GodotVerse
