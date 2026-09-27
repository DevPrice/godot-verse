// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostEngineAdapters.h"

#include "Containers/Map.h"
#include "HAL/PlatformTime.h"
#include "HostScriptState.h"
#include "HostTypeModel.h"
#include "ULangUEUtils.h"
#include "uLang/Semantics/Attributable.h"
#include "uLang/Semantics/Definition.h"
#include "uLang/Semantics/Expression.h"
#include "uLang/Semantics/SemanticClass.h"
#include "uLang/Semantics/SemanticFunction.h"
#include "uLang/Semantics/SemanticProgram.h"
#include "uLang/Semantics/SemanticTypes.h"
#include "uLang/Syntax/VstNode.h"

#include <cstdio>

namespace GodotVerse {

struct FMirrorDefinition
{
    int32 PathIndex{INDEX_NONE};
    int32 Line{-1};
    int32 Column{-1};
    bool bIsClassVarAccessor{false};
};

namespace {

/// Where the parse says a definition was written.
AUTORTFM_DISABLE void LocationFromVst(const uLang::CDefinition& Definition, FUtf8String& OutPath, int32& OutLine, int32& OutColumn)
{
    if (const uLang::CExpressionBase* DefinitionNode = PrototypeOf(Definition).GetAstNode())
    {
        if (const Verse::Vst::Node* Vst = DefinitionNode->GetMappedVstNode())
        {
            const Verse::SLocus& Whence = Vst->Whence();
            OutPath = FULangConversionUtils::ULangStrToFUtf8String(Vst->GetSnippetPath());
            OutLine = (int32)Whence.BeginRow();
            OutColumn = (int32)Whence.BeginColumn();
        }
    }
}

/// The files those definitions were written in, named once each rather than once per definition.
TArray<FUtf8String> GMirrorPaths;

/// Every definition /Godot.org/Godot declares, keyed by qualified name and signature. Empty until
/// the first build fills it, which is also what lets the mirror be retired to its digest.
TMap<FUtf8String, FMirrorDefinition> GMirrorDefinitions;
bool GMirrorRecorded = false;

/// How many definitions shared a key with one already recorded -- see RecordMirrorScope. Traced
/// rather than asserted on, since a repeat costs a line number and not a file.
int32 GMirrorKeyCollisions = 0;

/// What names a mirror definition across two analyses of it.
///
/// CScope::GetScopePath walks logical scopes only -- a snippet is not one -- so the qualified name
/// is the same string whether the definition was read from `GodotClasses.native.verse` or from the
/// one synthetic snippet a digest is. The signature is what tells an overload from its sibling:
/// GodotMath declares `operator'+'` once per math type, all of them at module scope with two
/// parameters, and the function type is the only thing that differs.
AUTORTFM_DISABLE FUtf8String MirrorKeyOf(const uLang::CDefinition& RawDefinition)
{
    // The prototype's name, so a member reached through a concrete instantiation --
    // `typed_array(node).ToArray` -- keys to the generic declaration this table actually recorded.
    // Recording walks the generic, whose prototype is itself, so no recorded key moves.
    const uLang::CDefinition& Definition = PrototypeOf(RawDefinition);
    FUtf8String Key = FULangConversionUtils::ULangStrToFUtf8String(uLang::GetQualifiedNameString(Definition));
    if (const uLang::CFunction* Function = Definition.AsNullable<uLang::CFunction>())
    {
        if (const uLang::CFunctionType* Type = Function->_Signature.GetFunctionType())
        {
            Key += UTF8TEXT(" ");
            Key += FULangConversionUtils::ULangStrToFUtf8String(Type->AsCode());
        }
    }
    return Key;
}

/// The class a parametric type definition stands for, or null for anything else.
///
/// `signal(t) := class(...)` is a CFunction whose result is a type rather than a value, so every
/// walk that tests for a function and stops finds one here. The class is inside the CTypeType the
/// signature answers -- the same unwrap DescribeCompletion does for an archetype's receiver.
AUTORTFM_DISABLE const uLang::CClass* ParametricClassOf(const uLang::CDefinition& Definition)
{
    const uLang::CFunction* const Function = Definition.AsNullable<uLang::CFunction>();
    if (!Function)
    {
        return nullptr;
    }
    const uLang::CFunctionType* const Type = Function->_Signature.GetFunctionType();
    if (!Type)
    {
        return nullptr;
    }
    const uLang::CTypeType* const TypeType = Type->GetReturnType().GetNormalType().AsNullable<uLang::CTypeType>();
    if (!TypeType || !TypeType->PositiveType())
    {
        return nullptr;
    }
    return TypeType->PositiveType()->GetNormalType().AsNullable<uLang::CClass>();
}

AUTORTFM_DISABLE void RecordMirrorScope(const uLang::CLogicalScope& Scope)
{
    for (const uLang::TSRef<uLang::CDefinition>& Definition : Scope.GetDefinitions())
    {
        FUtf8String Path;
        int32 Line = -1;
        int32 Column = -1;
        LocationFromVst(*Definition, Path, Line, Column);
        if (!Path.IsEmpty())
        {
            FMirrorDefinition Entry;
            Entry.PathIndex = GMirrorPaths.AddUnique(Path);
            Entry.Line = Line;
            Entry.Column = Column;
            if (const uLang::CFunction* Function = Definition->AsNullable<uLang::CFunction>())
            {
                Entry.bIsClassVarAccessor = Function->_bIsAccessorOfSomeClassVar;
            }
            // A key that repeats means two definitions this cannot tell apart, and the first is no
            // worse a guess than the last -- they are siblings in one scope, so what differs is the
            // line and not the file. Counted so that a drift in what a key has to carry is visible
            // rather than silent.
            FUtf8String Key = MirrorKeyOf(*Definition);
            if (GMirrorDefinitions.Contains(Key))
            {
                ++GMirrorKeyCollisions;
                // Named, not just counted: a count says a key has stopped being unique and leaves
                // whoever reads it to find out which, and the answer decides whether the cost is a
                // line number or a whole definition's documentation.
                if (AnalysisTraceEnabled())
                {
                    fprintf(stderr, "[vh-trace]   mirror key collision: %s\n", reinterpret_cast<const char*>(*Key));
                }
            }
            GMirrorDefinitions.FindOrAdd(MoveTemp(Key), Entry);
        }

        // Not into a function: its parameters and locals are reachable from nowhere an editor can
        // put a cursor, and nothing reads the location of a parameter item -- Godot's argument hint
        // draws a name and a type (verse_script_language.cpp, call_hint_for) and no more.
        const uLang::CLogicalScope* const Inner = Definition->DefinitionAsLogicalScopeNullable();
        if (Inner && !Definition->AsNullable<uLang::CFunction>())
        {
            RecordMirrorScope(*Inner);
        }
        else if (const uLang::CClass* const Parametric = ParametricClassOf(*Definition))
        {
            // A *parametric* class is a CFunction -- `signal(t) := class...` is a function
            // answering a type -- so the rule above walked straight past every member of one, and
            // signal(t).Await, typed_array(t).ToArray and event(t)'s members had no recorded
            // location at all. After the first build the mirror is read from its digest, and a
            // digest is one synthetic snippet with no file behind it, so the consumer had nothing
            // to read a comment from and hovered them with a type and an empty box.
            RecordMirrorScope(*Parametric);
        }
    }
}

} // namespace

/// Instantiating a parametric class makes a fresh CDefinition per member -- `typed_array(node)`
/// has its own `ToArray` -- and none of them was written anywhere: the file, the line and the
/// prose all belong to the generic declaration they were instantiated from. uLang says so itself
/// where it *ensures* against `GetAttributes` on an instantiated definition, "which inherits its
/// attributes from its prototype definition" (uLang/Semantics/Definition.h:222).
///
/// An ordinary definition is its own prototype, so this is identity for all but the mirror's four
/// parametric classes.
AUTORTFM_DISABLE const uLang::CDefinition& PrototypeOf(const uLang::CDefinition& Definition)
{
    const uLang::CDefinition* const Prototype = Definition.GetPrototypeDefinition();
    return Prototype ? *Prototype : Definition;
}

/// Gated on the package so that a script's own definitions -- which are read from their real files
/// on every analysis -- never pay for building a key. The attribute package shares the mirror's
/// verse path and so passes the gate; it stays Source, and the table has its definitions recorded
/// with the same file and line its own parse would have given, so either answer is the same one.
AUTORTFM_DISABLE const FMirrorDefinition* FindMirrorDefinition(const uLang::CDefinition& Definition)
{
    if (GMirrorDefinitions.IsEmpty())
    {
        return nullptr;
    }
    const uLang::CAstPackage* const Package = Definition._EnclosingScope.GetPackage();
    if (!Package || !FUtf8StringView(Package->_VersePath.AsCString()).Equals(FUtf8StringView(GodotVersePath)))
    {
        return nullptr;
    }
    return GMirrorDefinitions.Find(MirrorKeyOf(Definition));
}

AUTORTFM_DISABLE bool MirrorDefinitionsRecorded()
{
    return GMirrorRecorded;
}

AUTORTFM_DISABLE void ForgetMirrorDefinitions()
{
    GMirrorPaths.Empty();
    GMirrorDefinitions.Empty();
    GMirrorKeyCollisions = 0;
    GMirrorRecorded = false;
}

/// Records where every definition of the generated mirror was written, and which of its functions
/// are a class var's accessors.
///
/// This is what lets /Godot.org/Godot be read from its digest, which takes an analysis from
/// ~1410 ms to ~740 ms whatever the project's own size is. A digest is one synthetic snippet of
/// bodiless declarations at a path the toolchain picks (SolarisModule.cpp, MakeDigestSnippet) and
/// *no such file is ever written*, so a package read from one loses two things the editor is built
/// on:
///
///   - every definition's file and line, which is goto-definition, the doc comment a tooltip reads
///     "above" a declaration, and -- because a top-level definition reports the file it was written
///     in as its owner -- whether the Godot side recognises a name as one of the package's globals
///     at all (`is_godot_package_global`, which pairs the owner with the path);
///   - CFunction::_bIsAccessorOfSomeClassVar, which the analyzer sets only where it resolves a
///     class var's `<getter>`/`<setter>` attributes against the functions they name. The digest
///     re-emits the var and not the attributes, so every one of the mirror's property accessors
///     comes back offerable as an override.
///
/// Both are answered from here instead. Taken from the first build's own semantic analysis, which
/// is the last program that reads the mirror's own files, through the post-analysis injection --
/// so the table costs a walk of a program that exists anyway rather than an analysis of its own.
AUTORTFM_DISABLE void RecordMirrorDefinitions(const uLang::CSemanticProgram& Program)
{
    if (GMirrorRecorded)
    {
        return;
    }
    const uLang::CModule* const Mirror = Program.FindDefinitionByVersePath<uLang::CModule>(GodotVersePath);
    if (!Mirror)
    {
        return;
    }

    const double Started = FPlatformTime::Seconds();
    RecordMirrorScope(*Mirror);
    GMirrorRecorded = true;

    if (AnalysisTraceEnabled())
    {
        SIZE_T Bytes = GMirrorDefinitions.GetAllocatedSize();
        for (const TPair<FUtf8String, FMirrorDefinition>& Entry : GMirrorDefinitions)
        {
            Bytes += Entry.Key.GetAllocatedSize();
        }
        fprintf(stderr,
                "[vh-trace] mirror table: %d definition(s) in %d file(s), %d ambiguous, %d KB retained, %.1f ms\n",
                GMirrorDefinitions.Num(),
                GMirrorPaths.Num(),
                GMirrorKeyCollisions,
                (int32)(Bytes / 1024),
                (FPlatformTime::Seconds() - Started) * 1000.0);
        fflush(stderr);
    }
}

AUTORTFM_DISABLE void FillLocation(const uLang::CDefinition& Definition,
                                   const FMirrorDefinition* Recorded,
                                   FUtf8String& OutPath,
                                   int32& OutLine,
                                   int32& OutColumn)
{
    if (Recorded)
    {
        OutPath = GMirrorPaths[Recorded->PathIndex];
        OutLine = Recorded->Line;
        OutColumn = Recorded->Column;
        return;
    }
    LocationFromVst(Definition, OutPath, OutLine, OutColumn);
}

AUTORTFM_DISABLE void FillLocation(const uLang::CDefinition& Definition, FUtf8String& OutPath, int32& OutLine, int32& OutColumn)
{
    FillLocation(Definition, FindMirrorDefinition(Definition), OutPath, OutLine, OutColumn);
}

/// A top-level definition's owner is the file it was written in, since a snippet scope carries its
/// path as its name. That makes it a location as much as a name -- the Godot side tests the two
/// against each other to recognise a package global -- so a mirror definition answers it from the
/// table as well.
///
/// A class is named the way every ClassNameUtf8 in the ABI names one: `left/widget` for a script
/// class under a `.vmodule`, its bare name for a mirrored or a bound one. The consumer registers
/// a script class's documentation under that string and Godot looks the doc up by it, so a
/// member owned by the bare `widget` found no documentation at all inside a module (B38).
AUTORTFM_DISABLE FUtf8String OwnerNameOf(const uLang::CDefinition& Definition, const FMirrorDefinition* Recorded)
{
    if (Recorded && Definition._EnclosingScope.GetKind() == uLang::CScope::EKind::Snippet)
    {
        return GMirrorPaths[Recorded->PathIndex];
    }
    if (Definition._EnclosingScope.GetKind() == uLang::CScope::EKind::Class)
    {
        return QualifiedNameOf(static_cast<const uLang::CClass&>(Definition._EnclosingScope));
    }
    return FUtf8String(Definition._EnclosingScope.GetScopeName().AsCString());
}

AUTORTFM_DISABLE FUtf8String OwnerNameOf(const uLang::CDefinition& Definition)
{
    return OwnerNameOf(Definition, FindMirrorDefinition(Definition));
}

/// The flag is set by the analyzer where it resolves a class var's `<getter>`/`<setter>` against
/// the functions they name, and a digest re-emits the var without the attributes -- so for the
/// mirror, which is read from its digest after the first build, the answer comes from the side
/// table that recorded it while those attributes were still there.
AUTORTFM_DISABLE bool IsClassVarAccessor(const uLang::CFunction& Function, const FMirrorDefinition* Recorded)
{
    return Function._bIsAccessorOfSomeClassVar || (Recorded && Recorded->bIsClassVarAccessor);
}

AUTORTFM_DISABLE const uLang::CClass* FindScriptClass(const uLang::CSemanticProgram& Program, FUtf8StringView ClassName)
{
    const FUtf8String ClassPath = FUtf8String(ScriptVersePath) + UTF8TEXT("/") + FUtf8String(ClassName);
    return Program.FindDefinitionByVersePath<uLang::CClass>(
        FULangConversionUtils::FUtf8StringViewToULangStringView(ClassPath));
}

/// The one spelling three readers had each built inline -- the enum reader, the statics reader and
/// the struct-payload field keys -- and now the class lookup needs it too.
AUTORTFM_DISABLE FUtf8String DecoratedNameOf(const uLang::CDefinition& Definition)
{
    return FUtf8String(UTF8TEXT("("))
        + FULangConversionUtils::ULangStrToFUtf8String(
              Definition._EnclosingScope.GetScopePath('/', uLang::CScope::EPathMode::PrefixSeparator))
        + UTF8TEXT(":)") + FUtf8String(Definition.AsNameCString());
}

AUTORTFM_DISABLE FUtf8String ExtensionMethodName(const uLang::CFunction& Function)
{
    const uLang::CSemanticProgram& Program = Function._EnclosingScope.GetProgram();
    return FULangConversionUtils::ULangStrToFUtf8String(
        uLang::CUTF8String(Program._IntrinsicSymbols.StripExtensionFieldOpName(Function.GetName())));
}

/// Measured rather than assumed: a class is keyed by DecoratedNameOf alone, and reusing that for
/// R-NODE-10's `ToString` found nothing, so to_string silently kept Godot's own text.
AUTORTFM_DISABLE FUtf8String ExtensionMethodDecoratedName(const uLang::CFunction& Function)
{
    return FUtf8String(UTF8TEXT("("))
        + FULangConversionUtils::ULangStrToFUtf8String(
              Function._EnclosingScope.GetScopePath('/', uLang::CScope::EPathMode::PrefixSeparator))
        + UTF8TEXT(":)")
        + FULangConversionUtils::ULangStrToFUtf8String(Function.GetDecoratedName());
}

AUTORTFM_DISABLE TOptional<FUtf8String> AttributeArgument(const uLang::CAttributable& Attributes,
                                                          const uLang::CClass* AttributeClass,
                                                          const uLang::CSemanticProgram& Program)
{
    if (!AttributeClass)
    {
        return {};
    }
    // Matches on the invocation's *return type*, which is why a caller names the attribute class
    // rather than the `<constructor>` beside it, and declines an argument that is a MakeTuple.
    const uLang::TOptional<uLang::CUTF8String> Text = Attributes.GetAttributeTextValue(AttributeClass, Program);
    if (!Text.IsSet())
    {
        return {};
    }
    return FULangConversionUtils::ULangStrToFUtf8String(*Text);
}

namespace {

/// The shape matches verse_doc_comment_above's exactly (src/verse_doc_markup.h has the rules): a
/// `#` line is what follows the delimiter and one space, indentation kept so an indented sample is
/// still a code block to the consumer's converter; a `<# ... #>` block and a `<#>` comment are the
/// text between or under their delimiters, the continuation lines dedented by what they share and
/// blank lines at either end dropped; every line trimmed at its end and joined with newlines.
AUTORTFM_DISABLE void AppendCommentProse(TArray<FUtf8String>& OutLines, const FUtf8String& Source)
{
    FUtf8StringView Text = FUtf8StringView(Source);
    Text.TrimStartAndEndInline();

    // `<#>` before `<#`, or the longer delimiter is read as the shorter plus a `>`.
    if (Text.StartsWith(FUtf8StringView(UTF8TEXT("<#>"))))
    {
        Text.RightChopInline(3);
    }
    else if (Text.StartsWith(FUtf8StringView(UTF8TEXT("<#"))))
    {
        Text.RightChopInline(2);
        if (Text.EndsWith(FUtf8StringView(UTF8TEXT("#>"))))
        {
            Text.LeftChopInline(2);
        }
    }
    else if (Text.StartsWith(FUtf8StringView(UTF8TEXT("#"))))
    {
        Text.RightChopInline(1);
    }

    TArray<FUtf8String> Lines;
    FUtf8String(Text).ParseIntoArray(Lines, UTF8TEXT("\n"), /*InCullEmpty*/ false);
    if (Lines.IsEmpty())
    {
        return;
    }
    for (FUtf8String& Line : Lines)
    {
        Line.TrimEndInline();
    }

    // The delimiter's own line: one space is the delimiter's, the rest is text.
    FUtf8String& First = Lines[0];
    if (First.StartsWith(UTF8TEXT(" ")))
    {
        First.RightChopInline(1);
    }

    // The continuation lines, dedented by the leading whitespace every non-blank one shares.
    int32 Common = -1;
    for (int32 Index = 1; Index < Lines.Num(); Index++)
    {
        const FUtf8String& Line = Lines[Index];
        int32 Indent = 0;
        while (Indent < Line.Len() && (Line[Indent] == UTF8CHAR(' ') || Line[Indent] == UTF8CHAR('\t')))
        {
            Indent++;
        }
        if (Indent == Line.Len())
        {
            continue;
        }
        if (Common < 0)
        {
            Common = Indent;
            continue;
        }
        const FUtf8String& Reference = Lines[1];
        int32 Shared = 0;
        while (Shared < Common && Shared < Indent && Line[Shared] == Reference[Shared])
        {
            Shared++;
        }
        Common = Shared;
    }
    for (int32 Index = 1; Index < Lines.Num() && Common > 0; Index++)
    {
        FUtf8String& Line = Lines[Index];
        if (Line.Len() <= Common)
        {
            Line.Empty();
        }
        else
        {
            Line.RightChopInline(Common);
        }
    }

    int32 Begin = 0;
    int32 End = Lines.Num();
    while (Begin < End && Lines[Begin].IsEmpty())
    {
        Begin++;
    }
    while (End > Begin && Lines[End - 1].IsEmpty())
    {
        End--;
    }
    for (int32 Index = Begin; Index < End; Index++)
    {
        OutLines.Add(Lines[Index]);
    }
}

} // namespace

/// Two sources, and a definition has one or the other rather than both.
///
/// **`@doc("...")` is how Verse's own library documents itself** -- 132 of them across
/// /Verse.org/Verse, `Sqrt` and `Concatenate` among them -- and an attribute's text is reachable
/// from nowhere else: a consumer reading the source file above the declaration finds an attribute
/// line, not prose.
///
/// **A comment block is how this bridge documents its own**, and the parser keeps one as prefix
/// comments on the node that begins the construct. That is the same association
/// verse_doc_comment_above warns about on the consumer's side: for a member behind four lines of
/// `@editable` the comments hang off the attribute clause rather than the member, so this can come
/// back empty where re-reading the file would not. It is a fallback for a definition whose file
/// the consumer cannot open, not a replacement for that reading.
AUTORTFM_DISABLE FUtf8String DocOf(const uLang::CDefinition& Definition, const uLang::CSemanticProgram& Program)
{
    // The prototype: prose is written once, on the generic declaration, and GetAttributes
    // *ensures* against being asked of an instantiated definition (PrototypeOf says why).
    const uLang::CDefinition& Prototype = PrototypeOf(Definition);

    if (const TOptional<FUtf8String> Text = AttributeArgument(Prototype.GetAttributes(), Program._doc_attribute.Get(), Program);
        Text.IsSet() && !Text->IsEmpty())
    {
        return Text.GetValue();
    }

    const uLang::CExpressionBase* const Ast = Prototype.GetAstNode();
    const Verse::Vst::Node* const Vst = Ast ? Ast->GetMappedVstNode() : nullptr;
    if (!Vst)
    {
        return FUtf8String();
    }

    TArray<FUtf8String> Lines;
    for (const Verse::Vst::TNodeRef<Verse::Vst::Node>& Node : Vst->GetPrefixComments())
    {
        const Verse::Vst::Comment* const Comment = Node->AsNullable<Verse::Vst::Comment>();
        if (!Comment)
        {
            continue;
        }
        // Exhaustive on purpose: a fifth kind is how a doc-comment syntax would arrive, and it
        // should fail this build rather than be read as an ordinary comment
        // (tripwire/no_doc_comment_syntax). The node's text is the comment as written, delimiters
        // included, for all four (tLang.cpp prints it back verbatim), so one stripping rule serves.
        VH_EXHAUSTIVE_SWITCH_BEGIN
        switch (Comment->_Type)
        {
        case Verse::Vst::Comment::EType::block:
        case Verse::Vst::Comment::EType::line:
        case Verse::Vst::Comment::EType::ind:
        case Verse::Vst::Comment::EType::frag:
            AppendCommentProse(Lines, FUtf8String(Comment->GetSourceCStr()));
            break;
        }
        VH_EXHAUSTIVE_SWITCH_END
    }
    FUtf8String Prose;
    for (const FUtf8String& Line : Lines)
    {
        if (!Prose.IsEmpty())
        {
            Prose += UTF8TEXT("\n");
        }
        Prose += Line;
    }
    return Prose;
}

} // namespace GodotVerse
