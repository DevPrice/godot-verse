// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostLookup.h"

#include "Containers/Set.h"
#include "HAL/PlatformTime.h"
#include "HostEngineAdapters.h"
#include "HostScript.h"
#include "HostSnapshot.h"
#include "HostScriptState.h"
#include "HostTypeModel.h"
#include "ULangUEUtils.h"
#include "uLang/Semantics/DataDefinition.h"
#include "uLang/Semantics/Definition.h"
#include "uLang/Semantics/Expression.h"
#include "uLang/Semantics/ModuleAlias.h"
#include "uLang/Semantics/SemanticClass.h"
#include "uLang/Semantics/SemanticEnumeration.h"
#include "uLang/Semantics/SemanticFunction.h"
#include "uLang/Semantics/SemanticProgram.h"
#include "uLang/Semantics/SemanticTypes.h"
#include "uLang/Semantics/TypeAlias.h"
#include "uLang/SourceProject/UploadedAtFNVersion.h"
#include "uLang/Syntax/VstNode.h"

#include <cstdio>

namespace GodotVerse {

namespace {

/// An STextRange ends exclusively, so the position one past an identifier's last byte does not
/// resolve to it. That is what makes hovering the `(` of a call miss the callee rather than hit it.
AUTORTFM_DISABLE bool LocusContains(const Verse::SLocus& Range, uint32 Row, uint32 Column)
{
    const uLang::STextPosition Position{Row, Column};
    return Range.GetBegin() <= Position && Position < Range.GetEnd();
}

/// True for the node types whose locus spans a whole definition rather than just a reference to
/// one. They resolve, but only against their name -- see NarrowedLocus.
AUTORTFM_DISABLE bool IsDefinitionNode(uLang::EAstNodeType NodeType)
{
    using namespace uLang;
    return NodeType == EAstNodeType::Definition_Function
        || NodeType == EAstNodeType::Definition_Data
        || NodeType == EAstNodeType::Definition_TypeAlias
        || NodeType == EAstNodeType::Definition_Class
        || NodeType == EAstNodeType::Definition_Enum;
}

/// The Vst node carrying just the *name* of a definition, given the node carrying the whole thing.
///
/// Everything else a declaration is made of describes something other than the definition itself:
/// `<public>` and `<override>` are specifiers, `:float` is a type, `(Delta:float)` is a list of
/// other definitions. Only the name means "this one". Narrowing to the definition's first child is
/// not enough -- for `PhysicsProcess<override>(Delta:float):void` that child still spans the
/// specifier, the parameters and the return type, which is why hovering any of them used to
/// describe the method.
///
/// The name is at the leading edge, so this descends first children: a TypeSpec's is what is being
/// typed, a PrePostCall's is the callee, and both bottom out at the Identifier. An Identifier's own
/// locus excludes its attributes, which hang off its Aux rather than its children.
AUTORTFM_DISABLE const Verse::Vst::Node* DefinitionNameNode(const Verse::Vst::Node& Vst)
{
    const Verse::Vst::Node* Node = &Vst;
    // Bounded rather than while(true): a malformed tree must not spin here.
    for (int32 Depth = 0; Depth < 8; ++Depth)
    {
        if (Node->IsA<Verse::Vst::Identifier>())
        {
            return Node;
        }
        if (Node->GetChildCount() == 0)
        {
            return nullptr;
        }
        Node = &*Node->GetChildren()[0];
    }
    return nullptr;
}

/// A definition's own locus runs from its first attribute to the end of its body, so matching the
/// cursor against it would resolve every blank column inside a function to that function. Only its
/// name is tight enough to mean the author pointed at it.
AUTORTFM_DISABLE const Verse::Vst::Node* NarrowedLocus(const uLang::CAstNode& AstNode, const Verse::Vst::Node& Vst)
{
    using namespace uLang;
    if (!IsDefinitionNode(AstNode.GetNodeType()))
    {
        return &Vst;
    }

    // A class and an enum are the two whose name is not inside their node at all. A function or a
    // data member maps to the whole `Name := ...`, so the name is down its first children; a
    // class maps to the `class(area2d):` macro on the *right* of the `:=`, whose first child is
    // the word `class`. Narrowing to that made `class` the hoverable thing on the line and left
    // the name that was actually being declared resolving to nothing.
    //
    // The name is the enclosing Vst::Definition's left operand. The walk up is bounded rather
    // than a single step because the macro is wrapped: the shape is Definition -> Clause ->
    // Macro, and a `where` clause or a parenthesised form could add another.
    if (AstNode.GetNodeType() == EAstNodeType::Definition_Class
        || AstNode.GetNodeType() == EAstNodeType::Definition_Enum)
    {
        for (const Verse::Vst::Node* Parent = Vst.GetParent(); Parent != nullptr; Parent = Parent->GetParent())
        {
            if (Parent->IsA<Verse::Vst::Definition>())
            {
                return DefinitionNameNode(*static_cast<const Verse::Vst::Definition*>(Parent)->GetOperandLeft());
            }
            if (Parent->IsA<Verse::Vst::Snippet>())
            {
                break;
            }
        }
        return nullptr;
    }

    return Vst.GetChildCount() > 0 ? DefinitionNameNode(*Vst.GetChildren()[0]) : nullptr;
}

/// The definition an identifier node resolves to, or null for a node that is not one.
AUTORTFM_DISABLE const uLang::CDefinition* ReferencedDefinition(const uLang::CAstNode& AstNode,
                                                                const uLang::CSemanticProgram& Program,
                                                                vh_lookup_kind& OutKind)
{
    using namespace uLang;

    switch (AstNode.GetNodeType())
    {
    // A definition is its own best answer at its name: hovering `Ready` where it is declared
    // should describe Ready, not decline because nothing refers to it there.
    case EAstNodeType::Definition_Function:
        OutKind = VH_LOOKUP_FUNCTION;
        return &*static_cast<const CExprFunctionDefinition&>(AstNode)._Function;

    case EAstNodeType::Definition_Data:
        OutKind = VH_LOOKUP_DATA;
        return &*static_cast<const CExprDataDefinition&>(AstNode)._DataMember;

    case EAstNodeType::Definition_TypeAlias:
        OutKind = VH_LOOKUP_TYPE_ALIAS;
        return &*static_cast<const CExprTypeAliasDefinition&>(AstNode)._TypeAlias;

    // `player := class(area2d)` is the first line of every script, and hovering its name resolved
    // nothing at all until this was here -- the class was reachable from every *other* file and
    // silent in its own. _Generalized is what Identifier_Class answers with too, so a reference
    // and the declaration report the same definition.
    case EAstNodeType::Definition_Class:
        OutKind = VH_LOOKUP_CLASS;
        return static_cast<const CExprClassDefinition&>(AstNode)._Class._Generalized;

    case EAstNodeType::Definition_Enum:
        OutKind = VH_LOOKUP_ENUM;
        return &static_cast<const CExprEnumDefinition&>(AstNode)._Enum;

    // An enumerator, which is a definition of its own rather than a member of the enumeration's
    // scope reached by name: `packed_scene_gen_edit_state.Disabled` resolved the enumeration and
    // then nothing for the half the author was actually pointing at.
    case EAstNodeType::Literal_Enum:
        if (const CEnumerator* Enumerator = static_cast<const CExprEnumLiteral&>(AstNode)._Enumerator)
        {
            OutKind = VH_LOOKUP_DATA;
            return Enumerator;
        }
        return nullptr;

    case EAstNodeType::Identifier_Data:
        OutKind = VH_LOOKUP_DATA;
        return &static_cast<const CExprIdentifierData&>(AstNode)._DataDefinition;

    case EAstNodeType::Identifier_Function:
        OutKind = VH_LOOKUP_FUNCTION;
        return &static_cast<const CExprIdentifierFunction&>(AstNode)._Function;

    case EAstNodeType::Identifier_OverloadedFunction:
        // No overload has been picked, and every candidate shares the name the author clicked.
        // The first is a better answer than refusing to resolve at all.
        for (const CFunction* Function : static_cast<const CExprIdentifierOverloadedFunction&>(AstNode)._FunctionOverloads)
        {
            if (Function)
            {
                OutKind = VH_LOOKUP_FUNCTION;
                return Function;
            }
        }
        return nullptr;

    case EAstNodeType::Identifier_Class:
        if (const CClass* Class = static_cast<const CExprIdentifierClass&>(AstNode).GetClass(Program))
        {
            OutKind = VH_LOOKUP_CLASS;
            return Class->_Generalized;
        }
        return nullptr;

    case EAstNodeType::Identifier_Enum:
        if (const CEnumeration* Enumeration = static_cast<const CExprEnumerationType&>(AstNode).GetEnumeration(Program))
        {
            OutKind = VH_LOOKUP_ENUM;
            return Enumeration;
        }
        return nullptr;

    case EAstNodeType::Identifier_TypeAlias:
        OutKind = VH_LOOKUP_TYPE_ALIAS;
        return &static_cast<const CExprIdentifierTypeAlias&>(AstNode)._TypeAlias;

    case EAstNodeType::Identifier_Module:
        if (const CModule* Module = static_cast<const CExprIdentifierModule&>(AstNode).GetModule(Program))
        {
            OutKind = VH_LOOKUP_MODULE;
            return Module;
        }
        return nullptr;

    case EAstNodeType::Identifier_ModuleAlias:
        OutKind = VH_LOOKUP_MODULE;
        return &static_cast<const CExprIdentifierModuleAlias&>(AstNode)._ModuleAlias;

    default:
        return nullptr;
    }
}

struct AUTORTFM_DISABLE FLookupVisitor : public uLang::SAstVisitor
{
    FLookupVisitor(const uLang::CSemanticProgram& InProgram, const FUtf8String& InPath, uint32 InRow, uint32 InColumn)
        : Program(InProgram)
        , Path(InPath)
        , Row(InRow)
        , Column(InColumn)
    {
    }

    virtual void Visit(const char* FieldName, uLang::CAstNode& AstNode) override { VisitElement(AstNode); }

    virtual void VisitElement(uLang::CAstNode& AstNode) override
    {
        if (const Verse::Vst::Node* Vst = AstNode.GetMappedVstNode())
        {
            const Verse::Vst::Node* Locus = NarrowedLocus(AstNode, *Vst);
            if (Locus != nullptr && LocusContains(Locus->Whence(), Row, Column)
                && FULangConversionUtils::ULangStrToFUtf8String(Vst->GetSnippetPath()).Equals(Path, ESearchCase::IgnoreCase))
            {
                vh_lookup_kind Kind = VH_LOOKUP_UNKNOWN;
                if (const uLang::CDefinition* Definition = ReferencedDefinition(AstNode, Program, Kind))
                {
                    // A child's locus is contained in its parent's, so the deepest node visited
                    // that still contains the cursor is the innermost -- last write wins.
                    Found = Definition;
                    FoundKind = Kind;
                    bFoundIsDefinition = IsDefinitionNode(AstNode.GetNodeType());
                }
            }

            // A parameter is not in the tree this walks: analysis moves it onto the function's
            // signature, leaving only its type behind in the AST. So it is asked of the function
            // whose declaration the cursor is inside, which is also the only place it can be --
            // and it is asked after the walk above, so it wins over the enclosing function.
            if (AstNode.GetNodeType() == uLang::EAstNodeType::Definition_Function
                && LocusContains(Vst->Whence(), Row, Column)
                && FULangConversionUtils::ULangStrToFUtf8String(Vst->GetSnippetPath()).Equals(Path, ESearchCase::IgnoreCase))
            {
                const uLang::CFunction& Function = *static_cast<const uLang::CExprFunctionDefinition&>(AstNode)._Function;
                for (const uLang::CDataDefinition* Param : Function._Signature.GetParams())
                {
                    const uLang::CExpressionBase* ParamAst = Param ? Param->GetAstNode() : nullptr;
                    const Verse::Vst::Node* ParamVst = ParamAst ? ParamAst->GetMappedVstNode() : nullptr;
                    const Verse::Vst::Node* ParamName = ParamVst ? DefinitionNameNode(*ParamVst) : nullptr;
                    if (ParamName != nullptr && LocusContains(ParamName->Whence(), Row, Column))
                    {
                        Found = Param;
                        FoundKind = VH_LOOKUP_DATA;
                        bFoundIsDefinition = true;
                        break;
                    }
                }
            }
        }
        AstNode.VisitChildren(*this);
    }

    const uLang::CSemanticProgram& Program;
    FUtf8String Path;
    uint32 Row;
    uint32 Column;
    const uLang::CDefinition* Found{nullptr};
    vh_lookup_kind FoundKind{VH_LOOKUP_UNKNOWN};
    bool bFoundIsDefinition{false};
};

/// Whether a definition is one of its enclosing function's parameters. Locals live in a control
/// scope rather than the function scope, so the scope kind alone nearly answers it -- but the
/// signature is asked directly, because "nearly" is how a `where` clause's type variable would end
/// up documented as an argument.
AUTORTFM_DISABLE bool IsFunctionParameter(const uLang::CDefinition& Definition)
{
    const uLang::CDataDefinition* Data = Definition.AsNullable<uLang::CDataDefinition>();
    if (!Data || Definition._EnclosingScope.GetKind() != uLang::CScope::EKind::Function)
    {
        return false;
    }
    const uLang::CFunction& Function = static_cast<const uLang::CFunction&>(Definition._EnclosingScope);
    for (const uLang::CDataDefinition* Param : Function._Signature.GetParams())
    {
        if (Param == Data)
        {
            return true;
        }
    }
    return false;
}

} // namespace

AUTORTFM_DISABLE TResult<FLookupDesc> LookupSymbol(FUtf8StringView Path, int32 Line, int32 Column)
{
    // A position is not a question a snapshot can answer -- the loci live in the AST, which the
    // worker is rebuilding -- so this declines rather than waits.
    if (IsBackgroundCheckRunning())
    {
        return EHostFailure::AnalysisRunning;
    }
    if (!ProgramIsAnalysisOnly())
    {
        return EHostFailure::BuiltSinceAnalysis;
    }
    if (Line < 0 || Column < 0)
    {
        return EHostFailure::InvalidPosition;
    }
    // Past the gates above an analysis has run, so a program with no AST is one whose parse lost the
    // whole file: nothing is there, and asking for the same analysis again would not change that.
    uLang::CSemanticProgram* const Program = CurrentSemanticProgram();
    if (!Program || !Program->_AstProject)
    {
        return EHostFailure::NothingAtPosition;
    }

    FLookupDesc OutDesc;

    const FUtf8String ProjectVersePath(ScriptVersePath);
    FLookupVisitor Visitor(*Program, FUtf8String(Path), (uint32)Line, (uint32)Column);
    for (const uLang::CAstCompilationUnit* CompilationUnit : Program->_AstProject->OrderedCompilationUnits())
    {
        for (const uLang::CAstPackage* Package : CompilationUnit->Packages())
        {
            // The project's package and nothing else. The cursor is in a file the editor has open,
            // and every res:// file is added to the package at ScriptVersePath -- so the walk that
            // used to test `_VerseScope` was walking the whole 4.3 MB mirror as well, which sets
            // that scope too (AddAttributePackage, VerseHost.Build.cs' SetupVerse). Same fix and
            // same reason as the completion walk beneath this one.
            //
            // What the cursor *resolves to* is not narrowed by this: the definition the visitor
            // finds is whatever the reference names, in whatever package declares it.
            if (FUtf8String(Package->_VersePath.AsCString()) != ProjectVersePath
                || !Package->_RootModule || !Package->_RootModule->GetAstPackage())
            {
                continue;
            }
            Package->_RootModule->GetAstPackage()->VisitChildren(Visitor);
        }
    }

    if (!Visitor.Found)
    {
        return EHostFailure::NothingAtPosition;
    }

    const uLang::CDefinition& Definition = *Visitor.Found;
    OutDesc.Name = FUtf8String(Definition.AsNameCString());
    OutDesc.Kind = Visitor.FoundKind;
    OutDesc.Owner = OwnerNameOf(Definition);

    if (const uLang::CDataDefinition* Data = Definition.AsNullable<uLang::CDataDefinition>())
    {
        OutDesc.bIsVar = Data->IsVar();
        if (const uLang::CTypeBase* Type = Data->GetType())
        {
            OutDesc.Type = FULangConversionUtils::ULangStrToFUtf8String(Type->AsCode());
        }
    }
    else if (const uLang::CFunction* Function = Definition.AsNullable<uLang::CFunction>())
    {
        if (const uLang::CFunctionType* Type = Function->_Signature.GetFunctionType())
        {
            OutDesc.Type = FULangConversionUtils::ULangStrToFUtf8String(Type->AsCode());
        }
    }
    else if (const uLang::CEnumerator* Enumerator = Definition.AsNullable<uLang::CEnumerator>())
    {
        // An enumerator is neither of the two above, so it would carry no type at all -- and the
        // consumer draws the type and nothing else for anything it cannot name a Godot page for.
        // The enumeration it belongs to is the only thing worth saying about one.
        if (Enumerator->_Enumeration)
        {
            OutDesc.Type = FULangConversionUtils::ULangStrToFUtf8String(Enumerator->_Enumeration->AsCode());
        }
    }

    OutDesc.bIsParameter = IsFunctionParameter(Definition);

    // A parameter's comment is read here too, unlike before. DocOf reads the prefix comments off
    // the definition's own VST node, so a parameter gets the comment written against *it* -- the
    // inline `<# doc #> P:t`, or a `#` line above it on its own line -- and not the function's.
    // The consumer cannot do this: it re-reads the source by line, and for a parameter on the same
    // line as the function declaration "the comment above" is the function's, so the host is the
    // only reader that can tell them apart (B41).
    OutDesc.Doc = DocOf(Definition, *Program);

    FillLocation(Definition, OutDesc.Path, OutDesc.Line, OutDesc.Column);

    // Only at a declaration. A call site already resolves to the implementation that will run,
    // and redirecting that to the parent would be wrong rather than merely unhelpful.
    OutDesc.bIsDefinition = Visitor.bFoundIsDefinition;
    if (OutDesc.bIsDefinition)
    {
        if (const uLang::CDefinition* Overridden = Definition.GetOverriddenDefinition())
        {
            OutDesc.OverriddenOwner = OwnerNameOf(*Overridden);
            OutDesc.OverriddenDoc = DocOf(*Overridden, *Program);
            FillLocation(*Overridden, OutDesc.OverriddenPath, OutDesc.OverriddenLine, OutDesc.OverriddenColumn);
        }
    }
    return MoveTemp(OutDesc);
}

namespace {

/// Everything the completion walk needs out of one pass over the AST.
struct AUTORTFM_DISABLE FCompletionVisitor : public uLang::SAstVisitor
{
    FCompletionVisitor(const FUtf8String& InPath, uint32 InRow, uint32 InColumn, const uLang::CScope* InDefaultScope)
        : Path(InPath)
        , Row(InRow)
        , Column(InColumn)
        , Scope(InDefaultScope)
    {
    }

    virtual void Visit(const char* FieldName, uLang::CAstNode& AstNode) override { VisitElement(AstNode); }

    virtual void VisitElement(uLang::CAstNode& AstNode) override
    {
        using namespace uLang;

        // The file's own scope, which is where a cursor inside no definition at all completes --
        // an attribute written above a class, most of all. A `using` is added to whatever scope
        // was open when it was analysed, and at the top of a file that is the snippet, so a
        // position defaulted to the package's root module instead sees none of the Godot API:
        // not `node2d`, not `Print`, and not the `global_class` the line is reaching for.
        //
        // Set before the narrowing below rather than beside it: a class or function containing
        // the cursor still wins, because the walk reaches it after its enclosing snippet.
        if (AstNode.GetNodeType() == EAstNodeType::Context_Snippet)
        {
            const CExprSnippet& Snippet = static_cast<const CExprSnippet&>(AstNode);
            if (Snippet._SemanticSnippet
                && FULangConversionUtils::ULangStrToFUtf8String(Snippet._Path).Equals(Path, ESearchCase::IgnoreCase))
            {
                bSawPath = true;
                Scope = Snippet._SemanticSnippet;
            }
        }

        const Verse::Vst::Node* Vst = AstNode.GetMappedVstNode();
        if (Vst && FULangConversionUtils::ULangStrToFUtf8String(Vst->GetSnippetPath()).Equals(Path, ESearchCase::IgnoreCase))
        {
            bSawPath = true;
            const bool bContainsCursor = LocusContains(Vst->Whence(), Row, Column);

            // The scope to complete in, and the one to test accessibility against. A definition's
            // locus spans its whole body, so this is the enclosing declaration rather than the
            // thing under the cursor -- the opposite of the narrowing the lookup wants.
            if (bContainsCursor)
            {
                if (AstNode.GetNodeType() == EAstNodeType::Definition_Function)
                {
                    Scope = &*static_cast<const CExprFunctionDefinition&>(AstNode)._Function;
                    Locals.Empty();
                }
                else if (AstNode.GetNodeType() == EAstNodeType::Definition_Class)
                {
                    Scope = &static_cast<const CExprClassDefinition&>(AstNode)._Class;
                    Locals.Empty();
                }
            }

            // Anything declared earlier in the enclosing definition. Block scoping is not
            // consulted: a local from a sibling `if` branch is offered too, which over-offers
            // rather than hiding the name the author is reaching for.
            if (AstNode.GetNodeType() == EAstNodeType::Definition_Data
                && (Vst->Whence().BeginRow() < Row || (Vst->Whence().BeginRow() == Row && Vst->Whence().BeginColumn() < Column)))
            {
                Locals.AddUnique(&*static_cast<const CExprDataDefinition&>(AstNode)._DataMember);
            }

            // The receiver of a `.`. Definition nodes span their bodies and so would swallow any
            // cursor inside one; an error node's type is unknown by construction -- but its
            // analysed children survive it, which is exactly what lets a half-typed member still
            // name a receiver.
            //
            // Two candidates, because innermost-wins is the wrong rule on its own and is still the
            // right fallback. The position asked about is the receiver's *last byte*, so the node
            // meant is the one whose locus ends just past it -- and for a call, the invocation and
            // its argument clause both end at the `)`. Innermost-wins picks the argument, whose
            // type is the tuple that was passed: `GetInputSingleton().` offered nothing at all,
            // and so did every other member access on a call's result. The walk is outermost
            // first, so the first node ending at the cursor is the outermost one.
            if (bContainsCursor && IsReceiverCandidate(AstNode.GetNodeType()))
            {
                Expr = &static_cast<const CExpressionBase&>(AstNode);
                if (!EndsAtCursor && Vst->Whence().GetEnd() == uLang::STextPosition{Row, Column + 1})
                {
                    EndsAtCursor = &static_cast<const CExpressionBase&>(AstNode);
                }
            }
        }
        AstNode.VisitChildren(*this);
    }

    /// Everything but the containing contexts, the definitions and the error node is a
    /// CExpressionBase, and the visitor only ever sees one node type per class.
    static bool IsReceiverCandidate(uLang::EAstNodeType NodeType)
    {
        using namespace uLang;
        return !IsDefinitionNode(NodeType)
            && NodeType != EAstNodeType::Error_
            && NodeType != EAstNodeType::Context_Project
            && NodeType != EAstNodeType::Context_CompilationUnit
            && NodeType != EAstNodeType::Context_Package
            && NodeType != EAstNodeType::Context_Snippet
            && NodeType != EAstNodeType::Definition_Module
            && NodeType != EAstNodeType::Definition_Enum
            && NodeType != EAstNodeType::Definition_Class;
    }

    FUtf8String Path;
    uint32 Row;
    uint32 Column;
    const uLang::CExpressionBase* Expr{nullptr};
    const uLang::CExpressionBase* EndsAtCursor{nullptr};
    const uLang::CScope* Scope{nullptr};
    TArray<const uLang::CDataDefinition*> Locals;
    /// Whether this package holds the file at all. Without it the default scope would make every
    /// other package answer with its own root module.
    bool bSawPath{false};

    /// The expression a `.` at the asked-about position hangs off, or null.
    const uLang::CExpressionBase* Receiver() const { return EndsAtCursor ? EndsAtCursor : Expr; }
};

/// Strips the wrappers a value picks up on its way out of a `var` member or an `option`, none of
/// which have members of their own. `Position` reads as `^vector2`; what has an `X` is vector2.
AUTORTFM_DISABLE const uLang::CNormalType* UnwrapToMemberBearingType(const uLang::CTypeBase* Type)
{
    using namespace uLang;
    if (!Type)
    {
        return nullptr;
    }
    const CNormalType* Normal = &Type->GetNormalType();
    for (int32 Depth = 0; Depth < 8; ++Depth)
    {
        if (const CPointerType* Pointer = Normal->AsNullable<CPointerType>())
        {
            Normal = &Pointer->PositiveValueType()->GetNormalType();
        }
        else if (const CReferenceType* Reference = Normal->AsNullable<CReferenceType>())
        {
            Normal = &Reference->PositiveValueType()->GetNormalType();
        }
        else if (const COptionType* Option = Normal->AsNullable<COptionType>())
        {
            Normal = &Option->GetValueType()->GetNormalType();
        }
        else
        {
            return Normal;
        }
    }
    return Normal;
}

/// The byte offset of a 1-based row and utf8 byte column in Text, or -1 when the text is shorter
/// than that. uLang's diagnostic locus counts both from one; everything else here counts from zero.
AUTORTFM_DISABLE int32 OffsetOfRowColumn(const FUtf8String& Text, int32 Row, int32 Column)
{
    if (Row < 1 || Column < 1)
    {
        return -1;
    }
    int32 Offset = 0;
    for (int32 Line = 1; Line < Row; ++Line)
    {
        while (Offset < Text.Len() && Text[Offset] != UTF8CHAR('\n'))
        {
            ++Offset;
        }
        if (Offset >= Text.Len())
        {
            return -1;
        }
        ++Offset;
    }
    const int32 Result = Offset + Column - 1;
    return Result < Text.Len() ? Result : -1;
}

} // namespace

AUTORTFM_DISABLE FUtf8String SubjectTypeOfDiagnostic(FUtf8StringView Path, int32 Row, int32 Column)
{
    using namespace uLang;

    // A name with a receiver needs at least a receiver byte and a dot in front of it, so a column
    // below three cannot be one and the arithmetic below would run off the start of the line.
    if (Column < 3)
    {
        return FUtf8String();
    }

    // Whether there is a receiver at all is a question about the text, and the text is here: the
    // snippets the analysis ran over are the host's own. Asking the AST instead would mean trusting
    // whatever expression happens to sit two bytes before a bare unresolved identifier.
    FUtf8String Text;
    if (!ScriptSnippetText(Path, Text))
    {
        return FUtf8String();
    }
    const int32 NameOffset = OffsetOfRowColumn(Text, Row, Column);
    if (NameOffset < 2 || Text[NameOffset - 1] != UTF8CHAR('.'))
    {
        return FUtf8String();
    }

    CSemanticProgram* const Program = CurrentSemanticProgram();
    if (!Program || !Program->_AstProject)
    {
        return FUtf8String();
    }

    // The receiver's last byte, which is the position member completion asks about for the very
    // same expression -- and the reason this works at all: the analyzer replaces a failed member
    // access with an error node and hangs the *analysed* receiver under it, so the receiver's type
    // outlives the error that named it.
    const FUtf8String ProjectVersePath(ScriptVersePath);
    for (const CAstCompilationUnit* CompilationUnit : Program->_AstProject->OrderedCompilationUnits())
    {
        for (const CAstPackage* Package : CompilationUnit->Packages())
        {
            if (FUtf8String(Package->_VersePath.AsCString()) != ProjectVersePath
                || !Package->_RootModule || !Package->_RootModule->GetAstPackage())
            {
                continue;
            }
            FCompletionVisitor Visitor(FUtf8String(Path), (uint32)(Row - 1), (uint32)(Column - 3), Package->_RootModule);
            Package->_RootModule->GetAstPackage()->VisitChildren(Visitor);
            if (!Visitor.bSawPath || !Visitor.Receiver())
            {
                continue;
            }
            const CNormalType* Type = UnwrapToMemberBearingType(Visitor.Receiver()->GetResultType(*Program));
            // A receiver written as a type name -- `node_process_mode.Inherit` -- resolves to the
            // type of types, wrapping the one the message would have named.
            if (const CTypeType* TypeType = Type ? Type->AsNullable<CTypeType>() : nullptr)
            {
                Type = TypeType->PositiveType() ? UnwrapToMemberBearingType(TypeType->PositiveType()) : nullptr;
            }
            if (Type)
            {
                return FULangConversionUtils::ULangStrToFUtf8String(Type->AsCode());
            }
        }
    }
    return FUtf8String();
}

namespace {

/// Everything a function's declaration spells after its name, as Verse source:
/// "(Delta:float)<transacts>:void". The function type cannot stand in for it -- the parameter
/// names live on the signature, and the type spells the same definition "float->void".
///
/// Effects come out relative to the function default, which is why an ordinary method's
/// signature carries no specifier at all: BuildEffectAttributeCode emits only what an author
/// would have had to write.
///
/// SkipLeadingParams drops the receiver off an extension method's signature: `_Signature` carries
/// it as parameter 0 (`(V:vector2).Length()` parses "(V:vector2)" as the first parameter clause),
/// but the call an author writes is `V.Length()`, and the receiver is not part of that spelling.
AUTORTFM_DISABLE FUtf8String SpellSignature(const uLang::CFunction& Function, int32 SkipLeadingParams = 0)
{
    const uLang::CFunctionType* Type = Function._Signature.GetFunctionType();
    if (!Type)
    {
        return FUtf8String();
    }

    uLang::CUTF8StringBuilder Builder;
    Builder.Append('(');
    const char* Separator = "";
    int32 ParamIndex = 0;
    for (const uLang::CDataDefinition* Param : Function._Signature.GetParams())
    {
        if (ParamIndex++ < SkipLeadingParams)
        {
            continue;
        }
        if (!Param || !Param->GetType())
        {
            continue;
        }
        Builder.Append(Separator);
        Separator = ", ";
        Builder.Append(Param->AsNameCString());
        Builder.Append(':');
        Builder.Append(Param->GetType()->AsCode());
    }
    Builder.Append(')');
    Type->BuildEffectAttributeCode(Builder);
    Builder.Append(':');
    Builder.Append(Type->GetReturnType().AsCode());
    return FULangConversionUtils::ULangStrToFUtf8String(Builder.MoveToString());
}

/// Whether a subclass could redeclare a function with <override>, which is the three things the
/// analyzer refuses one for: it has to be a class member -- a module-level function has nothing to
/// override it from -- it must not be <final>, and it must not be a class var's <getter>/<setter>,
/// which DetectIncorrectOverrideAttribute rejects by name. The generated Godot mirror is built out
/// of those accessors, so leaving the last one out offers a few hundred overrides that do not
/// compile.
AUTORTFM_DISABLE bool IsOverridable(const uLang::CFunction& Function, bool bIsClassVarAccessor)
{
    if (Function._EnclosingScope.GetKind() != uLang::CScope::EKind::Class || bIsClassVarAccessor)
    {
        return false;
    }
    const uLang::CSemanticProgram& Program = Function._EnclosingScope.GetProgram();
    return !Function.GetPrototypeDefinition()->HasAttributeClass(Program._finalClass, Program);
}

/// Which of a scope's definitions the walk keeps.
enum class ECompleteFilter : uint8
{
    /// Every name the scope admits.
    Any,
    /// Only what may follow an `@`.
    PrefixAttributes,
    /// Only what may follow a `<`.
    Specifiers,
    /// Only what may stand where a type is expected.
    Types,
    /// Only what may stand in a class header's parentheses.
    Supertypes,
    /// Only what a `set` may assign to.
    Assignable,
    /// Only a data member, var or not, which is what an archetype body may give a value.
    Fields,
};

/// Whether an attribute class may be written in the position Filter names.
///
/// Verse keeps the two positions apart and refuses the wrong one: a class tagged
/// `@attribscope_specifier` "can only be used as a <specifier>" and one tagged
/// `@attribscope_attribute` "can only be used as an @attribute"
/// (SemanticAnalyzer.cpp, ErrSemantic_InvalidAttributeScope). A user-defined attribute carries
/// neither tag, and the analyzer's own comment says there is no way yet to signal which it is, so
/// it is accepted in both -- which is what the fallthrough below reproduces.
AUTORTFM_DISABLE bool AttributeClassFitsPosition(const uLang::CClass& Class,
                                                 const uLang::CSemanticProgram& Program,
                                                 ECompleteFilter Filter)
{
    const uLang::CDefinition& Prototype = PrototypeOf(Class);
    if (Prototype.HasAttributeClass(Program._attributeScopeSpecifier, Program))
    {
        return Filter == ECompleteFilter::Specifiers;
    }
    if (Prototype.HasAttributeClass(Program._attributeScopeAttribute, Program))
    {
        return Filter == ECompleteFilter::PrefixAttributes;
    }
    return true;
}

/// Whether a definition is a name an `@` or a `<` could be followed by, for whichever of the two
/// Filter names.
///
/// Two shapes, because Verse spells a payload-carrying attribute as a call: `editable` is the
/// attribute class itself, while `@clamp_min("0.0")` names the `<constructor>` function beside
/// clamp_min_attribute, which is what makes the class out of the argument. `attribute` itself is
/// excluded -- it is the base every attribute derives from, and applying it means nothing.
AUTORTFM_DISABLE bool IsAttributeName(const uLang::CDefinition& Definition, ECompleteFilter Filter)
{
    using namespace uLang;

    const CSemanticProgram& Program = Definition._EnclosingScope.GetProgram();
    const CClass* AttributeClass = Program._attributeClass;
    if (!AttributeClass)
    {
        return false;
    }

    if (const CClass* Class = Definition.AsNullable<CClass>())
    {
        return Class != AttributeClass && Class->IsSubtypeOf(*AttributeClass)
            && AttributeClassFitsPosition(*Class, Program, Filter);
    }
    if (const CFunction* Function = Definition.AsNullable<CFunction>())
    {
        if (!PrototypeOf(*Function).AsChecked<CFunction>().IsConstructor())
        {
            return false;
        }
        const CFunctionType* Type = Function->_Signature.GetFunctionType();
        const CClass* Result = Type ? Type->GetReturnType().GetNormalType().AsNullable<CClass>() : nullptr;
        return Result && Result->IsSubtypeOf(*AttributeClass)
            && AttributeClassFitsPosition(*Result, Program, Filter);
    }
    return false;
}

/// Whether a definition is a name the position Filter describes will accept.
///
/// The positions that narrow are the ones Verse gives a single reading: a type after a `:`, a
/// superclass in a class header, the target of a `set`. Nothing here is about what *resolves* --
/// every one of these definitions is in scope either way -- only about what the compiler would
/// take, which is the difference between a list worth reading and 2773 names.
AUTORTFM_DISABLE bool DefinitionFitsFilter(const uLang::CDefinition& Definition, ECompleteFilter Filter)
{
    using namespace uLang;

    switch (Filter)
    {
    case ECompleteFilter::Any:
        return true;

    case ECompleteFilter::PrefixAttributes:
    case ECompleteFilter::Specifiers:
        return IsAttributeName(Definition, Filter);

    case ECompleteFilter::Types:
        // A module qualifies one -- `/Godot.org/Godot.node2d` -- so it belongs to a type position
        // even though it is not itself a type.
        return Definition.AsNullable<CClass>() || Definition.AsNullable<CEnumeration>()
            || Definition.AsNullable<CTypeAlias>() || Definition.AsNullable<CModule>()
            || Definition.AsNullable<CModuleAlias>();

    case ECompleteFilter::Supertypes:
        // An interface is a CClass here, so `class(a, b)` and a superclass chain are the same test.
        // An enum or an alias to a primitive is not something a class can derive from, which is
        // two thirds of what a type position admits and the whole reason this is its own filter.
        return Definition.AsNullable<CClass>() || Definition.AsNullable<CModule>()
            || Definition.AsNullable<CModuleAlias>();

    case ECompleteFilter::Assignable:
    {
        // `set` needs a mutable place. A non-var member is assigned once at construction, so
        // offering it is offering a compile error -- which is most of what a class body declares.
        const CDataDefinition* Data = Definition.AsNullable<CDataDefinition>();
        return Data && Data->IsVar();
    }

    case ECompleteFilter::Fields:
        // Deliberately wider than Assignable: construction is the one moment a non-var field is
        // written, so `vector2{X := 1.0}` sets a field no `set` could ever reach.
        return Definition.AsNullable<CDataDefinition>() != nullptr;
    }
    return false;
}

/// Fills one item from a definition, or returns false for a definition that is not a name the
/// author could have written: the compiler generates a constructor and an archetype per class,
/// and neither is spellable.
AUTORTFM_DISABLE bool DescribeCompletion(const uLang::CDefinition& Definition, ECompleteFilter Filter, GodotVerse::FCompleteItem& OutItem)
{
    using namespace uLang;

    if (Definition.GetName().IsNull())
    {
        return false;
    }

    // Asked once for the two answers it settles, since building its key spells the whole function
    // type -- and a completion describes a couple of thousand items.
    const FMirrorDefinition* const Recorded = FindMirrorDefinition(Definition);

    if (const CDataDefinition* Data = Definition.AsNullable<CDataDefinition>())
    {
        OutItem.Kind = VH_LOOKUP_DATA;
        OutItem.bIsVar = Data->IsVar();
        if (const CTypeBase* Type = Data->GetType())
        {
            OutItem.Type = FULangConversionUtils::ULangStrToFUtf8String(Type->AsCode());
        }
    }
    else if (const CFunction* Function = Definition.AsNullable<CFunction>())
    {
        // An attribute's `<constructor>` is the one the author writes -- `@clamp_min("0.0")` --
        // and IsAttributeName has already established that this is one. Everywhere else a
        // constructor is the copy the compiler generated per class, which has no spelling.
        if (PrototypeOf(*Function).AsChecked<CFunction>().IsConstructor() && Filter != ECompleteFilter::PrefixAttributes
            && Filter != ECompleteFilter::Specifiers)
        {
            return false;
        }

        // A class var's `<getter>`/`<setter>` is the same kind of unspellable name as the
        // constructor above, and there are four thousand of them: gen_verse_api.py turns each of
        // Godot's 3312 properties into a var plus a `PositionGetter`/`PositionSetter` pair, and
        // both take an `accessor` parameter no script can construct. Only the compiler names one,
        // at the point it rewrites a read or a write of the var. Offered, they crowd out the
        // members an author is reaching for -- every property of every class in the chain, twice.
        const bool bIsClassVarAccessor = IsClassVarAccessor(*Function, Recorded);
        if (bIsClassVarAccessor)
        {
            return false;
        }
        OutItem.Kind = VH_LOOKUP_FUNCTION;
        OutItem.ParamCount = Function->_Signature.NumParams();
        OutItem.Signature = SpellSignature(*Function);
        OutItem.bIsOverridable = IsOverridable(*Function, bIsClassVarAccessor);
        if (const CFunctionType* Type = Function->_Signature.GetFunctionType())
        {
            OutItem.Type = FULangConversionUtils::ULangStrToFUtf8String(Type->AsCode());
        }
    }
    else if (Definition.AsNullable<CClass>())
    {
        OutItem.Kind = VH_LOOKUP_CLASS;
    }
    else if (Definition.AsNullable<CEnumeration>() || Definition.AsNullable<CEnumerator>())
    {
        OutItem.Kind = VH_LOOKUP_ENUM;
    }
    else if (Definition.AsNullable<CTypeAlias>())
    {
        OutItem.Kind = VH_LOOKUP_TYPE_ALIAS;
    }
    else if (Definition.AsNullable<CModule>() || Definition.AsNullable<CModuleAlias>())
    {
        OutItem.Kind = VH_LOOKUP_MODULE;
    }
    else
    {
        return false;
    }

    OutItem.Name = FUtf8String(Definition.AsNameCString());
    OutItem.Owner = OwnerNameOf(Definition, Recorded);
    int32 UnusedColumn = -1;
    FillLocation(Definition, Recorded, OutItem.Path, OutItem.Line, UnusedColumn);
    return true;
}

/// Adds every definition a scope declares that the cursor's scope is allowed to see and Seen does
/// not already hold. Seen is carried across every scope of one query, nearest first, so the copy
/// that survives is the one that would actually resolve -- and the copies that do not are never
/// described, which is where the work is.
AUTORTFM_DISABLE void CollectScope(const uLang::CLogicalScope& From,
                                   const uLang::CScope* AccessFrom,
                                   ECompleteFilter Filter,
                                   int32 Distance,
                                   TSet<FUtf8String>& Seen,
                                   TArray<GodotVerse::FCompleteItem>& OutItems)
{
    for (const uLang::TSRef<uLang::CDefinition>& Definition : From.GetDefinitions())
    {
        if (AccessFrom && !Definition->IsAccessibleFrom(*AccessFrom))
        {
            continue;
        }
        if (!DefinitionFitsFilter(*Definition, Filter))
        {
            continue;
        }
        if (Definition->GetName().IsNull())
        {
            continue;
        }
        // Tested before describing and recorded only after: a definition DescribeCompletion refuses
        // -- a compiler-generated constructor, say -- must not reserve its name against a real
        // definition of the same name further out, which is what the post-hoc deduplication this
        // replaced could not get wrong.
        FUtf8String Name(Definition->AsNameCString());
        if (Seen.Contains(Name))
        {
            continue;
        }
        GodotVerse::FCompleteItem Item;
        if (DescribeCompletion(*Definition, Filter, Item))
        {
            Item.OwnerDistance = Distance;
            Seen.Add(MoveTemp(Name));
            OutItems.Add(MoveTemp(Item));
        }
    }
}

/// A class and everything it inherits. An override is declared in both, so the subclass' copy
/// wins by arriving first and the duplicate is dropped by Seen.
///
/// Distance is what the class itself is worth; every superclass step adds one, which is the number
/// an editor ranks by. An interface counts as a step from the class that lists it, because that is
/// where its members come from as far as anyone reading the subclass is concerned.
AUTORTFM_DISABLE void CollectClassAndSupers(const uLang::CClass& Class,
                                            const uLang::CScope* AccessFrom,
                                            ECompleteFilter Filter,
                                            int32 Distance,
                                            TSet<FUtf8String>& Seen,
                                            TArray<GodotVerse::FCompleteItem>& OutItems)
{
    // An interface is a CClass too, so the same walk covers `class(a, b)` as well as a superclass
    // chain; a diamond is dropped by Seen rather than tracked here.
    int32 Depth = Distance;
    for (const uLang::CClass* Current = &Class; Current; Current = Current->GetSuperClass(), ++Depth)
    {
        CollectScope(*Current, AccessFrom, Filter, Depth, Seen, OutItems);
        for (const uLang::CClass* Interface : Current->_SuperInterfaces)
        {
            if (Interface)
            {
                CollectScope(*Interface, AccessFrom, Filter, Depth + 1, Seen, OutItems);
            }
        }
    }
}

/// Fills one item for an extension method, the way DescribeCompletion fills one for an ordinary
/// member -- same fields, so the client completes `Length()` with its arity and brackets intact.
/// Split out rather than folded into DescribeCompletion because the two disagree on the name (the
/// mangled `operator'.Length'` has to become `Length`) and on the arity (parameter 0 is the
/// receiver, never part of what the author writes).
AUTORTFM_DISABLE bool DescribeExtensionMethodCompletion(const uLang::CFunction& Function, GodotVerse::FCompleteItem& OutItem)
{
    using namespace uLang;

    if (Function.GetName().IsNull())
    {
        return false;
    }

    OutItem.Kind = VH_LOOKUP_FUNCTION;
    OutItem.ParamCount = Function._Signature.NumParams() - 1;
    OutItem.Signature = SpellSignature(Function, /*SkipLeadingParams=*/1);
    OutItem.bIsOverridable = false;
    if (const CFunctionType* Type = Function._Signature.GetFunctionType())
    {
        OutItem.Type = FULangConversionUtils::ULangStrToFUtf8String(Type->AsCode());
    }

    OutItem.Name = ExtensionMethodName(Function);
    OutItem.Owner = FUtf8String(Function._EnclosingScope.GetScopeName().AsCString());
    int32 UnusedColumn = -1;
    FillLocation(Function, OutItem.Path, OutItem.Line, UnusedColumn);
    return true;
}

/// Extension methods are module-level definitions -- `(V:vector2).Length()` declares
/// `operator'.Length'` beside whatever else the module holds -- so a class's own member walk never
/// finds them. This walks the same scopes a bare identifier would resolve against (the cursor's
/// enclosing scopes, outward, and each one's `using` scopes -- which is how `/Godot.org/Godot`'s
/// math extension methods come into view for every script) and offers every extension method whose
/// receiver parameter accepts ReceiverType.
///
/// ReceiverType is a CClass rather than the more general CNormalType a resolved expression type
/// actually is: `void` -- what a position with no real receiver under it resolves to -- sits at the
/// bottom of the subtype lattice and matches every extension method's receiver parameter, which
/// turned "nothing under the cursor" into a wall of unrelated completions. Every type an extension
/// method exists for today is a CClass, so requiring one is not a narrowing an author can feel.
AUTORTFM_DISABLE void CollectExtensionMethods(const uLang::CScope* FromScope,
                                              const uLang::CClass& ReceiverType,
                                              TSet<FUtf8String>& Seen,
                                              TArray<GodotVerse::FCompleteItem>& OutItems)
{
    using namespace uLang;

    auto VisitLogicalScope = [FromScope, &ReceiverType, &Seen, &OutItems](const CLogicalScope& Scope)
    {
        for (const TSRef<CDefinition>& Definition : Scope.GetDefinitions())
        {
            const CFunction* Function = Definition->AsNullable<CFunction>();
            if (!Function || Function->_ExtensionFieldAccessorKind != EExtensionFieldAccessorKind::ExtensionMethod)
            {
                continue;
            }
            if (FromScope && !Definition->IsAccessibleFrom(*FromScope))
            {
                continue;
            }
            const SSignature::ParamDefinitions& Params = Function->_Signature.GetParams();
            if (Params.IsEmpty() || !Params[0] || !Params[0]->GetType())
            {
                continue;
            }
            const CTypeBase* ReceiverParam = Params[0]->GetType();

            // A *parametric* receiver is declared against a type variable -- `(Ev:event(t) where
            // t:type).Emit` -- and `event(int)` is not a subtype of `event(t)`: a parametric class
            // is nominal, so the two are different CClasses and the match below refuses outright.
            // `Emit` and `Subscribe` were the whole visible cost, since they are the only two
            // extension methods the bridge declares that way, and both are what an author reaches
            // for on a declared `event(t)` member.
            //
            // Instantiating is what a call site does: the type variables become fresh flow types,
            // and matching then constrains them the way overload resolution would. Only for a
            // signature that has any, so nothing else pays for it, and the flow types are this
            // call's own -- constraining them is visible to nothing else.
            if (const CFunctionType* FunctionType = Function->_Signature.GetFunctionType())
            {
                if (!FunctionType->GetTypeVariables().IsEmpty())
                {
                    if (const CFunctionType* Instantiated = SemanticTypeUtils::Instantiate(
                            FunctionType, VerseFN::UploadedAtFNVersion::Latest))
                    {
                        const CTypeBase& ParamsType = Instantiated->GetParamsType();
                        const CTupleType* const Tuple = ParamsType.GetNormalType().AsNullable<CTupleType>();
                        ReceiverParam = Tuple && Tuple->Num() > 0 ? (*Tuple)[0] : &ParamsType;
                    }
                }
            }

            // Contravariant: matching against the parameter's own type, the same test overload
            // resolution runs for an extension call, is what lets a subtype receiver still match.
            if (!SemanticTypeUtils::Matches(&ReceiverType, ReceiverParam, VerseFN::UploadedAtFNVersion::Latest))
            {
                continue;
            }
            // Recorded only once described, and tested before, for the same reason CollectScope
            // does it in that order: a definition the describe step refuses must not reserve its
            // name against a real one further out.
            GodotVerse::FCompleteItem Item;
            if (DescribeExtensionMethodCompletion(*Function, Item) && !Seen.Contains(Item.Name))
            {
                Seen.Add(Item.Name);
                OutItems.Add(MoveTemp(Item));
            }
        }
    };

    for (const CScope* Current = FromScope; Current; Current = Current->GetParentScope())
    {
        if (Current->GetKind() == CScope::EKind::Class)
        {
            for (const CClass* ClassCurrent = &static_cast<const CClass&>(*Current); ClassCurrent; ClassCurrent = ClassCurrent->GetSuperClass())
            {
                VisitLogicalScope(*ClassCurrent);
                for (const CClass* Interface : ClassCurrent->_SuperInterfaces)
                {
                    if (Interface)
                    {
                        VisitLogicalScope(*Interface);
                    }
                }
            }
        }
        else
        {
            VisitLogicalScope(Current->GetLogicalScope());
        }
        for (const CLogicalScope* Using : Current->GetUsingScopes())
        {
            if (Using)
            {
                VisitLogicalScope(*Using);
            }
        }
    }
}

/// Why the program the last analysis left cannot answer a position question about SourceText as
/// Path's text, or nothing when it can. Neither completion nor the argument hint analyses a buffer
/// of its own (ABI v7), so a buffer the program does not describe is the caller's to have analysed
/// and ask about again, rather than a stall it never asked for.
AUTORTFM_DISABLE TOptional<EHostFailure> WhyProgramCannotDescribe(FUtf8StringView Path, const FUtf8String& SourceText)
{
    if (IsBackgroundCheckRunning())
    {
        return EHostFailure::AnalysisRunning;
    }
    if (!ProgramDescribes(FUtf8String(Path), SourceText))
    {
        return EHostFailure::BufferNotAnalysed;
    }
    return {};
}

} // namespace

AUTORTFM_DISABLE TResult<TArray<FCompleteItem>> Complete(FUtf8StringView Path,
                                                         const FUtf8String& SourceText,
                                                         int32 Line,
                                                         int32 Column,
                                                         vh_complete_mode Mode)
{
    if (const TOptional<EHostFailure> Declined = WhyProgramCannotDescribe(Path, SourceText))
    {
        return Declined.GetValue();
    }
    if (Line < 0 || Column < 0)
    {
        return EHostFailure::InvalidPosition;
    }

    const double Started = FPlatformTime::Seconds();

    uLang::CSemanticProgram* const Program = CurrentSemanticProgram();
    if (!Program || !Program->_AstProject)
    {
        return EHostFailure::NothingAtPosition;
    }

    TArray<FCompleteItem> OutItems;

    const FUtf8String ProjectVersePath(ScriptVersePath);
    double WalkSeconds = 0.0;

    for (const uLang::CAstCompilationUnit* CompilationUnit : Program->_AstProject->OrderedCompilationUnits())
    {
        for (const uLang::CAstPackage* Package : CompilationUnit->Packages())
        {
            // The project's package and nothing else. `InternalUser` is not the filter it reads as:
            // the mirror and the attribute package are both set to it (AddAttributePackage,
            // VerseHost.Build.cs' SetupVerse), so a scope test walked all 4.3 MB of generated mirror
            // AST before reaching the two snippets that could contain the cursor -- which was the
            // whole of the warm path's ~97 ms. The verse path is exact: every res:// file is added
            // to the package at ScriptVersePath and a position question is only ever about one.
            if (FUtf8String(Package->_VersePath.AsCString()) != ProjectVersePath
                || !Package->_RootModule || !Package->_RootModule->GetAstPackage())
            {
                continue;
            }

            // A file that declares nothing still sits in its package's root module, which is the
            // scope a cursor at the top level of it completes in.
            const double WalkStarted = FPlatformTime::Seconds();
            FCompletionVisitor Visitor(FUtf8String(Path), (uint32)Line, (uint32)Column, Package->_RootModule);
            Package->_RootModule->GetAstPackage()->VisitChildren(Visitor);
            WalkSeconds += FPlatformTime::Seconds() - WalkStarted;
            if (!Visitor.bSawPath)
            {
                continue;
            }

            // A name that is in scope twice -- an override, or a class member shadowing an imported
            // one -- is one completion, and the walk below is ordered nearest-first, so the copy
            // that survives is the one that would actually resolve. Carried into the collection
            // rather than applied to its result because describing an item is the expensive part:
            // a type spelled with AsCode, and a whole signature spelled for every function.
            TSet<FUtf8String> Seen;

            if (Mode == VH_COMPLETE_ARCHETYPE_FIELDS)
            {
                // `vector2{<cursor>}`: the position is the class named before the brace, and what
                // may be written inside is the fields it and its superclasses declare. Methods are
                // deliberately absent -- an archetype body gives values, it does not override.
                //
                // The name resolves to a type rather than to a value, so the CTypeType unwrap the
                // member branch does for `node_process_mode.` is the same one needed here.
                if (!Visitor.Receiver())
                {
                    continue;
                }
                const uLang::CNormalType* Named = UnwrapToMemberBearingType(Visitor.Receiver()->GetResultType(*Program));
                if (const uLang::CTypeType* TypeType = Named ? Named->AsNullable<uLang::CTypeType>() : nullptr)
                {
                    Named = TypeType->PositiveType() ? UnwrapToMemberBearingType(TypeType->PositiveType()) : nullptr;
                }
                const uLang::CClass* Class = Named ? Named->AsNullable<uLang::CClass>() : nullptr;
                if (!Class)
                {
                    continue;
                }
                CollectClassAndSupers(*Class, Visitor.Scope, ECompleteFilter::Fields, 0, Seen, OutItems);
            }
            else if (Mode == VH_COMPLETE_MEMBERS)
            {
                if (!Visitor.Receiver())
                {
                    continue;
                }
                const uLang::CNormalType* Type = UnwrapToMemberBearingType(Visitor.Receiver()->GetResultType(*Program));
                if (!Type)
                {
                    continue;
                }

                // A receiver named as a type -- `node_process_mode.<cursor>` -- has a CTypeType
                // result ("type, the type of types"), wrapping the CEnumeration/CClass rather than
                // being one. Its own FindInstanceMember reaches through to the positive type, so
                // completion has to as well or a type name answers nothing.
                bool bIsTypeName = false;
                if (const uLang::CTypeType* TypeType = Type->AsNullable<uLang::CTypeType>())
                {
                    bIsTypeName = true;
                    Type = TypeType->PositiveType() ? UnwrapToMemberBearingType(TypeType->PositiveType()) : nullptr;
                    if (!Type)
                    {
                        continue;
                    }
                }

                if (const uLang::CClass* Class = Type->AsNullable<uLang::CClass>())
                {
                    // A class named as a type has no members reachable this way: CClass does not
                    // override FindTypeMember, so `SomeClass.` offers nothing in real Verse, and
                    // offering its instance members here would be offering something that does not
                    // compile.
                    if (!bIsTypeName)
                    {
                        CollectClassAndSupers(*Class, Visitor.Scope, ECompleteFilter::Any, 0, Seen, OutItems);

                        // A math value's methods (Length, Normalized, ...) are extension methods,
                        // not class members -- see CollectExtensionMethods for why this needs a
                        // CClass rather than the wider Type.
                        CollectExtensionMethods(Visitor.Scope, *Class, Seen, OutItems);
                    }
                }
                else if (const uLang::CEnumeration* Enumeration = Type->AsNullable<uLang::CEnumeration>())
                {
                    // An enumeration's enumerators are reachable both ways -- `node_process_mode.`
                    // and a value already of that type both resolve here -- because CEnumeration is
                    // its own FindTypeMember target as well as an enum value's ordinary type.
                    CollectScope(*Enumeration, Visitor.Scope, ECompleteFilter::Any, 0, Seen, OutItems);
                }
                else if (const uLang::CModule* Module = Type->AsNullable<uLang::CModule>())
                {
                    if (!bIsTypeName)
                    {
                        CollectScope(*Module, Visitor.Scope, ECompleteFilter::Any, 0, Seen, OutItems);
                    }
                }
            }
            else
            {
                ECompleteFilter Filter = ECompleteFilter::Any;
                switch (Mode)
                {
                case VH_COMPLETE_ATTRIBUTES: Filter = ECompleteFilter::PrefixAttributes; break;
                case VH_COMPLETE_SPECIFIERS: Filter = ECompleteFilter::Specifiers; break;
                case VH_COMPLETE_TYPES: Filter = ECompleteFilter::Types; break;
                case VH_COMPLETE_SUPERTYPES: Filter = ECompleteFilter::Supertypes; break;
                case VH_COMPLETE_ASSIGNABLE: Filter = ECompleteFilter::Assignable; break;
                default: break;
                }

                // A local is a value, so it belongs to a bare identifier and to a `set` target and
                // to neither of the other narrowed positions: an attribute is a type applied to a
                // declaration, and a local is never a type nor a superclass. DefinitionFitsFilter
                // is what drops the locals a `set` cannot take, which is every one that is not var.
                if (Filter == ECompleteFilter::Any || Filter == ECompleteFilter::Assignable)
                {
                    for (const uLang::CDataDefinition* Local : Visitor.Locals)
                    {
                        if (!DefinitionFitsFilter(*Local, Filter))
                        {
                            continue;
                        }
                        FCompleteItem Item;
                        FUtf8String Name(Local->AsNameCString());
                        if (!Seen.Contains(Name) && DescribeCompletion(*Local, Filter, Item))
                        {
                            Item.OwnerDistance = 0;
                            Seen.Add(MoveTemp(Name));
                            OutItems.Add(MoveTemp(Item));
                        }
                    }
                }

                // Out through the enclosing class and its superclasses, then the modules above it,
                // picking up each scope's `using` along the way -- which is where the whole
                // mirrored Godot API enters, since a script reaches it through `using {/Godot.org/Godot}`.
                //
                // One step outward per scope, and a superclass chain counts its own steps within
                // that. A `using` is -1 instead: it is not further out, it is elsewhere, and
                // counting the hop to the scope that carries it would rank all 9597 mirrored
                // methods a step or two from the cursor.
                int32 Distance = 0;
                for (const uLang::CScope* Current = Visitor.Scope; Current; Current = Current->GetParentScope(), ++Distance)
                {
                    if (Current->GetKind() == uLang::CScope::EKind::Class)
                    {
                        CollectClassAndSupers(static_cast<const uLang::CClass&>(*Current), Visitor.Scope, Filter, Distance, Seen, OutItems);
                    }
                    else
                    {
                        CollectScope(Current->GetLogicalScope(), Visitor.Scope, Filter, Distance, Seen, OutItems);
                    }
                    for (const uLang::CLogicalScope* Using : Current->GetUsingScopes())
                    {
                        if (Using)
                        {
                            CollectScope(*Using, Visitor.Scope, Filter, -1, Seen, OutItems);
                        }
                    }
                }
            }

            if (!OutItems.IsEmpty())
            {
                OutItems.Sort([](const FCompleteItem& Left, const FCompleteItem& Right) { return Left.Name < Right.Name; });
                if (AnalysisTraceEnabled())
                {
                    fprintf(stderr,
                            "[vh-trace] complete mode=%d: %.2f ms (%.2f ms ast walk), %d item(s)\n",
                            (int)Mode,
                            (FPlatformTime::Seconds() - Started) * 1000.0,
                            WalkSeconds * 1000.0,
                            OutItems.Num());
                    fflush(stderr);
                }
                return MoveTemp(OutItems);
            }
        }
    }

    return EHostFailure::NothingAtPosition;
}

AUTORTFM_DISABLE void ClassOwnMembers(const uLang::CClass& Class, TArray<FCompleteItem>& OutItems)
{
    OutItems.Empty();

    // The class' own scope only. What it inherits is documented by the class that declares it,
    // and for a mirrored Godot class that is Godot's own documentation rather than anything here.
    //
    // No access scope, which is the one thing here that admits a name a cursor could not write:
    // CollectScope skips IsAccessibleFrom when it is null. That is safe because every
    // `<epic_internal>` name in all four of host/Verse's files is a class var's
    // `<getter>`/`<setter>`, and DescribeCompletion refuses those outright. Adding a non-accessor
    // one means giving this an access scope -- and then deciding whose, since the callers are a
    // completion at a cursor, a script's own documentation and the method outline.
    TSet<FUtf8String> Seen;
    CollectScope(Class, nullptr, ECompleteFilter::Any, 0, Seen, OutItems);
    OutItems.Sort([](const FCompleteItem& Left, const FCompleteItem& Right) { return Left.Name < Right.Name; });
}

AUTORTFM_DISABLE bool ClassMembersLive(FUtf8StringView ClassName, TArray<GodotVerse::FCompleteItem>& OutItems)
{
    OutItems.Empty();

    const uLang::CClass* const Class = FindScriptClassLive(ClassName);
    if (!Class)
    {
        return false;
    }
    ClassOwnMembers(*Class, OutItems);
    return true;
}

/// The inherited half of what Complete answers at a member declaration inside ClassName, kept to
/// the names an <override> could be written for.
///
/// Reuses Complete's own walk rather than repeating it: same CollectClassAndSupers, same
/// DescribeCompletion, and the same access scope a cursor in the class body has -- so an item here
/// is byte for byte the item the refined answer will replace it with, which is what lets the
/// consumer format the two identically.
///
/// OwnMembers is what ClassMembersLive already described for this class, and seeding Seen with it
/// is the whole of the "inherited" part. Complete does the same thing by walking the class first:
/// a method the class has already overridden wins there and its superclass' copy is dropped, and
/// the winner is not a candidate -- there is nothing left to offer for a declaration that is
/// written. Describing those members a second time to rediscover that would cost what the seed
/// saves.
AUTORTFM_DISABLE bool ClassOverrideCandidatesLive(FUtf8StringView ClassName,
                                                  const TArray<GodotVerse::FCompleteItem>& OwnMembers,
                                                  TArray<GodotVerse::FCompleteItem>& OutItems)
{
    using GodotVerse::FCompleteItem;

    OutItems.Empty();

    const uLang::CClass* const Class = FindScriptClassLive(ClassName);
    if (!Class)
    {
        return false;
    }

    TSet<FUtf8String> Seen;
    Seen.Reserve(OwnMembers.Num());
    for (const FCompleteItem& Member : OwnMembers)
    {
        Seen.Add(Member.Name);
    }

    // The class itself as the access scope, which is where the cursor is: a superclass member the
    // class body could not name is not one it could override either.
    CollectClassAndSupers(*Class, Class, ECompleteFilter::Any, 0, Seen, OutItems);
    OutItems.RemoveAll([](const FCompleteItem& Item) { return !Item.bIsOverridable; });
    OutItems.Sort([](const FCompleteItem& Left, const FCompleteItem& Right) { return Left.Name < Right.Name; });
    return true;
}

AUTORTFM_DISABLE TResult<TArray<FCompleteItem>> ClassMembers(FUtf8StringView ClassName)
{
    const TResult<const FAnalysisSnapshot::FClass*> Found = FindSnapshotClass(ClassName);
    if (Found)
    {
        return Found.GetValue()->Members;
    }
    if (Found.GetFailure() != EHostFailure::NoSuchClass)
    {
        return Found.GetFailure();
    }
    // A generated binding. Asked second because the two are different namespaces and a name in
    // both is the author's own class rather than the one generated from their GDScript.
    if (const TArray<FCompleteItem>* const Binding =
            GetAnalysisSnapshot()->BindingMembers.Find(FUtf8String(ClassName)))
    {
        return *Binding;
    }
    return EHostFailure::NoSuchClass;
}

AUTORTFM_DISABLE TResult<TArray<FCompleteItem>> ClassOverrideCandidates(FUtf8StringView ClassName)
{
    const TResult<const FAnalysisSnapshot::FClass*> Found = FindSnapshotClass(ClassName);
    if (!Found)
    {
        return Found.GetFailure();
    }
    return Found.GetValue()->OverrideCandidates;
}

AUTORTFM_DISABLE TResult<FSignatureDesc> SignatureAt(FUtf8StringView Path,
                                                     const FUtf8String& SourceText,
                                                     int32 Line,
                                                     int32 Column)
{
    if (const TOptional<EHostFailure> Declined = WhyProgramCannotDescribe(Path, SourceText))
    {
        return Declined.GetValue();
    }
    if (Line < 0 || Column < 0)
    {
        return EHostFailure::InvalidPosition;
    }

    const double Started = FPlatformTime::Seconds();

    uLang::CSemanticProgram* const Program = CurrentSemanticProgram();
    if (!Program || !Program->_AstProject)
    {
        return EHostFailure::NothingAtPosition;
    }

    FSignatureDesc OutDesc;

    // The callee resolves the way any other identifier does, so this reuses the lookup walk rather
    // than the completion one: what is wanted is the definition at a position, not a scope.
    const FUtf8String ProjectVersePath(ScriptVersePath);
    FLookupVisitor Visitor(*Program, FUtf8String(Path), (uint32)Line, (uint32)Column);
    for (const uLang::CAstCompilationUnit* CompilationUnit : Program->_AstProject->OrderedCompilationUnits())
    {
        for (const uLang::CAstPackage* Package : CompilationUnit->Packages())
        {
            // The project's package alone, for the reason spelled out in Complete: the mirror is
            // an InternalUser package too, and walking it was the whole of this call's ~39 ms.
            if (FUtf8String(Package->_VersePath.AsCString()) != ProjectVersePath
                || !Package->_RootModule || !Package->_RootModule->GetAstPackage())
            {
                continue;
            }
            Package->_RootModule->GetAstPackage()->VisitChildren(Visitor);
        }
    }

    if (!Visitor.Found)
    {
        return EHostFailure::NothingAtPosition;
    }
    const uLang::CFunction* Function = Visitor.Found->AsNullable<uLang::CFunction>();
    if (!Function)
    {
        return EHostFailure::NotAFunction;
    }

    OutDesc.Name = FUtf8String(Function->AsNameCString());

    // The parameter *types*, which is the only place a named parameter's `?` survives: AnalyzeParam
    // sets the definition's type to the value type and wraps it in a CNamedType afterwards, for the
    // signature alone (SemanticAnalyzer.cpp, `if (ParamAst->IsNamed())`). So `?ExactMatch:logic`
    // reaches the loop below as a definition named ExactMatch of type logic, and without this the
    // hint spells a call no author can write -- a named parameter is passed `?ExactMatch := true`
    // and never positionally.
    TArray<const uLang::CTypeBase*> ParamTypes;
    if (const uLang::CFunctionType* Type = Function->_Signature.GetFunctionType())
    {
        OutDesc.Result = FULangConversionUtils::ULangStrToFUtf8String(Type->GetReturnType().AsCode());
        for (const uLang::CTypeBase* ParamType : Type->GetParamTypes())
        {
            ParamTypes.Add(ParamType);
        }
    }

    int32 ParamIndex = 0;
    for (const uLang::CDataDefinition* Param : Function->_Signature.GetParams())
    {
        const int32 ThisParam = ParamIndex++;
        FCompleteItem Item;
        if (Param && DescribeCompletion(*Param, ECompleteFilter::Any, Item))
        {
            Item.bIsNamed = ParamTypes.IsValidIndex(ThisParam) && ParamTypes[ThisParam] != nullptr
                && ParamTypes[ThisParam]->GetNormalType().AsNamedType() != nullptr;
            OutDesc.Params.Add(MoveTemp(Item));
        }
    }

    if (AnalysisTraceEnabled())
    {
        fprintf(stderr,
                "[vh-trace] signature: %.2f ms, %d param(s)\n",
                (FPlatformTime::Seconds() - Started) * 1000.0,
                OutDesc.Params.Num());
        fflush(stderr);
    }
    return MoveTemp(OutDesc);
}

} // namespace GodotVerse
