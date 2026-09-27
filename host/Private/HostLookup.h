// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Containers/StringView.h"
#include "Containers/UnrealString.h"
#include "Containers/Utf8String.h"
#include "HostResult.h"
#include "verse_host_abi.h"

namespace uLang {
class CClass;
}

/// Lookup, completion and the argument hint: every question the editor asks about a position or a
/// class's members, answered off the AST and the snapshot an analysis left and never by entering
/// the VM. It is the code Epic's eventual Verse language server replaces, so the surface is kept to
/// the ABI's five entry points and the three things the snapshot records out of it.
namespace GodotVerse {

/// One definition an identifier resolved to, with the source location to jump to.
struct FLookupDesc
{
    FUtf8String Name;
    /// Empty for a definition the project has no file for -- the generated Godot API, the Verse
    /// standard library -- which can be described but not jumped to.
    FUtf8String Path;
    /// Zero-based row; the column is a byte offset into it, which is how uLang counts and is not
    /// how Godot counts.
    int32 Line{-1};
    int32 Column{-1};
    FUtf8String Type;
    /// Name of the declaring scope: for a method, the class that declares it.
    FUtf8String Owner;
    vh_lookup_kind Kind{VH_LOOKUP_UNKNOWN};
    bool bIsVar{false};
    /// A parameter of the function that declares it. It has no documentation of its own: its
    /// source line is the line the whole function is declared on.
    bool bIsParameter{false};
    /// The cursor was on the definition itself, not on a reference to it.
    bool bIsDefinition{false};
    /// The definition this one immediately overrides, if any. An override cannot rename, so its
    /// name is Name above.
    FUtf8String OverriddenOwner;
    FUtf8String OverriddenPath;
    int32 OverriddenLine{-1};
    int32 OverriddenColumn{-1};
    /// Prose, delimiters stripped, or empty. See DocOf.
    FUtf8String Doc;
    FUtf8String OverriddenDoc;
};

/// Resolves the identifier at Line/Column of Path against the analysed program.
///
/// Positions are the compiler's: zero-based rows, byte-offset columns. The caller is responsible
/// for only asking about text the last analysis actually saw -- nothing here can detect an edit
/// since, and a stale locus is a confident jump to the wrong line.
///
/// Only ever answers off an analysis-only program: code generation replaces a definition's AST node
/// with an IR node, and the accessors this walks assert rather than fall back. After a build, and
/// while an analysis is rebuilding the program, it declines with the reason instead of waiting.
AUTORTFM_DISABLE TResult<FLookupDesc> LookupSymbol(FUtf8StringView Path, int32 Line, int32 Column);

/// One name completion could offer, described the way FLookupDesc describes a definition minus
/// the location -- completion says what a name is, not where it was written.
struct FCompleteItem
{
    FUtf8String Name;
    FUtf8String Type;
    FUtf8String Owner;
    /// Where it was declared, empty and -1 for a definition with no source behind it. Carried so
    /// a consumer can find the comment block above a name it is offering or documenting.
    FUtf8String Path;
    int32 Line{-1};
    vh_lookup_kind Kind{VH_LOOKUP_UNKNOWN};
    bool bIsVar{false};
    /// Parameters declared, or -1 for anything that is not a function.
    int32 ParamCount{-1};
    /// A function's declaration after its name, as Verse source: "(Delta:float)<transacts>:void".
    /// The parameter names live on the signature rather than the function type, so Type cannot
    /// carry them and an editor writing a declaration has nowhere else to get them.
    FUtf8String Signature;
    /// Whether a subclass could declare this with <override>.
    bool bIsOverridable{false};
    /// Hops from the class or scope the query was about to the one that declares this, or -1 when
    /// it was reached through a `using`. See vh_complete_item::OwnerDistance for why an import is
    /// not a number of hops.
    int32 OwnerDistance{-1};
    /// Whether this is a named parameter -- `?ExactMatch:logic = false` -- which a call site writes
    /// as `?ExactMatch := true` and may not pass positionally. Only SignatureAt fills it; the
    /// parameter's own definition does not carry it (see vh_complete_item::IsNamed).
    bool bIsNamed{false};
};

/// The parameters of one function, for an editor's argument hint.
struct FSignatureDesc
{
    FUtf8String Name;
    FUtf8String Result;
    TArray<FCompleteItem> Params;
};

/// Every member ClassName declares itself, read off the snapshot the last analysis left. Broader
/// than GetClassExports -- methods included, `@editable` not required -- because this exists to
/// become documentation rather than an inspector.
///
/// NotAnalysed before any analysis has landed; NoSuchClass for a class the last one did not have.
AUTORTFM_DISABLE TResult<TArray<FCompleteItem>> ClassMembers(FUtf8StringView ClassName);

/// What ClassName could still declare with <override>: every overridable member it inherits and
/// does not already declare, described exactly as Complete describes the same names in
/// VH_COMPLETE_SCOPE at a member declaration inside that class.
///
/// Same extraction, so the two cannot drift -- the walk is CollectClassAndSupers with Seen already
/// holding the class' own names, which is what makes the superclass' copy of an override the
/// class has written lose to it there and be absent here. Declines as ClassMembers does.
AUTORTFM_DISABLE TResult<TArray<FCompleteItem>> ClassOverrideCandidates(FUtf8StringView ClassName);

/// The function called at Line/Column of Path, with that file's text replaced by SourceText, and
/// its parameters. Line/Column name the callee's last byte rather than the cursor: the argument
/// list being typed does not analyse, so there is nothing at the cursor to resolve.
///
/// Reads the program the last analysis left, exactly as Complete does, and declines as it does
/// when that analysis was not of SourceText.
AUTORTFM_DISABLE TResult<FSignatureDesc> SignatureAt(FUtf8StringView Path,
                                                     const FUtf8String& SourceText,
                                                     int32 Line,
                                                     int32 Column);

/// Lists what could be written at Line/Column of Path -- the members of the expression there, or
/// everything its scope admits.
///
/// Runs no analysis: it reads the AST the last one left behind, and declines with
/// BufferNotAnalysed or AnalysisRunning unless that analysis was of this very buffer. It does not
/// have to have analysed *cleanly* -- uLang keeps the analysed children of an expression it could
/// not analyse, which is what lets `Position.` still name vector2 as the receiver.
AUTORTFM_DISABLE TResult<TArray<FCompleteItem>> Complete(FUtf8StringView Path,
                                                         const FUtf8String& SourceText,
                                                         int32 Line,
                                                         int32 Column,
                                                         vh_complete_mode Mode);

/// ClassMembers' answer for one script class, read off the current program rather than the
/// snapshot, which is what the snapshot is filled from. False when the program has no such class.
AUTORTFM_DISABLE bool ClassMembersLive(FUtf8StringView ClassName, TArray<FCompleteItem>& OutItems);

/// The same for any class the caller already holds -- a generated binding, which is not in the
/// script package the name lookup above searches.
AUTORTFM_DISABLE void ClassOwnMembers(const uLang::CClass& Class, TArray<FCompleteItem>& OutItems);

/// ClassOverrideCandidates' answer, read off the current program. OwnMembers is what
/// ClassMembersLive answered for the same class, which is what "does not already declare" is
/// tested against.
AUTORTFM_DISABLE bool ClassOverrideCandidatesLive(FUtf8StringView ClassName,
                                                  const TArray<FCompleteItem>& OwnMembers,
                                                  TArray<FCompleteItem>& OutItems);

/// The type of the receiver an unresolved member access at Row/Column of Path hangs off, as Verse
/// spells it, or empty when there is no receiver there. Row and Column are uLang's diagnostic
/// locus, counted from one. Runs on whichever thread just finished the analysis that reported it.
AUTORTFM_DISABLE FUtf8String SubjectTypeOfDiagnostic(FUtf8StringView Path, int32 Row, int32 Column);

} // namespace GodotVerse
