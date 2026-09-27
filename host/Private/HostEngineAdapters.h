// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/StringView.h"
#include "Containers/UnrealString.h"

namespace uLang {
class CClass;
class CDefinition;
class CFunction;
class CSemanticProgram;
}

/// The rules for asking uLang about a definition that no caller may skip, each behind one function:
/// where a definition was written, what it is called, and how the VM keys it. An engine drop that
/// moves one of them is a change here rather than at every call site.
///
/// Two things make "where was this written" harder than reading a locus. An instantiated member of
/// a parametric class was written nowhere -- its prototype was. And after the first build
/// /Godot.org/Godot is read from its digest, which drops every definition's file and line and every
/// class var accessor flag, so a mirror definition's answer comes from a side table recorded while
/// those were still there. FillLocation, OwnerNameOf and IsClassVarAccessor consult it themselves.
namespace GodotVerse {

/// The definition a question about source should be asked of: the generic declaration an
/// instantiated one came from, and the definition itself otherwise.
AUTORTFM_DISABLE const uLang::CDefinition& PrototypeOf(const uLang::CDefinition& Definition);

/// One mirror definition as its own source file spells it. Opaque: FindMirrorDefinition's answer is
/// only ever handed back to the functions below.
struct FMirrorDefinition;

/// The side table's row for a mirror definition, or null for one it does not name -- which includes
/// every definition outside /Godot.org/Godot. Building the key spells the whole function type, so a
/// caller asking about one definition several times asks this once and passes the row along.
AUTORTFM_DISABLE const FMirrorDefinition* FindMirrorDefinition(const uLang::CDefinition& Definition);

/// Where a definition was written, or nothing for one compiled from a package the project does
/// not own -- Verse's own library.
AUTORTFM_DISABLE void FillLocation(const uLang::CDefinition& Definition, FUtf8String& OutPath, int32& OutLine, int32& OutColumn);
AUTORTFM_DISABLE void FillLocation(const uLang::CDefinition& Definition,
                                   const FMirrorDefinition* Recorded,
                                   FUtf8String& OutPath,
                                   int32& OutLine,
                                   int32& OutColumn);

/// The name an answer carries as a definition's owner: the module-qualified class for a member,
/// and for a top-level definition the file it was written in.
AUTORTFM_DISABLE FUtf8String OwnerNameOf(const uLang::CDefinition& Definition);
AUTORTFM_DISABLE FUtf8String OwnerNameOf(const uLang::CDefinition& Definition, const FMirrorDefinition* Recorded);

/// Whether this function is the `<getter>` or `<setter>` a class var names.
AUTORTFM_DISABLE bool IsClassVarAccessor(const uLang::CFunction& Function, const FMirrorDefinition* Recorded);

/// Records the side table off a program that read the mirror from its own files, which is the first
/// build's. Once per process, until ForgetMirrorDefinitions; a second call does nothing.
AUTORTFM_DISABLE void RecordMirrorDefinitions(const uLang::CSemanticProgram& Program);

/// Whether the table has been recorded, which is what decides whether the mirror may be retired to
/// its digest.
AUTORTFM_DISABLE bool MirrorDefinitionsRecorded();

/// Drops the table, so that a host torn down and started again records it afresh.
AUTORTFM_DISABLE void ForgetMirrorDefinitions();

/// One of the script package's own classes by the module-qualified name every ClassNameUtf8 in the
/// ABI carries -- `player`, `gameplay/player` -- or null. The reverse, a class's own qualified
/// name, is QualifiedNameOf in HostTypeModel.h.
AUTORTFM_DISABLE const uLang::CClass* FindScriptClass(const uLang::CSemanticProgram& Program, FUtf8StringView ClassName);

/// The decorated name of a semantic definition: `(<enclosing scope path>:)<name>`, which is how the
/// VM keys a class, an enumeration or a struct field.
AUTORTFM_DISABLE FUtf8String DecoratedNameOf(const uLang::CDefinition& Definition);

/// What an author writes for an extension method: `Length` for the `operator'.Length'` that
/// `(V:vector2).Length()` declares.
AUTORTFM_DISABLE FUtf8String ExtensionMethodName(const uLang::CFunction& Function);

/// The key a VPackage holds an extension method under, which is not DecoratedNameOf's shape: the
/// enclosing scope again, around the function's whole decorated name, which carries its own scope
/// prefix and its signature -- so two overloads of one extension method have two keys.
///
///   (/user@localhost:)(/user@localhost:)operator'.ToString'(:(...:)game_state,:tuple())
AUTORTFM_DISABLE FUtf8String ExtensionMethodDecoratedName(const uLang::CFunction& Function);

} // namespace GodotVerse
