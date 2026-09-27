// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostScript.h"
#include "HostEngineAdapters.h"
#include "HostMarshal.h"
#include "HostScriptState.h"
#include "HostTypeModel.h"
#include "verse_diagnostics.h"
#include "AutoRTFM.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "GodotClasses.h"
#include "GodotClassNames.gen.h"
#include "GodotMathLayout.gen.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "HostDebug.h"
#include "HostEventLoop.h"
#include "HostRuntime.h"
#include "ISolarisIde.h"
#include "ISolarisModule.h"
#include "IVerseModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HostSidecar.h"
#include "Dom/JsonValue.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "SolBuildDiagnostic.h"
#include "Templates/Function.h"
#include "TestUtils/PlaceholderObjectForContentScope.h"
#include "ULangUEUtils.h"
#include "VerseComputationLimitControl.h"
#include "VerseContentScope.h"
#include "VerseEvent.h"
#include "VerseString.h"
#include "VerseTask.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "VerseVM/Inline/VVMRefInline.h"
#include "VerseVM/Inline/VVMValueInline.h"
#include "VerseVM/Inline/VVMValueObjectInline.h"
#include "VerseVM/Inline/VVMVerseClassInline.h"
#include "VerseVM/VVMArray.h"
#include "VerseVM/VVMMutableArray.h"
#include "VerseVM/VVMShape.h"
#include "VerseVM/VVMNativeRef.h"
#include "VerseVM/VVMRef.h"
#include "VerseVM/VVMRestValue.h"
#include "VerseVM/VVMClass.h"
#include "VerseVM/VVMCoroutine.h"
#include "VerseVM/Inline/VVMEnumerationInline.h"
#include "VerseVM/VVMEnumerator.h"
#include "VerseVM/VVMFalse.h"
#include "VerseVM/VVMOption.h"
#include "VerseVM/VVMInt.h"
#include "VerseVM/VVMFloat.h"
#include "VerseVM/VVMValueObject.h"
#include "VerseVM/VVMOpResult.h"
#include "VerseVM/VVMVerseClass.h"
#include "VerseVM/VVMGlobalProgram.h"
#include "VerseVM/VVMNativeConverter.h"
#include "VerseVM/VVMNativeFunction.h"
#include "VerseVM/VVMNativeStruct.h"
#include "VerseVM/VVMNamedType.h"
#include "VerseVM/VVMPackage.h"
#include "VerseVM/VVMProgram.h"
#include "VerseVM/VVMContext.h"
#include "VerseVM/VVMTaskGroup.h"
#include "VerseVM/VVMUniqueString.h"
#include "uLang/Diagnostics/Diagnostics.h"
#include "uLang/Semantics/Attributable.h"
#include "uLang/Semantics/DataDefinition.h"
#include "uLang/Semantics/Definition.h"
#include "uLang/Semantics/Expression.h"
#include "uLang/Semantics/FilteredDefinitionRange.h"
#include "uLang/Semantics/ModuleAlias.h"
#include "uLang/Semantics/SemanticClass.h"
#include "uLang/Semantics/SemanticEnumeration.h"
#include "uLang/Semantics/SemanticFunction.h"
#include "uLang/Semantics/SemanticProgram.h"
#include "uLang/Semantics/SemanticTypes.h"
#include "uLang/Semantics/TypeAlias.h"
#include "uLang/Syntax/VstNode.h"
#include "uLang/SourceProject/PackageRole.h"
#include "uLang/SourceProject/SourceDataProject.h"
#include "uLang/SourceProject/UploadedAtFNVersion.h"
#include "uLang/SourceProject/VerseScope.h"
#include "uLang/SourceProject/VerseVersion.h"
#include "uLang/CompilerPasses/ApiLayerInjections.h"
#include "uLang/CompilerPasses/IParserPass.h"
#include "uLang/Toolchain/ModularFeatureManager.h"
#include "uLang/Toolchain/ProgramBuildManager.h"

#include <atomic>
#include <cstdio>
#include <thread>

using GodotVerse::AnalysisTraceEnabled;
using GodotVerse::BindingsVersePath;
using GodotVerse::ClassChainOfOrigin;
using GodotVerse::ClassMembersLive;
using GodotVerse::ClassOverrideCandidatesLive;
using GodotVerse::ClassOwnMembers;
using GodotVerse::ClassifyDeclaredType;
using GodotVerse::ClassOriginOf;
using GodotVerse::CollectStructFields;
using GodotVerse::DescribeClassOf;
using GodotVerse::DescribeExportType;
using GodotVerse::DescribeType;
using GodotVerse::EClassOrigin;
using GodotVerse::EDeclaredKind;
using GodotVerse::DecoratedNameOf;
using GodotVerse::ExtensionMethodDecoratedName;
using GodotVerse::FDeclaredType;
using GodotVerse::FillLocation;
using GodotVerse::FindBindingClass;
using GodotVerse::FindGodotClass;
using GodotVerse::FindMirroredClass;
using GodotVerse::FindMirroredVClass;
using GodotVerse::FindReferenceClass;
using GodotVerse::FindScriptClassLive;
using GodotVerse::FindStructLayout;
using GodotVerse::FMemberType;
using GodotVerse::ForgetMirrorDefinitions;
using GodotVerse::FStructLayout;
using GodotVerse::FUserStructLayout;
using GodotVerse::GodotVersePath;
using GodotVerse::MirrorDefinitionsRecorded;
using GodotVerse::NativeClassOf;
using GodotVerse::NearestAncestorOfOrigin;
using GodotVerse::NewArrayValue;
using GodotVerse::NewReferenceWrapper;
using GodotVerse::NewStructValue;
using GodotVerse::PayloadStructClass;
using GodotVerse::QualifiedNameOf;
using GodotVerse::ReferenceOption;
using GodotVerse::ValueToWire;
using GodotVerse::WireToValue;
using GodotVerse::RecordMirrorDefinitions;
using GodotVerse::ScriptVersePath;
using GodotVerse::SubjectTypeOfDiagnostic;
using GodotVerse::UnwrapDeclaredType;

namespace {

/// What every generation's package name starts with; the generation number finishes it.
///
/// Not ISolIdeDataSource::DefaultDataSourceName, which is what ISolarisIde::AddDataSource would
/// have picked: the name has to be the host's to choose, because publishing a package marks its
/// exports LoaderImport and publishing that same package again asserts on the flag. A generation
/// is therefore a name no publish has used, and a name the IDE owns cannot be one.
constexpr const char* ScriptPackageBaseName = "GodotScripts";
constexpr const char* MainFunctionName = "Main(:[][]char,:[[]char][]char)";

/// A second package sharing the native package's verse path, so a script's existing
/// `using { /Godot.org/Godot }` reaches these definitions with no extra import.
constexpr const char* AttributePackageName = "GodotAttributes";
constexpr const char* AttributePackageVersePath = GodotVersePath;
constexpr const char* AttributeSnippetPath = "GodotAttributes.verse";

/// The generated bindings (R-INT-7): every ClassDB class the mirror does not carry, and every
/// script class with a `class_name`, as ordinary Verse subclasses of their mirrored base.
///
/// At BindingsVersePath rather than the mirror's, so a binding whose name collides with a
/// mirrored one is an ambiguity the author resolves at the use site -- `(/Godot.org/Bindings:)timer`
/// -- rather than a redefinition reported against a generated file nobody can edit. What that costs
/// is that the package cannot see anything `<internal>` to the mirror, which is why `CallConst`
/// exists (docs/generated-bindings.md 10.6).
constexpr const char* BindingsPackageBaseName = "GodotBindings";
constexpr const char* BindingsSnippetPath = "GodotBindings.verse";

/// The attributes this bridge owns, as Verse source compiled in this process.
///
/// They cannot ship in host/Verse with the rest of the package: `class(attribute)` is refused
/// unless CScope::IsAuthoredByEpic(), which only FGodotAuthorshipInjection below grants -- and
/// that runs here, while host/Verse is compiled by VNI at UBT time, which nothing we build can
/// reach. Hence a runtime-only package for the one thing VNI will not accept.
///
/// None of them carries `@customattribhandler`. Epic's `editable` does, which is why applying it
/// obliges the host to load VerseSimulationMetadata and submit to that handler's rules -- the ones
/// written for UEFN's inspector, which refuse a struct that is not concrete and so refuse `color`.
/// An attribute with no handler is just a type applied to a definition, which is all this needs.
constexpr const char* AttributePackageSource =
    "# Registers the class it is applied to as a Godot global class, the way C#'s [GlobalClass]\n"
    "# does. A marker with no argument: the name Godot registers is the class's own, which the\n"
    "# one-top-level-name-per-file rule already pins to the file stem.\n"
    "@attribscope_class\n"
    "global_class<public> := class<computes>(attribute) {}\n"
    "\n"
    "# Sends the member to Godot's inspector, the way C#'s [Export] does. What kind of field it\n"
    "# gets is read off the member's declared type rather than said again here: a bounded number\n"
    "# is a range, an enum is a list of choices, a mirrored class is the node or resource a slot\n"
    "# will accept.\n"
    "@attribscope_data\n"
    "export<public> := class<computes>(attribute) {}\n"
    "\n"
    "# Sends the member to Godot as a signal, the way C#'s [Signal] does for a delegate. The\n"
    "# member is an ordinary /Verse.org/Verse `event(t)`, so it is awaitable and satisfies\n"
    "# `awaitable(t)` for any Verse code that has never heard of Godot; the attribute is what\n"
    "# also registers it with the engine. Its *name* is the signal's name and its `t` is the\n"
    "# payload, so there is no second place to spell either.\n"
    "#\n"
    "#     @export_signal\n"
    "#     Struck<public>:event(struck_payload) = event(struck_payload){}\n"
    "#\n"
    "# Not spelled `@export_signal`'s obvious short name: a bare marker attribute *is* a class,\n"
    "# this package shares /Godot.org/Godot's verse path, and a third definition of `signal`\n"
    "# beside `signal(t)` and its `signal()` alias is glitch 3532 (docs/signal-declaration.md 3).\n"
    "# It joins the @export* family instead, which is what it does: the member exists either\n"
    "# way, and the attribute is what sends it to Godot.\n"
    "@attribscope_data\n"
    "export_signal<public> := class<computes>(attribute) {}\n"
    "\n"
    "# Runs the class's Ready and Process in the editor as well as in the game, the way C#'s\n"
    "# [Tool] does. A marker with no argument, and opt-in per class rather than per project: a\n"
    "# tool script runs against the scene the author is editing, with its mistakes.\n"
    "@attribscope_class\n"
    "tool<public> := class<computes>(attribute) {}\n"
    "\n"
    "# The inspector section the member opens, which every member declared after it joins until\n"
    "# one opens another -- Godot has no per-property section, so the section is a position in the\n"
    "# list. [ExportCategory], [ExportGroup] and [ExportSubgroup] in C#, and the same three nested\n"
    "# depths: a category is a heading, a group folds under it, a subgroup folds under that.\n"
    "@attribscope_data\n"
    "export_category_attribute<public> := class<computes>(attribute):\n"
    "    Name<public>:string\n"
    "\n"
    "export_category<public><constructor>(Name:string)<computes> := export_category_attribute:\n"
    "    Name := Name\n"
    "\n"
    "@attribscope_data\n"
    "export_group_attribute<public> := class<computes>(attribute):\n"
    "    Name<public>:string\n"
    "\n"
    "export_group<public><constructor>(Name:string)<computes> := export_group_attribute:\n"
    "    Name := Name\n"
    "\n"
    "@attribscope_data\n"
    "export_subgroup_attribute<public> := class<computes>(attribute):\n"
    "    Name<public>:string\n"
    "\n"
    "export_subgroup<public><constructor>(Name:string)<computes> := export_subgroup_attribute:\n"
    "    Name := Name\n"
    "\n"
    "# Says which class a module of constants and static functions belongs to, so Godot can be told\n"
    "# about them: `@statics(player)` over `player_statics`. Verse has no `static` keyword, and a\n"
    "# module is what it has instead -- a script can already write `PlayerStatics.MaxSpeed` with no\n"
    "# bridge in it at all. What the attribute buys is the *link*, which the editor needs and a\n"
    "# naming convention could not give: a typo in a convention produces a silently empty statics\n"
    "# module, where a module naming a class that does not exist is a diagnostic.\n"
    "@attribscope_module\n"
    "statics_attribute<public> := class<computes>(attribute):\n"
    "    ClassName<public>:string\n"
    "\n"
    "statics<public><constructor>(ClassName:string)<computes> := statics_attribute:\n"
    "    ClassName := ClassName\n"
    "\n"
    "# Makes the method a remote procedure call, the way GDScript's @rpc and C#'s [Rpc] do.\n"
    "#\n"
    "#     @rpc(\"any_peer call_local unreliable_ordered 2\")\n"
    "#\n"
    "# The words are Godot's own and may be written in any order, separated by spaces or\n"
    "# commas: any_peer/authority is who may call it, call_local/call_remote whether the\n"
    "# caller runs it too, and reliable/unreliable/unreliable_ordered how it travels. A\n"
    "# number among them is the channel. Anything else is a diagnostic at the method.\n"
    "#\n"
    "# **One string rather than GDScript's four arguments, and one constructor rather than\n"
    "# four arities** -- because the Verse toolchain refuses both spellings today, in two\n"
    "# places that each describe themselves as unfinished. An attribute site *references* its\n"
    "# constructor before calling it, and referencing an overloaded function is \"not yet\n"
    "# implemented\"; and GetAttributeTextValue, the only accessor there is, refuses any\n"
    "# attribute whose argument is a tuple, under a @HACK naming SOL-972 and asking for\n"
    "# compile-time evaluation of attribute types. **Worth re-checking on an engine drop**:\n"
    "# when either lands, @rpc(\"any_peer\", \"call_local\") is the spelling to move to, and\n"
    "# moving is additive -- this one keeps working beside it.\n"
    "#\n"
    "# There is no bare `@rpc` either: an attribute with no argument has to be the attribute\n"
    "# *class*, and the class is what this constructor builds. GDScript's default is spelled\n"
    "# out instead, as `@rpc(\"authority\")`.\n"
    "@attribscope_function\n"
    "rpc_attribute<public> := class<computes>(attribute):\n"
    "    Config<public>:string\n"
    "\n"
    "rpc<public><constructor>(Config:string)<computes> := rpc_attribute:\n"
    "    Config := Config\n"
    "\n"
    "# The five inspector hints a declared type cannot imply (R-EXP-1). Everything a bounded\n"
    "# number, an enum or a mirrored class can say is read off the type and needs no attribute;\n"
    "# these are the set where the type is `string` or `int` and says nothing about what the\n"
    "# value is for. Each takes one string, or none -- see the note on @rpc above for why an\n"
    "# attribute may not take several, and why that is worth re-checking on an engine drop.\n"
    "\n"
    "# A file path. The filter is Godot's own spelling -- `@export_file(\"*.png\")`, or\n"
    "# `@export_file(\"*.png,*.jpg\")`, and `@export_file(\"*\")` for any file. There is no\n"
    "# argumentless spelling, because that would have to be the attribute class itself.\n"
    "@attribscope_data\n"
    "export_file_attribute<public> := class<computes>(attribute):\n"
    "    Filter<public>:string\n"
    "\n"
    "export_file<public><constructor>(Filter:string)<computes> := export_file_attribute:\n"
    "    Filter := Filter\n"
    "\n"
    "# A directory path rather than a file. A marker, like @tool.\n"
    "@attribscope_data\n"
    "export_dir<public> := class<computes>(attribute) {}\n"
    "\n"
    "# A paragraph rather than a line: Godot draws a multi-line text box instead of a field.\n"
    "@attribscope_data\n"
    "export_multiline<public> := class<computes>(attribute) {}\n"
    "\n"
    "# A bitmask over named bits -- `@export_flags(\"Fire,Water,Earth\")` is bit 1, 2 and 4.\n"
    "# Verse has no flag enum, so the names cannot come from the type. Comma separated, which\n"
    "# is Godot's own spelling for this hint and passes straight through.\n"
    "@attribscope_data\n"
    "export_flags_attribute<public> := class<computes>(attribute):\n"
    "    Names<public>:string\n"
    "\n"
    "export_flags<public><constructor>(Names:string)<computes> := export_flags_attribute:\n"
    "    Names := Names\n"
    "\n"
    "# A NodePath, which is a `string` on this wire. The argument is the Godot class a picked\n"
    "# node must be -- `@export_node_path(\"Node2D\")`, or `@export_node_path(\"Node\")` for any.\n"
    "@attribscope_data\n"
    "export_node_path_attribute<public> := class<computes>(attribute):\n"
    "    TypeName<public>:string\n"
    "\n"
    "export_node_path<public><constructor>(TypeName:string)<computes>"
    " := export_node_path_attribute:\n"
    "    TypeName := TypeName\n"
    "\n"
    "# The icon the scene tree and the create-node dialog show for this class, the way C#'s\n"
    "# [Icon] does: `@icon(\"res://art/player.svg\")`.\n"
    "#\n"
    "# Declared here so a script carrying it compiles, and read *out of the source text* by\n"
    "# verse_scan_class_decl rather than from here -- Godot asks get_class_icon_path of scripts\n"
    "# it has only scanned, from the filesystem thread, before any host has been asked to build\n"
    "# anything. That is the same reason @global_class is read twice, and the two must agree.\n"
    "@attribscope_class\n"
    "icon_attribute<public> := class<computes>(attribute):\n"
    "    Path<public>:string\n"
    "\n"
    "icon<public><constructor>(Path:string)<computes> := icon_attribute:\n"
    "    Path := Path\n";

/// One .verse file, as the toolchain wants it: a path, its text, and somewhere to cache the
/// parse.
///
/// The IDE's own ISolIdeDataSource is the thing this replaces, and the one capability it had
/// that matters is the one kept here -- text that can be replaced in place, which is what an
/// editor's unsaved buffer is. Everything else it carried (VPL widgets, disk round-trips,
/// mutation broadcasts) has no caller on this side.
class FHostSourceSnippet : public uLang::ISourceSnippet
{
public:
    FHostSourceSnippet(uLang::CUTF8String&& InPath, uLang::CUTF8String&& InText)
        : Path(uLang::Move(InPath))
        , Text(uLang::Move(InText))
    {
    }

    virtual uLang::CUTF8String GetPath() const override { return Path; }
    virtual void SetPath(const uLang::CUTF8String& InPath) override { Path = InPath; }
    virtual bool IsInMemoryOnly() const override { return true; }
    virtual uLang::TOptional<uLang::CUTF8String> GetText() const override { return Text; }
    virtual uLang::TOptional<Verse::Vst::TNodeRef<Verse::Vst::Snippet>> GetVst() const override { return Vst; }
    virtual void SetVst(Verse::Vst::TNodeRef<Verse::Vst::Snippet> Snippet) override { Vst = Snippet; }

    /// Replacing the text drops the cached parse with it. CToolchain::ProcessSnippet clones a
    /// cached VST it considers valid instead of reparsing, so a VST kept across a text change
    /// would have the build analyse the text that was just replaced.
    void SetText(uLang::CUTF8String&& NewText)
    {
        Text = uLang::Move(NewText);
        Vst.Reset();
    }

private:
    uLang::CUTF8String Path;
    uLang::CUTF8String Text;
    uLang::TOptional<Verse::Vst::TNodeRef<Verse::Vst::Snippet>> Vst;
};

using FMainFunction = TVerseFunction<FVerseResult(
    TVerseCall<void>, const TArray<verse::string>&, const TMap<verse::string, verse::string>&)>;

TSharedPtr<ISolarisIde> GIde;

/// The project source the IDE builds, kept because IncrementalizeProjectSource is asked for it
/// by the caller rather than reachable from the IDE.
TOptional<TSharedRef<ISolIdeSourceProject>> GSourceProject;

/// How many generations have been published, and the name of the newest one's package. Zero and
/// empty until the first build: every lookup that names the package answers nothing until then,
/// which is the state the editor is in before its first Play.
int32 GScriptGeneration = 0;
FUtf8String GScriptPackageName;

/// The bindings package: what the consumer last handed over, the name the last prepare used, and
/// whether the source has changed since. Generational for the reason the script package is.
FUtf8String GBindingsSource;
FUtf8String GBindingsPackageName;
int32 GBindingsGeneration = 0;
bool GBindingsDirty = false;

/// Godot class name -> the Verse class in the bindings package, and the same for a script's global
/// class name. Two maps rather than one, because the two questions are asked of different
/// callbacks and a ClassDB name and a script name can collide with each other.
TMap<FUtf8String, FUtf8String> GBindingByGodotClass;
TMap<FUtf8String, FUtf8String> GBindingByScriptClass;

/// The reverse, for minting: the Verse binding class -> the name InstantiateClass is handed. A
/// ClassDB binding hands over its Godot class name; a script binding hands over the script's global
/// name, and the consumer is the side that knows that means "make the base and set_script"
/// (R-INT-12). The host never learns what a script is.
TMap<FUtf8String, FUtf8String> GMintNameByBinding;

/// The same rows before they were indexed, which is what the cook writes into the sidecar: an
/// exported game has no Godot to enumerate and no editor to have asked, so the table has to travel
/// with the packages it describes (R-INT-11).
TArray<GodotVerse::FBindingClass> GBindingRows;

/// Empties the two caches that answer "what does this cross as" and "what does this mint".
/// Declared here and defined beside the maps, which are a long way down this file.
AUTORTFM_DISABLE void ForgetCachedClasses();

/// The package the *source project* currently holds, which is not the same thing: a build that
/// failed published nothing but left its source behind for analysis to report against, so this
/// is the name the next build has to retire while GScriptPackageName stays where it was.
FUtf8String GScriptSourcePackageName;

/// Whether a generation has ever been published. Not a latch against a second one -- that is the
/// point of the phase -- but the guard on everything that reads a semantic program, which does
/// not exist until the first build makes one.
bool GProjectBuilt = false;

/// The generation GScriptSourcePackageName is named for, and the files it was prepared from.
///
/// A build prepares the *next* generation's package as its last act rather than the current one's
/// as its first, so that every analysis between two builds already runs under the name the next
/// publish will use. That is the whole of what makes a build able to reuse an analysis: a package
/// name no publish has used is what a generation is, and an analysis cannot be reused for a
/// generation it was not analysed under.
int32 GPreparedGeneration = 0;
TArray<GodotVerse::FScriptSource> GPreparedSources;

/// Whether the last analysis finished clean. A build reuses no program that a diagnostic was
/// reported against: the slow path is what reports it at the line the author is looking at.
bool GLastAnalysisClean = false;

/// Lets `/Godot.org/` declare attributes of its own.
///
/// AddSuperType refuses `class(attribute)` unless CScope::IsAuthoredByEpic(), which tests the
/// definition's verse path against CSemanticProgram::_EpicInternalModulePrefixes -- seeded with
/// Epic's three domains by CSemanticProgram::Initialize and reachable no other way. Note this
/// grants *authorship*, which the InternalUser package scope does not: that only unlocks access
/// to epic_internal definitions, which is why `@editable` can be borrowed but not declared.
///
/// It has to run per build rather than once: CProgramBuildManager::Build calls
/// ResetSemanticProgram() before every compile and every analysis, so the program -- and its
/// prefix list -- is new each time. This is the first hook that runs after that reset and still
/// ahead of analysis. The auto-qualify pre-pass builds a second program of its own, but routes
/// through the same RunCompilerPrePass, so it is covered too.
class FGodotAuthorshipInjection : public uLang::IPreSemAnalysisInjection
{
public:
    AUTORTFM_DISABLE virtual bool Ingest(
        const Verse::Vst::TNodeRef<Verse::Vst::Project>&,
        const uLang::SProgramContext& ProgramContext,
        const uLang::SBuildContext&) override
    {
        ProgramContext._Program->_EpicInternalModulePrefixes.AddUnique("/Godot.org/");
        return false; // Do not halt the toolchain.
    }
};

/// The parser, with one snippet's parse remembered: /Godot.org/Godot's digest, which is the same
/// 2.1 MB of text at every build and every analysis for the life of the process.
///
/// 97% of the digest bytes the parse phase reads are the mirror's; a project's own files are a
/// rounding error beside it. Cloning the tree that parse produced costs **36 ms** where reading the
/// text again costs most of the phase, so the phase goes from **202 ms to 77 ms** and an analysis
/// from 787 ms to 555 (spec.md R-PERF-2 has the table and the machine).
///
/// uLang has a dormant mechanism for this -- `SBuildContext::bCloneValidSnippetVsts`, with
/// `ISourceSnippet::IsSnippetValid` as its test -- and it cannot be reached from here twice over:
/// the flag is set on a build context `CProgramBuildManager::Build` constructs and nothing exposes,
/// and a digest's snippet is a `CSourceDataSnippet`, which does not override `IsSnippetValid`, so
/// the base's `false` stands however the flag is set. Nothing in the engine sets it, either.
///
/// Substituting the pass is the supported seam instead: `SToolchainOverrides::Parser` is what the
/// build manager is constructed with, and `ISolarisIde::SetBuildManager` takes a manager built that
/// way. Everything else -- the FN version gates, `SetBlockExecution`, the source project, the
/// registries a build updates -- stays `FSolarisIde::BuildAll`'s.
///
/// One entry, and a threshold that only the mirror clears: the next largest digest is `/Verse.org`
/// at 32 KB, and a script's own file is both small and different at every keystroke, so a cache
/// with room for it would hold nothing but misses.
///
/// **The cached tree is the one the parser produced, cloned before anything read it.** Semantic
/// analysis is handed a fresh clone each time for the same reason the engine's own path clones:
/// what a later phase writes onto a VST node must not be what the next build starts from.
class FGodotCachingParser : public uLang::IParserPass
{
public:
    explicit FGodotCachingParser(const uLang::TSRef<uLang::IParserPass>& InInner)
        : Inner(InInner)
    {}

    // No AUTORTFM_DISABLE, unlike everything else here that reaches Solaris: `IParserPass` declares
    // this one AUTORTFM_ENABLE and an override may not narrow the mode. It is safe because a parse
    // is only ever reached from a build, and every road into a build is disabled already.
    virtual void ProcessSnippet(const Verse::Vst::TNodeRef<Verse::Vst::Snippet>& OutVst,
                                const uLang::CUTF8StringView& TextViewSnippet,
                                const uLang::SBuildContext& BuildContext,
                                const uint32_t VerseVersion,
                                const uint32_t UploadedAtFNVersion) const override
    {
        // The version pair is part of the key rather than assumed: a digest carries its own
        // effective Verse version (CToolchain::FillInVst reads it off the digest, not off the
        // package), and the same text at a different version is a different parse.
        if (CachedVst.IsValid() && CachedVerseVersion == VerseVersion
            && CachedUploadedAtFNVersion == UploadedAtFNVersion && CachedText.ToStringView() == TextViewSnippet)
        {
            // One clone per child rather than one clone of the snippet whose children are then
            // moved across: `AppendChild` calls `DropParent`, which removes the node from the
            // array being walked. A freshly cloned node has no parent to drop, so nothing the
            // cache holds is touched -- and the cached tree has to survive, since every later
            // build reads it again.
            OutVst->AccessChildren().Reserve(OutVst->GetChildCount() + CachedVst->GetChildCount());
            for (const Verse::Vst::TNodeRef<Verse::Vst::Node>& Child : CachedVst->GetChildren())
            {
                OutVst->AppendChild(Child->CloneNode());
            }
            OutVst->SetForm(CachedVst->GetForm());
            OutVst->SetWhence(CachedVst->Whence());
            return;
        }

        Inner->ProcessSnippet(OutVst, TextViewSnippet, BuildContext, VerseVersion, UploadedAtFNVersion);

        // FillInVst swaps a clean diagnostics object in around each snippet, so this is what *this*
        // snippet's parse reported and nothing else. A tree with a syntax error in it is not one to
        // hand back a second time.
        if (TextViewSnippet.ByteLen() < CacheThresholdBytes || BuildContext._Diagnostics->HasErrors())
        {
            return;
        }

        // On the *second* sighting of a text, not the first. The one large snippet that is only
        // ever parsed once is the mirror's own source, which the first build reads and no build
        // after it does -- every later one reads the digest instead. Caching on sight put a clone
        // of 4.6 MB of Verse on the first build's critical path and never hit it, which cost the
        // first build ~240 ms to save nothing.
        if (SeenText.ToStringView() != TextViewSnippet)
        {
            SeenText = uLang::CUTF8String(TextViewSnippet);
            return;
        }
        CachedText = Move(SeenText);
        SeenText = uLang::CUTF8String();

        const double Started = FPlatformTime::Seconds();
        CachedVst = OutVst->CloneNode().As<Verse::Vst::Snippet>();
        CachedVerseVersion = VerseVersion;
        CachedUploadedAtFNVersion = UploadedAtFNVersion;

        // Open because the checker takes this function's AutoRTFM mode from the interface it
        // overrides, and everything the trace touches is the host's own disabled code.
        AutoRTFM::Open([&] {
            if (!AnalysisTraceEnabled())
            {
                return;
            }
            fprintf(stderr,
                    "[vh-trace] parse cache: %d KB of text, %.1f ms to clone\n",
                    (int32)(CachedText.ByteLen() / 1024),
                    (FPlatformTime::Seconds() - Started) * 1000.0);
            fflush(stderr);
        });
    }

private:
    /// Above the largest digest that is not the mirror's by a factor of sixteen.
    static constexpr int32 CacheThresholdBytes = 512 * 1024;

    uLang::TSRef<uLang::IParserPass> Inner;

    // ProcessSnippet is const on the interface; the cache is the whole reason this type exists.
    mutable uLang::CUTF8String CachedText;
    /// The last large snippet parsed without being cached -- see the second-sighting rule above.
    mutable uLang::CUTF8String SeenText;
    mutable Verse::Vst::TNodePtr<Verse::Vst::Snippet> CachedVst;
    mutable uint32_t CachedVerseVersion = 0;
    mutable uint32_t CachedUploadedAtFNVersion = 0;
};

/// Adds the attribute package to the IDE's source project.
///
/// Must run before the first AddDataSource. FSolarisIde::EnsureDataSourcePackageExists snapshots
/// the project's other packages as the script package's dependencies exactly once, guarded on that
/// list being empty -- so a package added after the first script is never depended on, and its
/// definitions do not resolve however the script spells the `using`.
AUTORTFM_DISABLE void AddAttributePackage(ISolarisIde& Ide)
{
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = Ide.GetBuildManager();
    if (!BuildManager.IsValid())
    {
        GodotVerse::ReportError(UTF8TEXT("No build manager; Godot attributes will be unavailable."));
        return;
    }

    const uLang::CSourceProject::SPackage& Package =
        BuildManager->FindOrAddSourcePackage(AttributePackageName, AttributePackageVersePath);
    Package._Package->SetVerseScope(uLang::EVerseScope::InternalUser);
    Package._Package->SetVerseVersion(Verse::Version::LatestUnstable);
    Package._Package->SetAllowExperimental(true);

    BuildManager->AddSourceSnippet(
        uLang::TSRef<uLang::CSourceDataSnippet>::New(
            uLang::CUTF8String(AttributeSnippetPath), uLang::CUTF8String(AttributePackageSource)),
        AttributePackageName,
        AttributePackageVersePath);
}

/// Creates the package this project's scripts are built into, and makes it depend on everything
/// else the project holds.
///
/// The dependency list is the half of AddDataSource that is easy to miss: without it the native
/// package's definitions do not resolve however a script spells the `using`. Taken fresh for the
/// package named rather than once for the project, which is what lets a later generation depend
/// on the same set without inheriting an earlier generation's entry.
AUTORTFM_DISABLE void AddScriptPackage(uLang::CProgramBuildManager& BuildManager, const FUtf8String& PackageName)
{
    const uLang::CSourceProject::SPackage& Package =
        BuildManager.FindOrAddSourcePackage(
            FULangConversionUtils::FUtf8StringToULangStr(PackageName), ScriptVersePath);
    Package._Package->SetVerseScope(uLang::EVerseScope::InternalUser);
    Package._Package->SetVerseVersion(Verse::Version::LatestUnstable);
    Package._Package->SetAllowExperimental(true);

    uLang::TArray<uLang::CUTF8String> Dependencies;
    for (const uLang::CSourceProject::SPackage& Other : BuildManager.GetSourceProject()->_Packages)
    {
        if (&Other != &Package)
        {
            Dependencies.Add(Other._Package->GetName());
        }
    }
    Package._Package->SetDependencyPackages(uLang::Move(Dependencies));
}

/// Puts a fresh generation of the bindings package into the source project, retiring the previous
/// one by name (R-INT-8).
///
/// **Generational for the reason the script package is**, and it is not a style choice: the
/// assembler publishes every Source package the program carries, and publishing one name twice is
/// `!ObjectItem->HasAnyFlags(EInternalObjectFlags::LoaderImport)` inside AsyncLoading2.cpp, which is
/// a crash and not a diagnostic. Changing what a package *says* means changing what it is called.
///
/// Its dependencies exclude every generation of the script package, because the edge runs the other
/// way: a script names binding classes, and a binding names only mirrored ones. Including them
/// would be a cycle the build reports as an unresolved import.
///
/// Ends by recomputing the *script* package's dependency list, which is the half that is easy to
/// miss. AddScriptPackage takes that list fresh from whatever else the project holds, and the
/// generation package was prepared at the end of the last build -- before this package existed
/// under this name. Without the recompute an analysis resolves no binding at all until a build has
/// been round.
AUTORTFM_DISABLE void PrepareBindingsPackage(uLang::CProgramBuildManager& BuildManager)
{
    uLang::TArray<uLang::CSourceProject::SPackage>& Packages = BuildManager.GetSourceProject()->_Packages;
    if (!GBindingsPackageName.IsEmpty())
    {
        for (int32 Index = Packages.Num() - 1; Index >= 0; --Index)
        {
            if (FUtf8String(Packages[Index]._Package->GetName().AsCString()) == GBindingsPackageName)
            {
                Packages.RemoveAt(Index);
            }
        }
        GBindingsPackageName.Empty();
    }

    GBindingsDirty = false;
    if (GBindingsSource.IsEmpty())
    {
        // A project with no addons and no `class_name` scripts. No package at all rather than an
        // empty one: an empty Verse snippet is a parse error, not a package with nothing in it.
        return;
    }

    const FUtf8String PackageName =
        FUtf8String(FString::Printf(TEXT("%hs_%d"), BindingsPackageBaseName, ++GBindingsGeneration));

    const uLang::CSourceProject::SPackage& Package = BuildManager.FindOrAddSourcePackage(
        FULangConversionUtils::FUtf8StringToULangStr(PackageName), BindingsVersePath);
    Package._Package->SetVerseScope(uLang::EVerseScope::InternalUser);
    Package._Package->SetVerseVersion(Verse::Version::LatestUnstable);
    Package._Package->SetAllowExperimental(true);

    uLang::TArray<uLang::CUTF8String> Dependencies;
    for (const uLang::CSourceProject::SPackage& Other : Packages)
    {
        const FUtf8StringView OtherName(Other._Package->GetName().AsCString());
        if (&Other != &Package && !OtherName.StartsWith(FUtf8StringView(ScriptPackageBaseName)))
        {
            Dependencies.Add(Other._Package->GetName());
        }
    }
    Package._Package->SetDependencyPackages(uLang::Move(Dependencies));

    BuildManager.AddSourceSnippet(
        uLang::TSRef<uLang::CSourceDataSnippet>::New(
            uLang::CUTF8String(BindingsSnippetPath),
            FULangConversionUtils::FUtf8StringToULangStr(GBindingsSource)),
        FULangConversionUtils::FUtf8StringToULangStr(PackageName),
        BindingsVersePath);

    GBindingsPackageName = PackageName;

    if (!GScriptSourcePackageName.IsEmpty())
    {
        AddScriptPackage(BuildManager, GScriptSourcePackageName);
    }
}

/// Puts a pending bindings package into the project, if the consumer has handed over a new one.
///
/// Called by the build and by the analysis alike, because a roster change has to reach completion
/// without waiting for a build -- an addon's classes are completable "within a second or two of the
/// addon loading" (R-INT-8), and an analysis is what runs in that second.
AUTORTFM_DISABLE void FlushPendingBindings(uLang::CProgramBuildManager& BuildManager)
{
    if (GBindingsDirty)
    {
        PrepareBindingsPackage(BuildManager);
    }
}

/// Drops a generation's package out of the source project.
///
/// The generation that published it is still live in the VM and its instances still run; what
/// goes is only the *source* the next build compiles. Without this every class in the project
/// would be declared twice at the same verse path -- the retired generation's snippets and the
/// new one's -- and the build would report a redefinition for each.
AUTORTFM_DISABLE void RemoveScriptPackage(uLang::CProgramBuildManager& BuildManager, const FUtf8String& PackageName)
{
    if (PackageName.IsEmpty())
    {
        return;
    }

    uLang::TArray<uLang::CSourceProject::SPackage>& Packages = BuildManager.GetSourceProject()->_Packages;
    for (int32 Index = Packages.Num() - 1; Index >= 0; --Index)
    {
        if (FUtf8String(Packages[Index]._Package->GetName().AsCString()) == PackageName)
        {
            Packages.RemoveAt(Index);
        }
    }
}

/// The module a source file's definitions go into, creating it and every module on the way.
///
/// An empty path is the package's root module, which is where a project with no `.vmodule`
/// markers puts every one of its files.
AUTORTFM_DISABLE uLang::CSourceModule& FindOrAddModule(uLang::CSourceModule& Root, const FUtf8String& ModulePath)
{
    uLang::CSourceModule* Module = &Root;
    FUtf8String Remaining = ModulePath;
    while (!Remaining.IsEmpty())
    {
        FUtf8String Name;
        int32 Slash = INDEX_NONE;
        if (Remaining.FindChar(UTF8CHAR('/'), Slash))
        {
            Name = Remaining.Left(Slash);
            Remaining.RightChopInline(Slash + 1);
        }
        else
        {
            Name = Remaining;
            Remaining.Empty();
        }

        if (Name.IsEmpty())
        {
            continue;
        }

        const uLang::CUTF8String ULangName = FULangConversionUtils::FUtf8StringToULangStr(Name);
        if (uLang::TOptional<uLang::TSRef<uLang::CSourceModule>> Existing = Module->FindSubmodule(ULangName))
        {
            Module = Existing.GetValue().Get();
            continue;
        }

        uLang::TSRef<uLang::CSourceModule> Added = uLang::TSRef<uLang::CSourceModule>::New(ULangName);
        Module->_Submodules.Add(Added);
        Module = Added.Get();
    }
    return *Module;
}

/// Whether the semantic program the IDE currently holds came from an analysis-only build.
/// Code generation hangs an IR package off every module, and the AST accessors the symbol
/// lookup walks assert rather than degrade when it finds one -- so the lookup has to be able
/// to tell the two shapes apart itself instead of trusting that a caller only asks after an
/// analysis.
bool GProgramIsAnalysisOnly = false;

/// Re-analyses the project with one file's text replaced. An empty Path replaces nothing and
/// simply re-analyses what the IDE already holds.
AUTORTFM_DISABLE bool RunCheck(const FUtf8String& Path, const FUtf8String& SourceText, TFunction<void(const FSolDiagnostic&)> Sink);

/// RecordMirrorDefinitions, and then retires the package to its digest. Split because the two want
/// different moments: the recording wants the program mid-build, where the AST is whole, and the
/// role change wants to be the last thing a build does to the source project.
AUTORTFM_DISABLE void RecordAndRetireMirror();

/// Where one build's time went, accumulated from the compiler's own statistics events.
///
/// SBuildEventInfo carries no timings -- PhaseStarted and PhaseCompleted say only which phase, and
/// the payload is the EBuildPhase -- so the clock is ours and the events are the boundaries. Phases
/// do not nest (Toolchain.cpp: BuildProject brackets Parse, then CompileVst brackets each of the
/// rest in turn), but Parse and SemanticAnalysis each run twice when identifier auto-qualification
/// is on, so both the sum and the count are kept.
struct FAnalysisTrace
{
    static constexpr int32 NumPhases = 6; // uLang::EBuildPhase, Parse .. Link.

    double PhaseStart[NumPhases] = {};
    double PhaseSeconds[NumPhases] = {};
    int32 PhaseRuns[NumPhases] = {};

    int32 Functions = 0;
    int32 Classes = 0;
    int32 TopLevelDefinitions = 0;

    AUTORTFM_DISABLE void OnEvent(const uLang::SBuildEventInfo& Event)
    {
        switch (Event.Type)
        {
        case uLang::EBuildEvent::PhaseStarted:
            if (Event.Int32 < static_cast<uint32>(NumPhases))
            {
                PhaseStart[Event.Int32] = FPlatformTime::Seconds();
            }
            break;
        case uLang::EBuildEvent::PhaseCompleted:
            if (Event.Int32 < static_cast<uint32>(NumPhases))
            {
                PhaseSeconds[Event.Int32] += FPlatformTime::Seconds() - PhaseStart[Event.Int32];
                ++PhaseRuns[Event.Int32];
            }
            break;
        case uLang::EBuildEvent::FunctionDefinition:
            Functions += static_cast<int32>(Event.Int32);
            break;
        case uLang::EBuildEvent::ClassDefinition:
            Classes += static_cast<int32>(Event.Int32);
            break;
        case uLang::EBuildEvent::TopLevelDefinition:
            TopLevelDefinitions += static_cast<int32>(Event.Int32);
            break;
        default:
            break;
        }
    }
};

/// Writes a finished trace out, with the packages the build read beside it.
///
/// To stderr rather than through ReportDiagnostic because a background analysis runs off the game
/// thread and every callback in the ABI is the game thread's alone -- and because a trace an author
/// did not ask for has no business in Godot's Output dock.
///
/// The packages are read after the build rather than during it, so the roles printed are the ones
/// it used: IncrementalizeProjectSource marks a compiled package External on the CSourcePackage
/// itself and the mark is sticky, which is what decides whether the next build parses that
/// package's digest or all of its source again (Toolchain.cpp, CToolchain::FillInVst).
AUTORTFM_DISABLE void PrintAnalysisTrace(const FAnalysisTrace& Trace, const char* What, double WallSeconds)
{
    static const char* const PhaseNames[FAnalysisTrace::NumPhases] = {
        "parse", "semantic", "localization", "ir-gen", "codegen", "link"};

    fprintf(stderr, "[vh-trace] %s: %.1f ms wall\n", What, WallSeconds * 1000.0);

    if (GIde.IsValid())
    {
        const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager();
        if (BuildManager.IsValid())
        {
            for (const uLang::CSourceProject::SPackage& Package : BuildManager->GetSourceProject()->_Packages)
            {
                const uLang::CSourcePackage::SSettings& Settings = Package._Package->GetSettings();
                // In bytes rather than yes/no, because for an External package the digest *is* what
                // the parse phase reads: one synthetic snippet standing in for every source file.
                int32 DigestBytes = 0;
                if (Package._Package->_Digest.IsSet())
                {
                    if (const uLang::TOptional<uLang::CUTF8String> Text = Package._Package->_Digest->_Snippet->GetText())
                    {
                        DigestBytes = (int32)Text->ByteLen();
                    }
                }
                fprintf(stderr,
                        "[vh-trace]   package %-20s verse=%-24s role=%-8s digest=%6d B snippets=%d\n",
                        Package._Package->GetName().AsCString(),
                        Settings._VersePath.AsCString(),
                        uLang::ToString(Settings._Role),
                        DigestBytes,
                        Package._Package->GetNumSnippets());
            }
        }
    }

    for (int32 Phase = 0; Phase < FAnalysisTrace::NumPhases; ++Phase)
    {
        if (Trace.PhaseRuns[Phase] > 0)
        {
            fprintf(stderr,
                    "[vh-trace]   phase %-13s %8.1f ms  x%d\n",
                    PhaseNames[Phase],
                    Trace.PhaseSeconds[Phase] * 1000.0,
                    Trace.PhaseRuns[Phase]);
        }
    }

    fprintf(stderr,
            "[vh-trace]   defined %d function(s), %d class(es), %d top-level\n",
            Trace.Functions,
            Trace.Classes,
            Trace.TopLevelDefinitions);
    fflush(stderr);
}

/// What the calling thread lost to WaitForBackgroundCheck since the last vh_tick took the figure
/// away. Counted in one place because WaitForBackgroundCheck is the only place a caller can block
/// on an analysis, and ~20 entry points reach it.
int32 GAnalysisWaitCount = 0;
double GAnalysisWaitSeconds = 0.0;

/// The buffer the program the IDE holds was last built from -- see ProgramAlreadyDescribes.
/// Written only from RunCheck, and read only after WaitForBackgroundCheck has joined the worker
/// that may have written it.
FUtf8String GAnalysedPath;
FUtf8String GAnalysedSource;

/// The project's source files, in the order vh_compile_project listed them. Owned here rather
/// than by the IDE because the package is: see ScriptPackageName.
TArray<uLang::TSRef<FHostSourceSnippet>> GScriptSnippets;
/// The outer every content scope instantiates into. Rooted once for the process: a scope holds it
/// weakly, so nothing here roots one per instance (phase-5-design.md 13's first risk).
UPlaceholderObjectForContentScope* GScopeOuter = nullptr;

/// The scope everything that is not a call into one instance runs under: analysis, the statics
/// reader, `Main`, the pump, and every field access. Its guard is the root of the stack and is
/// pushed once, at EnterContentScope.
TSharedPtr<verse::FContentScope> GProjectScope;
TOptional<verse::FContentScopeGuard> GProjectScopeGuard;

/// How deep the VM entries are nested. Only depth 0 is a moment at which the *root* guard can be
/// swapped, which is what RefreshProjectScope needs and why this is counted rather than inferred:
/// FContentScopeGuard exposes no depth, and popping a guard that is not the active one is an
/// ensure() away from a corrupt stack.
int32 GVerseEntryDepth = 0;

/// Replaces a terminated project scope with a fresh one, rather than un-terminating it.
///
/// A raised Verse runtime error calls Terminate() on the *active* content scope
/// (VVMRuntimeError.cpp), and FRunningContext::EnterVM_Internal then returns *without running its
/// functor* for every later entry into that scope. Nothing downstream can tell that apart from a
/// call that ran and did nothing, which is why every entry point below reports what actually ran.
///
/// Phase 3 answered this with ResetTerminationState() on one process-wide scope, at the next frame
/// boundary; the comment it carried recorded the defect that made it necessary -- one script's
/// first mistake ended Verse for the process, in an editor nobody restarts. Phase 5 keeps the
/// answer and narrows the question: Epic never revives either (ContentScopeRepository hands out a
/// *fresh* scope), the replacement happens at the next entry rather than at the next tick, and
/// with a scope per instance there is nothing project-wide left to stop. See spec R-DIAG-3.
AUTORTFM_DISABLE void RefreshProjectScope()
{
    if (!GProjectScope.IsValid() || !GProjectScope->WasTerminated())
    {
        return;
    }
    GProjectScopeGuard.Reset();
    GProjectScope = verse::MakeContentScope(GScopeOuter);
    GProjectScopeGuard.Emplace(GProjectScope.ToSharedRef());
}

/// Counts one entry into the VM, and replaces a terminated project scope at the outermost one.
struct FVerseEntry
{
    AUTORTFM_DISABLE FVerseEntry()
    {
        if (GVerseEntryDepth++ == 0)
        {
            RefreshProjectScope();
        }
    }
    AUTORTFM_DISABLE ~FVerseEntry() { --GVerseEntryDepth; }

    FVerseEntry(const FVerseEntry&) = delete;
    FVerseEntry& operator=(const FVerseEntry&) = delete;
};

/// Every entry into the VM goes through here rather than calling Context.EnterVM directly, so that
/// a seventh entry point cannot be added that forgets the scope handling above.
template <typename TBody>
AUTORTFM_DISABLE void EnterVerse(Verse::FRunningContext& Context, TBody&& Body)
{
    FVerseEntry Entry;
    Context.EnterVM(Forward<TBody>(Body));
}

/// A diagnostic captured on the worker, owning its strings so it outlives the analysis that
/// produced it and can be replayed on the game thread.
struct FCapturedDiagnostic
{
    vh_severity Severity;
    FUtf8String Message;
    FUtf8String FilePath;
    int32 Line;
    int32 Column;
    int32 EndLine;
    int32 EndColumn;
    int32 ReferenceCode;
    /// Filled after the analysis rather than at capture; see vh_diagnostic::SubjectTypeUtf8.
    FUtf8String SubjectType;
};

struct FBackgroundCheck
{
    std::thread Thread;
    std::atomic<bool> bRunning{false};

    /// Written by the worker before bRunning clears, read by the game thread after joining, so
    /// the join is the whole synchronisation.
    TArray<FCapturedDiagnostic> Diagnostics;
    bool bResult = false;

    /// Joined but not yet delivered. Set by whoever joins, cleared by the poll that reports it.
    bool bResultPending = false;

    FUtf8String Path;
    FUtf8String SourceText;
};

FBackgroundCheck GBackgroundCheck;


/// Declared in HostScript.h since the sidecar has to name it too (HostSidecar.cpp). Everything
/// below was written when it was a file-local type, and still reads that way.
using GodotVerse::FAnalysisSnapshot;

/// What the read entry points answer from, and what the next analysis is building. Swapped on the
/// game thread; the current one is shared rather than double-buffered by index, so a descriptor
/// handed out over the ABI keeps its bytes alive for as long as its holder keeps its share.
TSharedPtr<const FAnalysisSnapshot> GSnapshot;
TSharedPtr<FAnalysisSnapshot> GPendingSnapshot;

/// Fills the semantic half into GPendingSnapshot. Defined beside the readers it reuses.
AUTORTFM_DISABLE void TakeAnalysisSnapshot();

/// Fills the VM half of GPendingSnapshot and makes it current. Game thread only.
AUTORTFM_DISABLE void PublishAnalysisSnapshot();

/// Whether the injection below took a snapshot off the build that just ran. Cleared by every
/// caller of BuildAll before it calls one, so "still false" means the toolchain never reached the
/// hook -- which is what a semantic error does: Compile_SemanticError is in CompileMask_Aborted,
/// and SemanticAnalyzeVst skips its post-analysis injections for an aborted compile.
bool GSnapshotTakenDuringBuild = false;

/// Takes the snapshot from inside the build that produced the program, rather than from a second
/// build run afterwards to produce another one.
///
/// This is where a *code-generating* build is still describable. IR generation hangs an IR package
/// off every module and puts the AST out of reach, so CompileProject used to follow every
/// successful build with a whole analysis-only pass -- ~770 ms of the ~1.6 s an author waits on
/// Play -- whose only purpose was to rebuild a program equal to the one the build had already
/// thrown away. CToolchain::SemanticAnalyzeVst invokes this hook after the last semantic pass and
/// before localization, IR generation and code generation, which is exactly that program.
///
/// Both halves of what the trailing pass was for are taken here: the snapshot, and the mirror's
/// definition table, which wants the first build's program for the same reason.
///
/// The VM half of the snapshot is *not*, and cannot be: an inspector default is read off a
/// transient instance of a generated class, and this runs three phases before there is one.
/// PublishAnalysisSnapshot stays where it is, on the game thread, after the link.
class FGodotSnapshotInjection : public uLang::IPostSemAnalysisInjection
{
public:
    AUTORTFM_DISABLE virtual bool Ingest(
        const uLang::TSRef<uLang::CSemanticProgram>& Program,
        const uLang::SProgramContext&,
        const uLang::SBuildContext&) override
    {
        // The auto-qualify pre-pass builds a whole program of its own and routes it through this
        // same hook (Toolchain.cpp, BuildProject), and that program is discarded. Everything the
        // snapshot reads goes through the build manager, so describing anything else would be
        // describing a program no later question can reach. Off by default -- the CVar is
        // Verse.AutoQualifyBeforeCompile -- and this is what keeps it off-by-accident too.
        const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager =
            GIde.IsValid() ? GIde->GetBuildManager() : nullptr;
        if (!BuildManager.IsValid() || BuildManager->GetProgramContext()._Program.Get() != Program.Get())
        {
            return false;
        }

        TakeAnalysisSnapshot();
        RecordMirrorDefinitions(*Program);
        GSnapshotTakenDuringBuild = true;
        return false; // Do not halt the toolchain.
    }
};

AUTORTFM_DISABLE vh_severity ToVhSeverity(ELogVerbosity::Type Verbosity)
{
    switch (Verbosity)
    {
    case ELogVerbosity::Error:
        return VH_SEVERITY_ERROR;
    case ELogVerbosity::Warning:
        return VH_SEVERITY_WARNING;
    default:
        return VH_SEVERITY_INFO;
    }
}

AUTORTFM_DISABLE FCapturedDiagnostic CaptureSolDiagnostic(const FSolDiagnostic& Diagnostic)
{
    return FCapturedDiagnostic{ToVhSeverity(Diagnostic.Info.Severity),
                               FUtf8String(Diagnostic.Info.Message),
                               FUtf8String(Diagnostic.Location.FilePath),
                               Diagnostic.Location.RowSpan.X,
                               Diagnostic.Location.ColSpan.X,
                               Diagnostic.Location.RowSpan.Y,
                               Diagnostic.Location.ColSpan.Y,
                               static_cast<int32>(Diagnostic.Info.ReferenceCode)};
}

AUTORTFM_DISABLE void ForwardCapturedDiagnostic(const FCapturedDiagnostic& Diagnostic)
{
    GodotVerse::ReportDiagnostic(Diagnostic.Severity,
                                 Diagnostic.Message,
                                 Diagnostic.FilePath,
                                 Diagnostic.Line,
                                 Diagnostic.Column,
                                 Diagnostic.EndLine,
                                 Diagnostic.EndColumn,
                                 Diagnostic.SubjectType,
                                 Diagnostic.ReferenceCode);
}

/// uLang's ErrSemantic_UnknownIdentifier, which is both "Unknown identifier `X`." and "Unknown
/// member `X` in `Y`." -- the only two diagnostics with a subject to find a type for.
constexpr int32 UnknownIdentifierCode = 3506;

/// Asked once per analysis, over the diagnostics it produced, because the AST the answer is read
/// off does not exist until the analysis has finished. Every other diagnostic is left alone: this
/// walks the project's AST per entry, and one glitch code is the whole of what it can answer for.
AUTORTFM_DISABLE void ResolveSubjectTypes(TArray<FCapturedDiagnostic>& Diagnostics)
{
    for (FCapturedDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.ReferenceCode == UnknownIdentifierCode)
        {
            Diagnostic.SubjectType = SubjectTypeOfDiagnostic(Diagnostic.FilePath, Diagnostic.Line, Diagnostic.Column);
        }
    }
}

/// The sink for diagnostics nothing will post-process: the project source's own, which are about
/// the project rather than about any file in it.
AUTORTFM_DISABLE void ForwardSolDiagnostic(const FSolDiagnostic& Diagnostic)
{
    ForwardCapturedDiagnostic(CaptureSolDiagnostic(Diagnostic));
}

/// The IDE *is* the compiler: CreateProjectSource and MakeDevEnvironment are two of the four
/// ISolarisModule members that WITH_VERSE_COMPILER=0 takes away. A runtime host never gets one,
/// and everything that would have used it is refused at the ABI (VH_ERR_UNSUPPORTED) long before
/// reaching here -- this returns false so that a path that somehow did cannot proceed on a null.
#if !WITH_VERSE_COMPILER
AUTORTFM_DISABLE bool EnsureIde()
{
    GodotVerse::ReportError(UTF8TEXT("This build of the Verse host has no compiler."));
    return false;
}
#else
AUTORTFM_DISABLE bool EnsureIde()
{
    if (GIde.IsValid())
    {
        return true;
    }

    // Epic's metadata attributes are declared @customattribhandler, so evaluating a module that
    // applies one asks ICustomAttributeHandler::FindHandlerForAttribute for a handler and fails
    // the whole build with "No custom handler for attribute: editable" when there is none. The
    // bridge's own attributes need no handler, but a script is free to import
    // /Verse.org/Simulation and apply one of Epic's. The handler is registered by
    // FVerseSimulationMetadataModule::StartupModule, and in a monolithic program linking the
    // module does not run that -- nothing loads it unless asked.
    FModuleManager::Get().LoadModule(TEXT("VerseSimulationMetadata"));

    // Registered for the life of the process, which is the life of the host: the handle
    // unregisters the feature when it is destroyed, and every build after that loses authorship.
    static uLang::TModularFeatureRegHandle<FGodotAuthorshipInjection> GodotAuthorship;

    // And for the life of the process for the same reason: an injection is discovered once per
    // build, out of GetModularFeaturesOfType, so one that unregisters stops being found.
    static uLang::TModularFeatureRegHandle<FGodotSnapshotInjection> GodotSnapshot;

    ISolarisModule& SolarisModule = ISolarisModule::Get();

    TOptional<TSharedRef<ISolIdeSourceProject>> MaybeSourceProject = SolarisModule.CreateProjectSource(
        TEXT("VerseHost"), ISolarisModule::EBuildMode::Incremental, MakeIdeDiagnostics(ForwardSolDiagnostic));
    if (!MaybeSourceProject.IsSet())
    {
        GodotVerse::ReportError(UTF8TEXT("Failed to create the Verse source project."));
        return false;
    }

    const FSolIdeConfig IdeConfig{.Flags = ESolIdeFlags::WithPackageUsage,
                                  .VersePath = ScriptVersePath,
                                  .VerseScope = uLang::EVerseScope::InternalUser,
                                  .VerseVersion = Verse::Version::LatestUnstable,
                                  .bAllowExperimental = true};

    TSharedRef<ISolarisIde> Ide = SolarisModule.MakeDevEnvironment(IdeConfig);

    // A build manager of our own, for one reason: the parser is a toolchain part and a toolchain is
    // what a build manager is constructed with, so caching the mirror's parse means constructing
    // one. Before SetSourceProject, which is what wires the project into whichever manager the IDE
    // is holding. If the parser feature is not registered -- nothing in the engine unregisters it,
    // but a missing one would be a null deref here rather than a slower analysis -- the IDE keeps
    // the manager MakeDevEnvironment gave it.
    if (const uLang::TOptional<uLang::TSRef<uLang::IParserPass>> Parser = uLang::GetModularFeature<uLang::IParserPass>())
    {
        uLang::SBuildManagerParams ManagerParams;
        ManagerParams._ToolchainOverrides.Parser =
            uLang::TSPtr<uLang::IParserPass>(uLang::TSRef<FGodotCachingParser>::New(*Parser));
        Ide->SetBuildManager(uLang::TSRef<uLang::CProgramBuildManager>::New(ManagerParams));
    }

    Ide->SetSourceProject(*MaybeSourceProject);
    AddAttributePackage(*Ide);
    GSourceProject = MaybeSourceProject;
    GIde = Ide;
    return true;
}
#endif // WITH_VERSE_COMPILER

/// The fourth, and the one with two call sites. A no-op without a compiler, which is correct
/// rather than merely compilable: there is no source project to incrementalize.
AUTORTFM_DISABLE void IncrementalizeProjectSource()
{
#if WITH_VERSE_COMPILER
    ISolarisModule::Get().IncrementalizeProjectSource(
        GSourceProject.GetValue(),
        uLang::SBuildParams{._LinkType = uLang::SBuildParams::ELinkParam::RequireComplete});
#endif
}

/// Retires whatever source package the project holds and puts Generation's in its place, with a
/// snippet per file. Leaves GScriptSourcePackageName, GPreparedGeneration and GPreparedSources
/// describing it.
///
/// Called as a build's *last* act rather than its first, so the analyses that follow already run
/// under the name the next publish will use -- see GPreparedGeneration.
AUTORTFM_DISABLE void PrepareGenerationPackage(uLang::CProgramBuildManager& BuildManager,
                                               int32 Generation,
                                               const TArray<GodotVerse::FScriptSource>& Sources,
                                               const TArray<FUtf8String>& Texts)
{
    RemoveScriptPackage(BuildManager, GScriptSourcePackageName);
    GScriptSnippets.Empty(Sources.Num());

    const FUtf8String PackageName =
        FUtf8String(FString::Printf(TEXT("%hs_%d"), ScriptPackageBaseName, Generation));

    AddScriptPackage(BuildManager, PackageName);
    const uLang::CSourceProject::SPackage& Package = BuildManager.FindOrAddSourcePackage(
        FULangConversionUtils::FUtf8StringToULangStr(PackageName), ScriptVersePath);

    for (int32 Index = 0; Index < Sources.Num(); ++Index)
    {
        uLang::TSRef<FHostSourceSnippet> Snippet = uLang::TSRef<FHostSourceSnippet>::New(
            FULangConversionUtils::FUtf8StringToULangStr(Sources[Index].Path),
            FULangConversionUtils::FUtf8StringToULangStr(Texts[Index]));
        GScriptSnippets.Add(Snippet);
        FindOrAddModule(*Package._Package->_RootModule, Sources[Index].ModulePath).AddSnippet(Snippet);
    }

    GScriptSourcePackageName = PackageName;
    GPreparedGeneration = Generation;
    GPreparedSources = Sources;
}

/// Settles which packages the *next* parse reads from a digest and which it reads from source.
///
/// Run after a build rather than before it. The first IncrementalizeProjectSource could not retire
/// a native package: it leaves a VNI package Source while `IsCompiled(Name) == None`, which is true
/// right up until the build that registers its bindings. So the roles a session ran under used to
/// change under it at the *second* build, where /Verse.org and /Godot.org/Godot both went External
/// at once and an analysis went from ~1300 ms to ~750 ms. This is where that happens now: at every
/// build, including the first.
///
/// Unconditional, because a failed build is exactly the case where being selective would be wrong:
/// what deployed is what IsCompiled reports, and a package the build never got to stays Source on
/// its own.
///
/// Then: what it must not be allowed to retire. **Only the generation's own package** --
/// phase-2-design.md 181-190 measured 104 failing cases with it retired. The mirror is held Source
/// only until the definition table has taken what a digest does not carry, which is the first
/// build's own semantic analysis, so from the first build on it goes External and an analysis
/// costs ~520 ms rather than ~1450 ms.
///
/// **The attribute package used to be kept Source here and no longer is**, and the reason it was
/// no longer holds: a build generates a digest for every Source package it compiles, this one
/// included -- 2175 bytes of it, which VH_TRACE_ANALYSIS prints beside the package -- so an
/// External attribute package contributes its definitions the way any other digest does and
/// `@export` keeps resolving. Leaving it Source is what made a build unable to reuse an analysis:
/// the assembler publishes every Source package the program carries, and publishing one twice is
/// `!ObjectItem->HasAnyFlags(EInternalObjectFlags::LoaderImport)` in AsyncLoading2.cpp, which is a
/// crash and not a diagnostic.
AUTORTFM_DISABLE void RetirePackagesAfterBuild(uLang::CProgramBuildManager& BuildManager)
{
    IncrementalizeProjectSource();

    for (const uLang::CSourceProject::SPackage& Kept : BuildManager.GetSourceProject()->_Packages)
    {
        const uLang::CUTF8String& VersePathOf = Kept._Package->GetSettings()._VersePath;
        const FUtf8StringView Name(Kept._Package->GetName().AsCString());
        const bool bIsAttributePackage = Name.Equals(FUtf8StringView(AttributePackageName));
        const bool bIsMirror = !bIsAttributePackage
            && FUtf8StringView(VersePathOf.AsCString()).Equals(FUtf8StringView(GodotVersePath));
        if (Name.Equals(FUtf8StringView(GScriptSourcePackageName))
            || (bIsMirror && !MirrorDefinitionsRecorded()))
        {
            Kept._Package->SetRole(uLang::EPackageRole::Source);
        }
    }
}

/// Generates and publishes from the program the IDE is already holding: the last three phases of a
/// build, with the two that produced the program skipped.
///
/// `FSolarisIde::BuildAll` is not usable for this -- it goes through `CProgramBuildManager::Build`,
/// which calls `ResetSemanticProgram()` first and would throw away the very thing being reused. So
/// the phases are driven directly, and what BuildAll does around them is reproduced here:
///
///   - **`SetBlockExecution`**, which is the safety-critical half. VerseVM refuses to run while a
///     build is in flight and ticking anyway trips `ensure(!bBlockAllExecution)` and takes the
///     process down.
///   - the two FN version gates, read from the CVars `FSolarisIde::BuildAll` reads them from, so a
///     project compiled this way is compiled under the same language-version rules as one compiled
///     the long way.
///
/// What is *not* reproduced is BuildAll's tail -- the verse-path registry, the cached per-package
/// diagnostics and statistics, the plugin dependency map. Each is fed by an injection that runs
/// during **semantic analysis**, which this path does not run: the analysis that produced this
/// program already fed them, and its own BuildAll already emptied them. There is nothing left for
/// a tail to do.
///
/// Localization extraction is skipped with them, for the same reason and one more: nothing in this
/// bridge reads `TakeLocalizationInfo`.
AUTORTFM_DISABLE bool GenerateFromHeldProgram(uLang::CProgramBuildManager& BuildManager,
                                              TArray<FCapturedDiagnostic>& OutDiagnostics)
{
#if !WITH_VERSE_COMPILER
    return false;
#else
    const auto CVarInt = [](const TCHAR* Name, int32 Fallback) {
        IConsoleVariable* const Variable = IConsoleManager::Get().FindConsoleVariable(Name);
        return Variable ? Variable->GetInt() : Fallback;
    };

    uLang::SBuildContext Context(CreateDiagnostics(MakeIdeDiagnostics(
        [&OutDiagnostics](const FSolDiagnostic& Diagnostic) { OutDiagnostics.Add(CaptureSolDiagnostic(Diagnostic)); })));
    Context._Params = uLang::SBuildParams{
        ._UploadedAtFNVersion =
            static_cast<uint32_t>(CVarInt(TEXT("Verse.UploadedAtFNVersion"), VerseFN::UploadedAtFNVersion::Latest)),
        ._CurrentFNVersion =
            static_cast<uint32_t>(CVarInt(TEXT("Verse.CurrentFNVersion"), VerseFN::UploadedAtFNVersion::Latest)),
        ._LinkType = uLang::SBuildParams::ELinkParam::RequireComplete,
        ._bGenerateDigests = false,
        ._bGenerateCode = true};

    const bool bVerseWasBlocked = verse::FExecutionContext::SetBlockExecution(true);
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager.GetProgramContext()._Program;

    uLang::ECompilerResult Result = BuildManager.IrGenerateProgram(Program, Context);
    if (!uLang::IsAbortedCompile(Result))
    {
        Result |= BuildManager.AssembleProgram(Program, Context);
    }
    const bool bGenerated = !uLang::IsAbortedCompile(Result) && !Context._Diagnostics->HasErrors();
    const uLang::ELinkerResult Linked =
        bGenerated ? BuildManager.Link(Context) : uLang::ELinkerResult::Link_Skipped;

    verse::FExecutionContext::SetBlockExecution(bVerseWasBlocked);

    return bGenerated && Linked != uLang::ELinkerResult::Link_Failure && !Context._Diagnostics->HasErrors();
#endif
}

/// Whether the program the IDE is holding is already this build's answer.
///
/// Every clause is a way the held program could describe something other than the files on disk,
/// and there is no room for a "close enough": what this decides is whether a *publish* skips the
/// analysis that would have checked it.
///
///   - it has to have come from an analysis rather than from a build, because code generation
///     leaves an IR package on every module and nothing can be generated from it twice;
///   - that analysis has to have finished clean, because the slow path is what reports a
///     diagnostic at the line the author is looking at;
///   - the package has to be named for the generation this build will publish, which is what
///     PrepareGenerationPackage arranges;
///   - the project's files have to be the same files, in the same order, in the same modules --
///     `vh_compile_project` re-enumerates res:// every time and a file added, renamed or deleted
///     lands here;
///   - and every one of them has to say on disk exactly what the analysis read. An analysis reads
///     the editor's *buffer*; a build publishes what is in the file. Godot saves before running
///     only while `run/auto_save/save_before_running` is on, so the two really do come apart --
///     and this is what keeps a build meaning the same thing either way.
AUTORTFM_DISABLE bool HeldProgramIsThisBuild(const TArray<GodotVerse::FScriptSource>& Sources,
                                             const TArray<FUtf8String>& Texts)
{
    if (!GProjectBuilt || !GProgramIsAnalysisOnly || !GLastAnalysisClean)
    {
        return false;
    }
    if (GPreparedGeneration != GScriptGeneration + 1)
    {
        return false;
    }
    if (GPreparedSources.Num() != Sources.Num() || GScriptSnippets.Num() != Sources.Num())
    {
        return false;
    }

    for (int32 Index = 0; Index < Sources.Num(); ++Index)
    {
        if (GPreparedSources[Index].Path != Sources[Index].Path
            || GPreparedSources[Index].ModulePath != Sources[Index].ModulePath)
        {
            return false;
        }
        const uLang::TOptional<uLang::CUTF8String> Held = GScriptSnippets[Index]->GetText();
        if (!Held.IsSet() || FULangConversionUtils::ULangStrToFUtf8String(*Held) != Texts[Index])
        {
            return false;
        }
    }
    return true;
}

} // namespace

AUTORTFM_DISABLE uLang::CSemanticProgram* GodotVerse::CurrentSemanticProgram()
{
    if (!GIde.IsValid())
    {
        return nullptr;
    }
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager();
    return BuildManager.IsValid() ? BuildManager->GetProgramContext()._Program.Get() : nullptr;
}

AUTORTFM_DISABLE bool GodotVerse::ScriptSnippetText(FUtf8StringView Path, FUtf8String& OutText)
{
    for (const uLang::TSRef<FHostSourceSnippet>& Candidate : GScriptSnippets)
    {
        if (!FULangConversionUtils::ULangStrToFUtf8String(Candidate->GetPath()).Equals(FUtf8String(Path), ESearchCase::IgnoreCase))
        {
            continue;
        }
        const uLang::TOptional<uLang::CUTF8String> Text = Candidate->GetText();
        if (!Text.IsSet())
        {
            return false;
        }
        OutText = FULangConversionUtils::ULangStrToFUtf8String(*Text);
        return true;
    }
    return false;
}

AUTORTFM_DISABLE const uLang::CClass* GodotVerse::FindScriptClassLive(FUtf8StringView ClassName)
{
    const uLang::CSemanticProgram* const Program = CurrentSemanticProgram();
    return Program ? FindScriptClass(*Program, ClassName) : nullptr;
}

/// Read once: GetEnvironmentVariable allocates, and the analysis this is asked about is the
/// per-keystroke path.
AUTORTFM_DISABLE bool GodotVerse::AnalysisTraceEnabled()
{
    static const bool bEnabled = !FPlatformMisc::GetEnvironmentVariable(TEXT("VH_TRACE_ANALYSIS")).IsEmpty();
    return bEnabled;
}

AUTORTFM_DISABLE bool GodotVerse::EnterContentScope()
{
    if (GProjectScopeGuard.IsSet())
    {
        return true;
    }

    VerseComputationLimitControl::SetComputationLimits(false);

    // Verse needs a UObject outer to instantiate into and we have no UWorld, so synthesize one --
    // one, for the process. A content scope holds its outer weakly and every instance scope shares
    // this one, so a project with a thousand awaiting nodes roots no more UObjects than a project
    // with none.
    GScopeOuter = UPlaceholderObjectForContentScope::MakeRooted();
    GProjectScope = verse::MakeContentScope(GScopeOuter);
    GProjectScopeGuard.Emplace(GProjectScope.ToSharedRef());
    return true;
}

AUTORTFM_DISABLE void GodotVerse::LeaveContentScope()
{
    GProjectScopeGuard.Reset();
    GProjectScope.Reset();
}

AUTORTFM_DISABLE void GodotVerse::ResetScriptState()
{
    LeaveContentScope();
    // Before the IDE goes: a pending snapshot describes a program that is about to stop existing,
    // and a worker joined at shutdown leaves one nothing will ever publish.
    GSnapshot.Reset();
    GPendingSnapshot.Reset();
    GScriptSnippets.Empty();
    GSourceProject.Reset();
    GIde.Reset();
    ForgetMirrorDefinitions();
    GScriptGeneration = 0;
    GScriptPackageName.Empty();
    GScriptSourcePackageName.Empty();
    GBindingsPackageName.Empty();
    GBindingsSource.Empty();
    GBindingsDirty = false;
    GBindingByGodotClass.Empty();
    GBindingByScriptClass.Empty();
    GMintNameByBinding.Empty();
    GPreparedGeneration = 0;
    GPreparedSources.Empty();
    GLastAnalysisClean = false;
    GProjectBuilt = false;
}

namespace {

/// Builds the three lookups out of the rows, replacing whatever was there.
///
/// Shared by the editor's `SetBindings` and by an exported game's `AdoptCookedBindings`, which are
/// handed the same rows from two different places -- the consumer's enumeration, and the sidecar
/// the cook wrote it into. One indexer, so the two cannot disagree about what a row means.
AUTORTFM_DISABLE void IndexBindings(const TArray<GodotVerse::FBindingClass>& Classes)
{
    GBindingByGodotClass.Empty(Classes.Num());
    GBindingByScriptClass.Empty(Classes.Num());
    GMintNameByBinding.Empty(Classes.Num());
    GBindingRows = Classes;
    for (const GodotVerse::FBindingClass& Binding : Classes)
    {
        if (Binding.VerseClass.IsEmpty())
        {
            continue;
        }
        if (!Binding.GodotClass.IsEmpty())
        {
            GBindingByGodotClass.Add(Binding.GodotClass, Binding.VerseClass);
            GMintNameByBinding.Add(Binding.VerseClass, Binding.GodotClass);
        }
        else if (!Binding.ScriptClass.IsEmpty())
        {
            GBindingByScriptClass.Add(Binding.ScriptClass, Binding.VerseClass);
            GMintNameByBinding.Add(Binding.VerseClass, Binding.ScriptClass);
        }
    }
}

} // namespace

AUTORTFM_DISABLE const TArray<GodotVerse::FBindingClass>& GodotVerse::GetBindingClasses()
{
    return GBindingRows;
}

AUTORTFM_DISABLE void GodotVerse::AdoptCookedBindings(TArray<FBindingClass>&& Classes,
    FUtf8StringView PackageName)
{
    IndexBindings(Classes);
    // The package the *cook* published, rather than one this process prepared: a runtime host has
    // no build manager and never names a generation of its own, so this is the only way
    // FindBindingClass has a package to look in.
    GBindingsPackageName = FUtf8String(PackageName);
    ForgetCachedClasses();
}

AUTORTFM_DISABLE void GodotVerse::SetBindings(const FUtf8String& Source, TArray<FBindingClass>&& Classes)
{
    // The table is replaced whatever the source says -- a consumer may hand over the same Verse
    // with a different mapping, which is what happens when a script keeps its `class_name` and
    // Godot renumbers nothing.
    IndexBindings(Classes);

    // Only a *changed* source costs a package name. A roster whose Verse text is identical -- a
    // scene reload, a project reopen -- must not, or a session that reopens often exhausts them.
    if (GBindingsSource != Source)
    {
        GBindingsSource = Source;
        GBindingsDirty = true;
    }

    // A roster change is exactly when a handle's answer changes, and the class cache is keyed per
    // handle for the life of the process. The peer cache goes with it: what a script class mints
    // depends on whether a binding now sits between it and the mirror.
    ForgetCachedClasses();
}

AUTORTFM_DISABLE bool GodotVerse::CompileProject(const TArray<FScriptSource>& Sources, int32& OutGeneration)
{
    if (!EnsureIde())
    {
        return false;
    }

    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager();
    if (!BuildManager.IsValid())
    {
        ReportError(UTF8TEXT("No build manager; the project cannot be built."));
        return false;
    }

    // Read every file before touching the project, so a missing one leaves the previous
    // generation's source exactly as it was rather than half replaced.
    TArray<FUtf8String> Texts;
    Texts.Reserve(Sources.Num());
    for (const FScriptSource& Source : Sources)
    {
        FString SourceText;
        if (!FFileHelper::LoadFileToString(SourceText, *FString(Source.Path)))
        {
            ReportError(FUtf8String(TEXT("Failed to open Verse source file: ")) + Source.Path);
            return false;
        }
        Texts.Add(FUtf8String(SourceText));
    }

    const int32 Generation = GScriptGeneration + 1;

    // The program the last analysis left is this build's, or it is not; either way no analysis may
    // re-arm the reuse until one has run again, so the flag is cleared before anything else can
    // read it.
    // A pending bindings package makes the held program stale whatever the sources say: it was
    // analysed against a roster this build does not have.
    const bool bReuseHeldProgram = !GBindingsDirty && HeldProgramIsThisBuild(Sources, Texts);
    GLastAnalysisClean = false;

    FAnalysisTrace Trace;
    const double BuildStarted = FPlatformTime::Seconds();
    // Held rather than forwarded as they arrive, for the reason CheckProject holds its own: the
    // subject type each one may carry is read off the AST, and a build that succeeds generates code
    // and puts the AST out of reach. That costs nothing here: the one diagnostic with a subject to
    // find is ErrSemantic_UnknownIdentifier, which is an error, and a build with an error is a
    // build that failed -- where the AST is still whole, because it never reached codegen.
    TArray<FCapturedDiagnostic> BuildDiagnostics;
    GSnapshotTakenDuringBuild = false;

    bool bBuilt = false;
    if (bReuseHeldProgram)
    {
        // The snapshot, taken here rather than by the injection: this path runs no semantic
        // analysis, so the hook the injection lives on never fires. Before IR generation, which is
        // what puts the AST out of reach.
        TakeAnalysisSnapshot();
        GSnapshotTakenDuringBuild = true;

        bBuilt = GenerateFromHeldProgram(*BuildManager, BuildDiagnostics);

        // No IncrementalizeProjectSource around this one, and no role loop below it. Both exist to
        // decide what the *parse* reads, and nothing was parsed: the roles the last slow build left
        // are still the ones the last analysis ran under. Calling it here would be worse than
        // pointless -- the script package is compiled now, so it would be retired to a digest this
        // path never generated, which is a checkf inside Solaris rather than a diagnostic.
    }
    else
    {
        // Before PrepareGenerationPackage, which is what takes the script package's dependency
        // list fresh -- a bindings package added after it would be depended on by nothing.
        FlushPendingBindings(*BuildManager);
        PrepareGenerationPackage(*BuildManager, Generation, Sources, Texts);

        // Without this the build republishes the native VNI packages -- which are already loaded --
        // and aborts inside the async loader. It marks everything already compiled External so that
        // only the new generation's package is built.
        IncrementalizeProjectSource();

        FSolIdeBuildSettings Settings{.LinkSettings = uLang::SBuildParams::ELinkParam::RequireComplete};
        bBuilt = GIde->BuildAll(
            Settings,
            MakeIdeDiagnostics([&BuildDiagnostics](const FSolDiagnostic& Diagnostic) { BuildDiagnostics.Add(CaptureSolDiagnostic(Diagnostic)); },
                               [&Trace](const uLang::SBuildEventInfo& Event) { Trace.OnEvent(Event); }));

        RetirePackagesAfterBuild(*BuildManager);
    }

    auto ForwardBuildDiagnostics = [&BuildDiagnostics] {
        ResolveSubjectTypes(BuildDiagnostics);
        for (const FCapturedDiagnostic& Diagnostic : BuildDiagnostics)
        {
            ForwardCapturedDiagnostic(Diagnostic);
        }
    };

    if (AnalysisTraceEnabled())
    {
        PrintAnalysisTrace(Trace, bReuseHeldProgram ? "generation (from the held program)" : "generation",
                           FPlatformTime::Seconds() - BuildStarted);
    }
    GProjectBuilt = true;
    if (!bBuilt)
    {
        // Nothing was published, so GScriptPackageName still names the last generation that was:
        // its classes keep resolving and its instances keep running, which is R-ITER-5. The
        // failed generation's source stays in the project, because it is what the next analysis
        // reports diagnostics against.
        //
        // The snapshot is taken anyway, off the program the failed build left. uLang recovers and
        // carries on, so that program still describes most of what the author wrote, and this is
        // the only description there is until the next keystroke starts an analysis -- a first
        // build that fails would otherwise leave every class-describing read answering not-found.
        //
        // The injection is what usually took it, and it is exactly a *semantic* failure that it
        // did not: an error aborts the compile before the hook. So this stays, for that case.
        if (!GSnapshotTakenDuringBuild)
        {
            TakeAnalysisSnapshot();
        }
        PublishAnalysisSnapshot();
        ForwardBuildDiagnostics();
        return false;
    }

    GScriptGeneration = Generation;
    GScriptPackageName = GScriptSourcePackageName;
    OutGeneration = Generation;

    IVerseModule::Get(); // Runs VerseModule::StartupModule; VerseCmd does the same before calling in.

    // The build just done generated code, which leaves an IR package on every module and puts the
    // AST out of reach -- so what the editor reads about this project is the snapshot the build's
    // own semantic analysis left, and the three entry points that resolve a *position* have
    // nothing to resolve against until the next analysis. Both halves of that are said here: the
    // program is no longer analysis-only, and it no longer describes any buffer.
    //
    // This used to be a whole analysis-only pass over the same sources, run for no other reason
    // than to rebuild a program equal to the one the build had just discarded -- ~770 ms of the
    // ~1.6 s between Play and the game, every time. The consumer asks for a fresh analysis after a
    // build instead, which costs the same work off the critical path and only when there is an
    // editor to want it.
    GProgramIsAnalysisOnly = false;
    GAnalysedPath.Empty();
    GAnalysedSource.Empty();

    if (GSnapshotTakenDuringBuild)
    {
        // All that is left of the snapshot: an inspector default is read off a transient instance
        // of a generated class, so this half could not run until the link above made one.
        PublishAnalysisSnapshot();
    }
    else
    {
        // The fallback, for a build that somehow reached code generation without the hook -- uLang
        // would have had to skip the injections rather than abort, which nothing here does. Cheap
        // to keep, and what it avoids is a session describing a program nothing ever walked.
        RunCheck(FUtf8String(), FUtf8String(), [](const FSolDiagnostic&) {});
    }

    ForwardBuildDiagnostics();

    // The injection recorded the table; this is the role change that follows it, and it is a
    // build's last word on the source project. Only reached by a build that succeeded, which is
    // also the only kind that leaves a package the compiler will accept a digest of.
    RecordAndRetireMirror();

    // And the *next* generation's package, prepared now rather than at the start of the next build
    // -- which is what lets that build be the cheap one. Every analysis from here runs under the
    // name the next publish will use, so an analysis that lands with nothing edited after it is a
    // program a publish can be generated from directly. HeldProgramIsThisBuild is the whole test.
    //
    // The texts are the ones just built, which is what the files say; an analysis replaces one of
    // them with the editor's buffer as the author types, and that is exactly what the test catches.
    PrepareGenerationPackage(*BuildManager, Generation + 1, Sources, Texts);

    return true;
}

namespace {

/// The analysis itself, with the diagnostics sink left to the caller: the foreground path
/// forwards straight to Godot, the background one captures for replay on the game thread.
AUTORTFM_DISABLE bool RunCheck(const FUtf8String& Path, const FUtf8String& SourceText, TFunction<void(const FSolDiagnostic&)> Sink)
{
    if (!GProjectBuilt || !GIde.IsValid())
    {
        return false;
    }

    if (const uLang::TSPtr<uLang::CProgramBuildManager> Manager = GIde->GetBuildManager(); Manager.IsValid())
    {
        FlushPendingBindings(*Manager);
    }

    for (const uLang::TSRef<FHostSourceSnippet>& Snippet : GScriptSnippets)
    {
        if (FUtf8String(Snippet->GetPath().AsCString()) == Path)
        {
            Snippet->SetText(FULangConversionUtils::FUtf8StringToULangStr(SourceText));
            break;
        }
    }

    // What cannot happen twice in a process is a build that *generates*. Publishing a package marks
    // each of its exports with EInternalObjectFlags::LoaderImport, and publishing that package again
    // asserts on the flag the previous publish left -- for any package, not just the native ones,
    // which are merely the first to get there. README.md's first constraint has the whole finding,
    // including why WITH_EDITOR is what decides it. A build that only analyses publishes nothing and
    // can be run as often as the editor types.
    FSolIdeBuildSettings Settings{.LinkSettings = uLang::SBuildParams::ELinkParam::RequireComplete};
    Settings.bSemanticAnalysisOnly = true;
    Settings.bGenerateDigests = false;
    Settings.bGenerateCode = false;
    Settings.bGenerateAutoRTFMBytecode = false;

    FAnalysisTrace Trace;
    const double AnalysisStarted = FPlatformTime::Seconds();
    GSnapshotTakenDuringBuild = false;
    const bool bAnalysed = GIde->BuildAll(
        Settings,
        MakeIdeDiagnostics(MoveTemp(Sink),
                           [&Trace](const uLang::SBuildEventInfo& Event) { Trace.OnEvent(Event); }));
    if (AnalysisTraceEnabled())
    {
        // The worker clears bRunning only once RunCheck has returned, so the flag still names which
        // thread this analysis is on.
        PrintAnalysisTrace(Trace,
                           GBackgroundCheck.bRunning.load(std::memory_order_acquire) ? "analysis (background)"
                                                                                     : "analysis (foreground)",
                           FPlatformTime::Seconds() - AnalysisStarted);
    }
    GProgramIsAnalysisOnly = GProgramIsAnalysisOnly || bAnalysed;

    // What lets the next build skip straight to code generation, if nothing is edited before it
    // comes. Only a clean analysis arms it, and only a build disarms it.
    GLastAnalysisClean = bAnalysed;

    // Whatever the result, the program now describes this text. Recorded so that the two
    // buffer-taking entry points -- completion and the argument hint -- can skip re-analysing
    // when the editor asks both about one keystroke, which it does on every call it is inside.
    GAnalysedPath = Path;
    GAnalysedSource = SourceText;

    // Here rather than at the call sites, so that every road to a fresh program leaves a fresh
    // snapshot behind it. On the worker the swap waits for the game thread; in the foreground this
    // *is* the game thread, and publishing now is what the editor reads until the next keystroke.
    //
    // Usually already done: FGodotSnapshotInjection takes it mid-analysis, off the same program.
    // What lands here instead is the analysis that stopped at a semantic error, which is most of
    // them while an author is typing -- and that program is still the one to describe, because it
    // is the only one there is.
    if (!GSnapshotTakenDuringBuild)
    {
        TakeAnalysisSnapshot();
    }
    if (!GBackgroundCheck.bRunning.load(std::memory_order_acquire))
    {
        PublishAnalysisSnapshot();
    }
    return bAnalysed;
}

/// Whether the program already describes this exact buffer, so an analysis of it would be work
/// for the same answer. Only safe for a caller that wants the *program*: an analysis also reports
/// diagnostics, and skipping it skips those too.
AUTORTFM_DISABLE bool ProgramAlreadyDescribes(const FUtf8String& Path, const FUtf8String& SourceText)
{
    return !GAnalysedPath.IsEmpty() && GAnalysedPath == Path && GAnalysedSource == SourceText;
}

/// Body of the background thread. A free function rather than a lambda so it can carry
/// AUTORTFM_DISABLE like everything else that reaches Solaris.
AUTORTFM_DISABLE void BackgroundCheckMain()
{
    GBackgroundCheck.bResult = RunCheck(
        GBackgroundCheck.Path,
        GBackgroundCheck.SourceText,
        [](const FSolDiagnostic& Diagnostic) { GBackgroundCheck.Diagnostics.Add(CaptureSolDiagnostic(Diagnostic)); });

    // On this thread, which is the one that just built the AST being read -- and before bRunning
    // clears, so the game thread cannot start another analysis under it.
    ResolveSubjectTypes(GBackgroundCheck.Diagnostics);

    // Last, so the game thread never observes bRunning false with the results half written.
    GBackgroundCheck.bRunning.store(false, std::memory_order_release);
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::CheckProject(const FUtf8String& Path, const FUtf8String& SourceText)
{
    WaitForBackgroundCheck();

    // Held rather than forwarded as they arrive: a diagnostic's subject type is read off the AST,
    // and the AST is only whole once the analysis raising them has finished.
    TArray<FCapturedDiagnostic> Diagnostics;
    const bool bResult =
        RunCheck(Path, SourceText, [&Diagnostics](const FSolDiagnostic& Diagnostic) { Diagnostics.Add(CaptureSolDiagnostic(Diagnostic)); });

    ResolveSubjectTypes(Diagnostics);
    for (const FCapturedDiagnostic& Diagnostic : Diagnostics)
    {
        ForwardCapturedDiagnostic(Diagnostic);
    }
    return bResult;
}

AUTORTFM_DISABLE bool GodotVerse::ProgramDescribes(const FUtf8String& Path, const FUtf8String& SourceText)
{
    // The order is the whole of the thread safety: the worker writes GAnalysedPath/Source inside
    // RunCheck and clears bRunning afterwards with a release store, so reading the atomic first and
    // finding it false is what makes those writes visible here. Reversing these two lines would be
    // a race with no symptom until an analysis lands mid-comparison.
    return !GBackgroundCheck.bRunning.load(std::memory_order_acquire) && ProgramAlreadyDescribes(Path, SourceText);
}

AUTORTFM_DISABLE bool GodotVerse::ProgramIsAnalysisOnly()
{
    return GProgramIsAnalysisOnly;
}

namespace {

/// Records every name Module declares against Module's own path, then recurses. Path is relative
/// to the package root, and an empty one is the root module itself -- which is skipped, because it
/// is in scope from everywhere and so is never the answer to "what should I import".
///
/// Built once per analysis and inverted, where ResolveUnknownName used to walk the AST per query:
/// the walk reads uLang's AST project, which the background worker rebuilds under it, so asking at
/// query time was a race as well as a cost.
AUTORTFM_DISABLE void CollectModuleDeclarations(const uLang::CModule& Module,
                                                const FUtf8String& Path,
                                                TMap<FUtf8String, TArray<FUtf8String>>& Out)
{
    if (!Path.IsEmpty())
    {
        for (const uLang::TSRef<uLang::CDefinition>& Definition : Module.GetDefinitions())
        {
            Out.FindOrAdd(FUtf8String(Definition->AsNameCString())).AddUnique(Path);
        }
    }

    for (const uLang::CModule* Submodule : Module.GetDefinitionsOfKind<uLang::CModule>())
    {
        const FUtf8String Name8 = FUtf8String(Submodule->AsNameCString());
        CollectModuleDeclarations(*Submodule, Path.IsEmpty() ? Name8 : Path + UTF8TEXT("/") + Name8, Out);
    }
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::ResolveUnknownName(FUtf8StringView Name, TArray<FUtf8String>& OutModules)
{
    OutModules.Empty();
    if (!GSnapshot || !GSnapshot->bAstAvailable || Name.IsEmpty())
    {
        return false;
    }
    if (const TArray<FUtf8String>* const Modules = GSnapshot->ModulesDeclaring.Find(FUtf8String(Name)))
    {
        OutModules = *Modules;
    }
    return true;
}

AUTORTFM_DISABLE bool GodotVerse::BeginBackgroundCheck(const FUtf8String& Path, const FUtf8String& SourceText)
{
    if (GBackgroundCheck.Thread.joinable() || GBackgroundCheck.bRunning.load(std::memory_order_acquire)
        || GBackgroundCheck.bResultPending)
    {
        return false;
    }
    if (!GProjectBuilt || !GIde.IsValid())
    {
        return false;
    }

    GBackgroundCheck.Path = Path;
    GBackgroundCheck.SourceText = SourceText;
    GBackgroundCheck.Diagnostics.Empty();
    GBackgroundCheck.bResult = false;
    GBackgroundCheck.bRunning.store(true, std::memory_order_release);
    GBackgroundCheck.Thread = std::thread(&BackgroundCheckMain);
    return true;
}

AUTORTFM_DISABLE bool GodotVerse::IsBackgroundCheckRunning()
{
    return GBackgroundCheck.bRunning.load(std::memory_order_acquire);
}

namespace {

/// Joins the worker, leaving what it captured queued for the next poll. Joining is only about
/// making it safe to touch Verse again; delivering the result is PollBackgroundCheck's job, and
/// splitting the two is what stops a wait from swallowing an analysis the caller never heard
/// about -- it would drop those diagnostics and leave the caller asking for the same analysis
/// again on the next keystroke.
AUTORTFM_DISABLE void JoinBackgroundCheck()
{
    if (GBackgroundCheck.Thread.joinable())
    {
        GBackgroundCheck.Thread.join();
        GBackgroundCheck.bResultPending = true;
    }
}

/// The two things `@statics` was chosen over a naming convention to make checkable (R-NODE-4).
/// Defined further down, beside GetClassStatics, whose walk and helpers it shares.
AUTORTFM_DISABLE void ReportStaticsDiagnostics();

} // namespace

AUTORTFM_DISABLE bool GodotVerse::PollBackgroundCheck(bool& OutFinished)
{
    OutFinished = false;
    if (GBackgroundCheck.bRunning.load(std::memory_order_acquire))
    {
        return true;
    }
    JoinBackgroundCheck();

    // Before the diagnostics, and outside the bResultPending gate, because a wait may already have
    // joined the worker and taken the result-pending flag with it: what decides there is something
    // to publish is the snapshot the worker left, not whether this poll is the one that reaps it.
    PublishAnalysisSnapshot();

    if (!GBackgroundCheck.bResultPending)
    {
        return true;
    }

    for (const FCapturedDiagnostic& Diagnostic : GBackgroundCheck.Diagnostics)
    {
        ForwardCapturedDiagnostic(Diagnostic);
    }
    GBackgroundCheck.Diagnostics.Empty();
    GBackgroundCheck.bResultPending = false;

    // After the compiler's own, and through the same channel: these are read off the semantic
    // program the analysis just left behind, so this is the first moment they can be asked.
    ReportStaticsDiagnostics();

    OutFinished = true;
    return GBackgroundCheck.bResult;
}

AUTORTFM_DISABLE void GodotVerse::WaitForBackgroundCheck()
{
    // Only a join that blocked is a stall worth reporting: a caller arriving after the worker has
    // finished joins a dead thread in microseconds, and counting those would make the figure a call
    // count rather than the main thread's lost time.
    const bool bWasRunning = GBackgroundCheck.bRunning.load(std::memory_order_acquire);
    const double WaitStarted = bWasRunning ? FPlatformTime::Seconds() : 0.0;

    JoinBackgroundCheck();

    if (bWasRunning)
    {
        ++GAnalysisWaitCount;
        GAnalysisWaitSeconds += FPlatformTime::Seconds() - WaitStarted;
    }
}

AUTORTFM_DISABLE void GodotVerse::TakeAnalysisWaitStats(int32& OutCount, double& OutSeconds)
{
    OutCount = GAnalysisWaitCount;
    OutSeconds = GAnalysisWaitSeconds;
    GAnalysisWaitCount = 0;
    GAnalysisWaitSeconds = 0.0;
}

namespace {
/// FVerseFunction's package constructor dereferences the result of LookupPackage without checking
/// it, so asking for a function when the build failed crashes rather than returning invalid.
AUTORTFM_DISABLE bool ScriptPackageLoaded()
{
    return Verse::GlobalProgram && Verse::GlobalProgram->LookupPackage(GScriptPackageName) != nullptr;
}
}

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
        Instance.Scope = verse::MakeContentScope(GScopeOuter);
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

/// The UClass behind a top-level Verse class in a named package, or null if there is no such class
/// or it does not derive from object. A class that does not derive from object has no
/// native representation at all, so `Cast<UClass>` is itself most of the check.
AUTORTFM_DISABLE UClass* FindClassInPackage(FUtf8StringView PackageName, const char* VersePath, FUtf8StringView ClassName)
{
    Verse::VPackage* Package = Verse::GlobalProgram ? Verse::GlobalProgram->LookupPackage(FUtf8String(PackageName)) : nullptr;
    if (!Package)
    {
        return nullptr;
    }

    // A qualified name decorates as (scope:)leaf, and the module is part of the scope:
    // `gameplay/player` is `(/user@localhost/gameplay:)player`. A root-module class has no module
    // to add and decorates exactly as it did before modules existed.
    FUtf8String Scope(VersePath);
    FUtf8String Leaf(ClassName);
    int32 LastSlash = INDEX_NONE;
    if (Leaf.FindLastChar(UTF8CHAR('/'), LastSlash))
    {
        Scope += UTF8TEXT("/");
        Scope += Leaf.Left(LastSlash);
        Leaf.RightChopInline(LastSlash + 1);
    }

    const FUtf8String Decorated = FUtf8String(UTF8TEXT("(")) + Scope + UTF8TEXT(":)") + Leaf;

    UClass* Found = nullptr;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    EnterVerse(Context, [&] {
        Verse::VClass* Class = Package->LookupDefinition<Verse::VClass>(FUtf8StringView(Decorated));
        if (!Class)
        {
            return;
        }
        UClass* NativeClass = Cast<UClass>(Class->GetOrCreateNativeType(Context));
        if (NativeClass && NativeClass->IsChildOf(verse::vh_object::StaticClass()))
        {
            Found = NativeClass;
        }
    });
    return Found;
}

} // namespace

AUTORTFM_DISABLE UClass* GodotVerse::FindGodotClass(FUtf8StringView ClassName)
{
    return FindClassInPackage(GScriptPackageName, ScriptVersePath, ClassName);
}

AUTORTFM_DISABLE UClass* GodotVerse::FindBindingClass(FUtf8StringView ClassName)
{
    return GBindingsPackageName.IsEmpty()
        ? nullptr
        : FindClassInPackage(GBindingsPackageName, BindingsVersePath, ClassName);
}

AUTORTFM_DISABLE bool GodotVerse::HasClass(FUtf8StringView ClassName)
{
    if (GSnapshot)
    {
        if (const FAnalysisSnapshot::FClass* const Found = GSnapshot->Classes.Find(FUtf8String(ClassName)))
        {
            return Found->bInPublishedProgram;
        }
    }

    // A name the last analysis did not declare is not the same as one the published generation does
    // not carry: renaming a class in an unsaved buffer leaves the old name in the VM and takes it
    // out of the program. Asking the VM is the honest answer, and it is a lookup -- but it is an
    // entry into the VM, which a running build forbids.
    return !IsBackgroundCheckRunning() && FindGodotClass(ClassName) != nullptr;
}

namespace {
/// The bridge's own attributes, declared in AttributePackageSource above and so sharing the verse
/// path a script already imports. The section ones name the attribute *class* rather than the
/// `<constructor>` function beside it: GetAttributeTextValue matches on the invocation's return
/// type, and a single string argument is the one attribute payload SOL-972 leaves readable.
constexpr const char* ExportAttributePath = "/Godot.org/Godot/export";
constexpr const char* ExportSignalAttributePath = "/Godot.org/Godot/export_signal";
constexpr const char* StaticsAttributePath = "/Godot.org/Godot/statics_attribute";
constexpr const char* RpcAttributePath = "/Godot.org/Godot/rpc_attribute";
constexpr const char* ExportFileAttributePath = "/Godot.org/Godot/export_file_attribute";
constexpr const char* ExportDirAttributePath = "/Godot.org/Godot/export_dir";
constexpr const char* ExportMultilineAttributePath = "/Godot.org/Godot/export_multiline";
constexpr const char* ExportFlagsAttributePath = "/Godot.org/Godot/export_flags_attribute";
constexpr const char* ExportNodePathAttributePath = "/Godot.org/Godot/export_node_path_attribute";
constexpr const char* ExportCategoryAttributePath = "/Godot.org/Godot/export_category_attribute";
constexpr const char* ExportGroupAttributePath = "/Godot.org/Godot/export_group_attribute";
constexpr const char* ExportSubgroupAttributePath = "/Godot.org/Godot/export_subgroup_attribute";

/// The string a single-argument metadata attribute was spelled with, or empty when the member
/// does not carry it.
AUTORTFM_DISABLE FUtf8String AttributeText(const uLang::CDataDefinition& Member, const uLang::CClass* AttributeClass, const uLang::CSemanticProgram& Program)
{
    if (!AttributeClass)
    {
        return FUtf8String();
    }
    const uLang::TOptional<uLang::CUTF8String> Text = Member.GetAttributes().GetAttributeTextValue(AttributeClass, Program);
    return Text.IsSet() ? FULangConversionUtils::ULangStrToFUtf8String(*Text) : FUtf8String();
}

/// The recording, and then the role change that lets the next build read the mirror from its
/// digest.
///
/// Retiring is a build's last word on the source project rather than the injection's, because a
/// role is read at the *next* FillInVst and a package flipped mid-build would have the build that
/// is still running disagree with itself about what it is compiling. The recording has to come
/// first all the same: IncrementalizeProjectSource is what actually marks the package External,
/// and CompileProject asks MirrorDefinitionsRecorded() before deciding whether to force it back.
AUTORTFM_DISABLE void RecordAndRetireMirror()
{
    if (!GIde.IsValid())
    {
        return;
    }
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager();
    if (!BuildManager.IsValid())
    {
        return;
    }
    RecordMirrorDefinitions(*BuildManager->GetProgramContext()._Program);
    if (!MirrorDefinitionsRecorded())
    {
        return;
    }

    for (const uLang::CSourceProject::SPackage& Retired : BuildManager->GetSourceProject()->_Packages)
    {
        const uLang::CUTF8String& VersePathOf = Retired._Package->GetSettings()._VersePath;
        const bool bIsAttributePackage =
            FUtf8StringView(Retired._Package->GetName().AsCString()).Equals(FUtf8StringView(AttributePackageName));
        if (!bIsAttributePackage && FUtf8StringView(VersePathOf.AsCString()).Equals(FUtf8StringView(GodotVersePath)))
        {
            Retired._Package->SetRole(uLang::EPackageRole::External);
        }
    }
}

/// One method's declared parameter and result types, which is what a call needs and what
/// FMethodDesc -- which carries only what crosses the ABI -- does not have.
struct FMethodSignatureTypes
{
    TArray<FMemberType> Params;
    FMemberType Result;
};

/// How a `signal(t)`'s payload maps onto Godot's argument list.
///
/// Three shapes, because Verse has three answers to "what is one value carrying several things":
/// a tuple, which cannot name its elements; a struct, which can; and everything else, which is one
/// thing. The *emission* has to take the value apart the same way the descriptor put it together,
/// so one shape serves both rather than each deciding for itself.
enum class EPayloadShape : uint8
{
    /// One argument, the payload itself. `signal(int)`, `signal(node2d)`.
    Bare,
    /// One argument per element, positionally named. `tuple()` is this with no arguments.
    Tuple,
    /// One argument per top-level field, named by the field.
    Struct,
};

/// One Godot argument a payload decomposes into.
struct FPayloadArg
{
    /// What Godot is told the argument is called, and so what the connect dialog shows and what
    /// _make_function writes: `Arg0` for a tuple element, the field's own name for a struct.
    FUtf8String Name;
    /// The decorated key this field is stored under, for LoadField at emission. Empty unless the
    /// payload is a struct -- a tuple is an array and is read by index.
    FUtf8String FieldKey;
    FMemberType Type;
};

/// A payload's whole story: what it decomposes into, and -- when it decomposes into nothing usable
/// -- which argument spoiled it and why (vh_signal_reject).
struct FPayloadShape
{
    EPayloadShape Kind = EPayloadShape::Bare;
    TArray<FPayloadArg> Args;
    int32 Reject = VH_SIGNAL_OK;
    FUtf8String RejectDetail;
    /// The struct the payload decomposes, for Kind == Struct and null otherwise. Kept because the
    /// *inbound* direction has to build one back, and the semantic class is what names it.
    const uLang::CClass* StructClass = nullptr;
    /// The payload as one type, rather than as the arguments it decomposes into. What `Await`
    /// needs: an emission arrives as N Godot arguments and the event it feeds takes one `t`, so
    /// the inbound direction has to put back together exactly what DescribePayload took apart.
    FMemberType Whole;
};

} // namespace

/// The three tables one class's analysis recorded. Defined here rather than in the header because
/// every value in them is a uLang-shaped description this file owns; the sidecar moves them through
/// the two converters below and never reaches inside.
struct GodotVerse::FDeclaredTypes
{
    /// Member name -> declared type, derived class first and up the script chain.
    TMap<FUtf8String, FMemberType> Members;
    /// Decorated method name -> what it takes and answers.
    TMap<FUtf8String, FMethodSignatureTypes> Methods;
    /// Signal member name -> what its payload decomposes into.
    TMap<FUtf8String, FPayloadShape> Signals;
};

/// The same thing for the *mirror's* signals, which belong to no script class: `timer.Timeout` ->
/// what a `Timer.Timeout().Await()` has to rebuild.
struct GodotVerse::FEngineSignalTypes
{
    TMap<FUtf8String, FPayloadShape> Shapes;
};

namespace {

/// The class's recorded tables, or null. What every runtime lookup falls back to, and the whole of
/// what a host with no semantic program has.
AUTORTFM_DISABLE const GodotVerse::FDeclaredTypes* RecordedTypes(FUtf8StringView ClassName);

AUTORTFM_DISABLE FMemberType DescribeMemberType(FUtf8StringView ClassName, FUtf8StringView FieldName)
{
    FMemberType Result;
    if (!GIde.IsValid())
    {
        // A runtime host's whole answer. The table was recorded by the cook's own analysis and read
        // back out of the sidecar; without it every field read and write in an exported game is a
        // description of nothing, which reads as "wrong type" rather than as "no compiler".
        if (const GodotVerse::FDeclaredTypes* const Recorded = RecordedTypes(ClassName))
        {
            if (const FMemberType* const Found = Recorded->Members.Find(FUtf8String(FieldName)))
            {
                return *Found;
            }
        }
        return Result;
    }
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager();
    if (!BuildManager.IsValid())
    {
        return Result;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;

    const FUtf8String ClassPath = FUtf8String(ScriptVersePath) + UTF8TEXT("/") + FUtf8String(ClassName);
    const uLang::CClass* Class = Program->FindDefinitionByVersePath<uLang::CClass>(
        FULangConversionUtils::FUtf8StringViewToULangStringView(ClassPath));
    if (!Class)
    {
        return Result;
    }

    // Up the chain, because one script class may derive from another and the member may be the
    // base's. The walk stops at the first class outside the script package: above that is generated
    // API, whose members are Godot's own properties rather than script state.
    const TArray<const uLang::CClass*> Chain = ClassChainOfOrigin(*Class, *Program, EClassOrigin::Script);
    for (int32 Link = Chain.Num() - 1; Link >= 0; --Link)
    {
        for (const uLang::TSRef<uLang::CDataDefinition>& Member : Chain[Link]->GetDefinitionsOfKind<uLang::CDataDefinition>())
        {
            if (!FUtf8StringView(Member->AsNameCString()).Equals(FieldName))
            {
                continue;
            }
            Result = DescribeType(Member->GetType(), *Program);
            Result.Member = &*Member;
            Result.bIsVar = Member->IsVar();
            return Result;
        }
    }
    return Result;
}

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

/// Reads FieldName off Object. Returns false for a field the shape does not carry, and for any
/// Verse type with no vh_value counterpart.
AUTORTFM_DISABLE bool ReadFieldOf(UObject* Object, FUtf8StringView FieldName, vh_value& OutValue, GodotVerse::FFieldStorage& OutStorage)
{
    if (!Object)
    {
        return false;
    }

    OutValue = vh_value{};
    OutValue.VariantTag = VH_VARIANT_NIL;
    OutStorage.Text.Reset();
    OutStorage.Blocks.Reset();
    OutStorage.Strings.Reset();

    bool bRead = false;
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
            return;
        }

        bRead = ValueToWire(Context, Value, DescribeMemberType(DeclaringClass, FieldName),
                            OutStorage, OutValue);
    });
    return bRead;
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

AUTORTFM_DISABLE UClass* GodotVerse::FindMirroredClass(FUtf8StringView ClassName)
{
    UClass* Found = nullptr;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    EnterVerse(Context, [&] {
        if (Verse::VClass* Class = FindMirroredVClass(Context, ClassName))
        {
            Found = Cast<UClass>(Class->GetOrCreateNativeType(Context));
        }
    });
    return Found;
}

namespace {

/// The Godot object a vh_object under construction should adopt instead of minting one.
///
/// Since R-NODE-3 every vh_object runs a block clause that asks the host for a peer, and the host
/// itself is much the commoner constructor: a script instance for a node Godot already made, a
/// mirror wrapper for a handle crossing in, the transient instance the export defaults are read
/// off. Every one of those already has its object -- or deliberately has none -- so an
/// unconditional mint would leak a Godot object per construction while a working scene looked
/// entirely normal (docs/phase-4b-design.md 4.3).
///
/// Every host-side NewObject of a vh_object is wrapped in one of these. The class is carried as
/// well as the handle so that a *member* of the class being built -- `Helper := helper{}` in a
/// script -- still mints its own: the record answers the construction it was opened for and
/// nothing else, whichever of the two the VM runs first.
///
/// thread_local because a class's declared defaults are read off an instance built by whichever
/// thread ran the analysis, which is not the game thread.
struct FAdoptRecord
{
    const UClass* Class = nullptr;
    int64 Handle = 0;
    bool bActive = false;
};
thread_local FAdoptRecord GAdopt;

/// Scoped, and restoring rather than clearing: NewObject of a script class runs member
/// initializers that can construct more objects, and one of those can be another script class.
class FAdoptPeerScope
{
public:
    AUTORTFM_DISABLE FAdoptPeerScope(const UClass* Class, int64 Handle)
        : Saved(GAdopt)
    {
        GAdopt = FAdoptRecord{Class, Handle, true};
    }
    AUTORTFM_DISABLE ~FAdoptPeerScope() { GAdopt = Saved; }

    FAdoptPeerScope(const FAdoptPeerScope&) = delete;
    FAdoptPeerScope& operator=(const FAdoptPeerScope&) = delete;

private:
    FAdoptRecord Saved;
};

/// Nothing constructed while this stands gets a Godot object, however deep.
///
/// One caller, and it is the case FAdoptPeerScope cannot answer: the throwaway instance the export
/// defaults are read off. That instance is a *reading device*, not something an author asked for,
/// and its members' initializers run in full -- so a class whose member is `var Held:node2d =
/// node2d{}` would mint a real node on every analysis, once per exporting class, per keystroke.
/// Nodes are deliberately not freed when Verse drops them (Godot's rule, docs/phase-4b-design.md
/// 4.4), so each of those would be a leak nothing reports.
///
/// The peers read back as handle 0, which is what a declared default meant before any of this and
/// what the export list already refuses for an object-typed member (VH_EXPORT_OBJECT_NOT_OPTIONAL).
thread_local int32 GSuppressMintDepth = 0;

class FSuppressMintScope
{
public:
    AUTORTFM_DISABLE FSuppressMintScope() { ++GSuppressMintDepth; }
    AUTORTFM_DISABLE ~FSuppressMintScope() { --GSuppressMintDepth; }

    FSuppressMintScope(const FSuppressMintScope&) = delete;
    FSuppressMintScope& operator=(const FSuppressMintScope&) = delete;
};

/// A fresh Verse wrapper around a Godot handle, which is what a mirrored-class member holds.
///
/// Built the way Instantiate builds a script's own object, and buildable that way for the same
/// reason: a mirrored class is ordinary Verse over the one native `object`, so its instance *is* a
/// UObject and none of the VM's own object allocation comes into it.
/// UVerseClass::PostInitInstance has run the class's Verse constructor by the time NewObject
/// returns, which leaves only the field C++ owns to fill in.
AUTORTFM_DISABLE UObject* NewMirroredWrapper(UClass* NativeClass, int64 Handle)
{
    UObject* Wrapper = nullptr;
    {
        // The handle this wrapper is *for*: the block clause runs inside NewObject and writes it,
        // and the assignment below then writes the same value a second time.
        FAdoptPeerScope Adopting(NativeClass, Handle);
        Wrapper = NativeClass ? NewObject<UObject>(GetTransientPackage(), NativeClass) : nullptr;
    }
    verse::vh_object* Shadow = Cast<verse::vh_object>(Wrapper);
    if (!Shadow)
    {
        return nullptr;
    }
    Shadow->Handle.Set(Handle, Shadow);
    return Wrapper;
}

/// Handle -> the script instance bound to it. R-SCN-6's identity half: a node carrying a Verse
/// script has to cross into Verse as *that script's own object*, or `player[GetNode("Player")]`
/// fails on exactly the case the cast exists for -- a fresh mirror wrapper's class is `node`, and
/// no downcast to a script class can succeed against one.
TMap<int64, GodotVerse::FInstance*> GInstancesByHandle;

/// Handle -> the Verse object that minted it (R-NODE-3). Two jobs at once: it is what
/// `BeginDestroy` consults to know that this peer is the host's to release -- every object crossing
/// *from* Godot is a vh_object too, and freeing one would take a node the scene owns -- and it is
/// what keeps `H` the same Verse object after a round trip through Godot, the way a scripted node's
/// own instance does. Neither keeps anything alive: the row exists to be asked about.
///
/// Two pointers to one object, answering different questions. The weak one answers "is it still
/// alive", which is what handing the object back to Godot needs. The raw one answers "is this the
/// object that made this row", which the weak one **cannot**: by the time BeginDestroy runs the
/// object is already unreachable and every weak pointer to it reads as null, so a release keyed on
/// the weak pointer matched nothing and released nothing -- a leak that looked exactly like the
/// mechanism not working. It is compared and never dereferenced.
struct FMintedPeer
{
    TWeakObjectPtr<UObject> Object;
    const UObject* Owner = nullptr;
};
TMap<int64, FMintedPeer> GMintedByHandle;

/// Handle -> the mirrored UClass an object of it crosses as, asked of Godot once per Godot object
/// rather than once per crossing.
///
/// Safe to keep for the life of the process because Godot does not reuse an instance id within a
/// run: a freed object leaves a row naming a class nothing will ask about again. A null answer is
/// cached too -- a class the mirror does not carry is not worth asking Godot about twice.
TMap<int64, UClass*> GHandleClassCache;

/// A Verse function Godot holds as a Callable, as the pair that can name it again later.
///
/// Not the function *value*: 4a accepts only a method bound to a script instance (OQ-16 carries
/// the unbound case), and for one of those the owner's handle and the method's decorated name say
/// everything -- which means nothing here has to keep a VM cell alive, and invoking is the same
/// InstanceCall path Godot's own dispatch takes, argument conversion and all.
struct FCallbackTarget
{
    int64 OwnerHandle = 0;
    FUtf8String DecoratedName;

    /// Non-zero for a Callable the host minted to feed a suspended task rather than to call a
    /// script method: the token of the row in GAwaiters below. Nothing a script hands to
    /// `MakeCallable` ever carries one.
    int64 AwaitToken = 0;

    /// Pack the emission's arguments into one Godot Array before dispatching. What a subscriber to
    /// a *foreign* signal receives, because nothing declares that signal's payload and there is no
    /// per-argument shape to convert against.
    bool bArgsAsArray = false;

    /// Non-zero for the permanent connection an `@export_signal` event member holds: the binding
    /// whose `Event` this emission is signalled into. The event-member analogue of AwaitToken, and
    /// exclusive with it -- an await is one wait, this is every emission for the instance's life.
    int64 EventSignalId = 0;
};

TMap<int64, FCallbackTarget> GCallbacks;
int64 GNextCallbackId = 1;

/// The one table in this file that is touched off the game thread, and so the one that needs a lock.
///
/// vh_callback_release is deliberately unguarded (R-ASYNC-8's exception, argued at its definition):
/// a Godot Callable is destroyed on whatever thread dropped its last reference, and refusing that
/// would leak the row instead. Releasing never enters the VM, so allowing it is safe -- but a
/// TMap::Remove racing a Find on the game thread is not, and that is what this closes.
///
/// The id counter needs no lock: only the game thread mints one.
FCriticalSection GCallbacksLock;

/// One `signal` member of one live instance: everything the member's *type* and *name* said,
/// resolved once at construction so neither has to be spelled again.
struct FSignalBinding
{
    int64 OwnerHandle = 0;
    FUtf8String Name;
    /// What the payload decomposes into -- the same shape the signal descriptor reported to Godot,
    /// so the arguments a handler was generated for and the arguments an emission carries cannot
    /// disagree.
    FPayloadShape Payload;
    /// vh_signal_reject, copied off the descriptor. A rejected signal is still bound, so that
    /// emitting it can say the reason the editor said rather than the generic "names nothing" --
    /// which is what a script running outside the editor gets, and it was the whole complaint.
    int32 Reject = VH_SIGNAL_OK;
    FUtf8String RejectDetail;

    /// The `event(t)` an `@export_signal` member holds, and the connection that feeds it.
    ///
    /// Null for a `signal(t)`, whose event is reached through the signal object at each await and
    /// whose connection lives exactly as long as that wait. An `event(t)` has no such hook -- its
    /// `Await` is Verse's own native -- so the connection is made once here and held for the
    /// instance's life, which is the one place R-SIG-5's connect-while-awaiting is traded away.
    ///
    /// Held strongly because it is what an emission is delivered into, and dropped at
    /// ReleaseInstance: a strong pointer kept past the node would be a GC root per scripted node,
    /// which is the shape of leak that looks entirely normal in a working scene.
    TStrongObjectPtr<UObject> Event;
    int64 CallableRef = 0;
    int64 CallbackId = 0;
};

TMap<int64, FSignalBinding> GSignalBindings;
int64 GNextSignalId = 1;

/// The `event(t)` an `@export_signal` member holds -> its binding id.
///
/// A `signal(t)` needs no such table: the row's id is written into the object's own `Id` field at
/// bind time, which is what the native class exists for. `event(t)` is Verse's own and cannot be
/// reopened to carry one, so the object *is* the key -- which is also why the binding holds it
/// strongly. The raw pointer is safe for exactly as long as that strong pointer is, and both are
/// dropped together at ReleaseInstance.
TMap<const UObject*, int64> GEventBindingIds;

/// One live connection, which is what a `connection` names.
struct FSubscription
{
    int64 OwnerHandle = 0;
    FUtf8String Name;
    /// The reference id of the Callable Godot holds. Released when the subscription is cancelled,
    /// which is also what drops the host's claim on the callback behind it.
    int64 CallableRef = 0;
};

TMap<int64, FSubscription> GSubscriptions;
int64 GNextSubscriptionId = 1;

/// One task waiting on one Godot signal (R-SIG-5).
///
/// A wait is a connection the host owns for exactly as long as the wait lasts. What it feeds is a
/// `/Verse.org/Verse` `event(t)` living on the `signal` object the script awaited -- which is
/// why the object is held here rather than only its binding id: an engine-signal accessor mints a
/// *fresh* `signal` on every call, several of them share one binding, and only the object
/// says which event a given wait is suspended on.
struct FAwaiter
{
    /// Held strongly: `Timer.Timeout().Await()` awaits a temporary, and nothing else on the Verse
    /// side has to outlive the statement that made it.
    TStrongObjectPtr<UObject> Waiter;
    /// The binding whose payload shape says how to put the emission's arguments back together.
    /// Zero for a foreign signal, whose arguments become one Godot Array instead.
    int64 SignalId = 0;
    /// What to disconnect from, and what to disconnect with.
    int64 OwnerHandle = 0;
    FUtf8String Name;
    int64 CallableRef = 0;
    /// The callback id the Callable carries, so ending the wait drops its row too.
    int64 CallbackId = 0;

    /// The scope the wait was started in, and the cleanup it registered there.
    ///
    /// This is what ends a wait that never resumes *and* never runs its `defer`: terminating a
    /// task group does not unwind the tasks in it, so a node freed while awaiting would otherwise
    /// leave a live Godot connection, a held object and a callback row behind. Epic's own rule, and
    /// their own hook -- `event::SubscribeInternal` drops a subscription the same way
    /// (`VerseEvent.cpp:139-178`). `phase-4-gaps.md` G9 is this.
    TWeakPtr<verse::FContentScope> Scope;
    FDelegateHandle Cleanup;
};

TMap<int64, FAwaiter> GAwaiters;
int64 GNextAwaitToken = 1;

/// "<handle>:<signal>" -> the binding id, so an accessor called twice on one object answers the
/// same row. A row is never dropped: the ids are small, and nothing tells the host that Godot has
/// freed an object.
TMap<FUtf8String, int64> GEngineSignalIds;

/// "<class>.<accessor>" -> a binding holding only the payload shape, which is the expensive half:
/// every Timer's `timeout` carries the same nothing, and the lookup walks the semantic program.
TMap<FUtf8String, FSignalBinding> GEngineSignalShapes;

/// The same table, recorded by the cook and read back out of the sidecar. What a runtime host has
/// instead of a semantic program to walk.
TSharedPtr<GodotVerse::FEngineSignalTypes> GRecordedEngineSignals;

/// The Godot class a mirrored Verse class stands for, out of the generated table's other half.
///
/// The emitted classes only, so this is an inverse rather than a second lookup: the table above
/// folds every Godot class onto its nearest *emitted* ancestor, and several rows of it can share a
/// Verse name. Binary search, because that half is sorted by Verse name.
const char* GodotNameForMirroredClass(FUtf8StringView VerseName)
{
    int32 Low = 0;
    int32 High = UE_ARRAY_COUNT(verse_classes::mirrored_classes) - 1;
    while (Low <= High)
    {
        const int32 Mid = Low + ((High - Low) / 2);
        const FUtf8StringView Candidate(
            reinterpret_cast<const UTF8CHAR*>(verse_classes::mirrored_classes[Mid].verse_name));
        const int32 Order = Candidate.Compare(VerseName);
        if (Order == 0)
        {
            return verse_classes::mirrored_classes[Mid].godot_name;
        }
        if (Order < 0)
        {
            Low = Mid + 1;
        }
        else
        {
            High = Mid - 1;
        }
    }
    return nullptr;
}

/// The Godot class to mint a peer of for an object of this Verse class: the nearest ancestor that
/// is one of the mirror's own, which for a script's `class(ref_counted)` is `RefCounted`.
///
/// The walk tests the *package* rather than the name, because the mirror's names are ordinary
/// identifiers a project could in principle reuse, and a script class that happened to be called
/// `node2d` would otherwise be minted as a Node2D. A VClass knows which package declared it; a
/// UClass's name does not.
///
/// Cached per UClass, so the walk happens once per script class rather than once per `helper{}`.
/// Safe to keep: a UClass is never reused for another Verse class, and a hot-reload generation
/// publishes new ones.
TMap<const UClass*, const char*> GPeerClassCache;

AUTORTFM_DISABLE void ForgetCachedClasses()
{
    GHandleClassCache.Empty();
    GPeerClassCache.Empty();
}

AUTORTFM_DISABLE const char* GodotPeerClassFor(const UClass* Class)
{
    if (const char** Cached = GPeerClassCache.Find(Class))
    {
        return *Cached;
    }

    const char* Found = nullptr;
    // The bindings package comes first, and it has to: a binding's whole point is that Godot has a
    // class the mirror does not, so the mirrored ancestor this walk would otherwise reach is the
    // *wrong* class to mint -- a RigidBody2D where a RapierBody2D was meant, silently, while a
    // working scene looks entirely normal (docs/generated-bindings.md 6).
    for (const UClass* Cursor = Class; Cursor && !Found; Cursor = Cursor->GetSuperClass())
    {
        const UVerseClass* const VerseClass = Cast<UVerseClass>(Cursor);
        const Verse::VClass* const VClass = VerseClass ? VerseClass->Class.Get() : nullptr;
        if (!VClass
            || !VClass->GetPackage().GetRootPath().AsStringView().Equals(FUtf8StringView(BindingsVersePath)))
        {
            continue;
        }
        if (const FUtf8String* const MintName = GMintNameByBinding.Find(FUtf8String(VClass->GetBaseName().AsStringView())))
        {
            // Held by the map for as long as the roster does, and the map outlives the cache: both
            // are emptied together by SetBindings.
            Found = reinterpret_cast<const char*>(**MintName);
        }
    }

    for (const UClass* Cursor = Class; Cursor && !Found; Cursor = Cursor->GetSuperClass())
    {
        const UVerseClass* const VerseClass = Cast<UVerseClass>(Cursor);
        const Verse::VClass* const VClass = VerseClass ? VerseClass->Class.Get() : nullptr;
        if (!VClass
            || !VClass->GetPackage().GetRootPath().AsStringView().Equals(FUtf8StringView(GodotVersePath)))
        {
            continue;
        }
        Found = GodotNameForMirroredClass(VClass->GetBaseName().AsStringView());
    }

    GPeerClassCache.Add(Class, Found);
    return Found;
}

/// The mirrored Verse class name for a Godot class name, out of the generated table.
///
/// Every Godot class is in that table, not only the emitted ones: with --classes-file a subset is
/// generated, and each row then names the nearest ancestor that was. Binary search, because the
/// table is sorted by Godot name and this is asked once per Godot object.
const char* MirroredNameForGodotClass(FUtf8StringView GodotName)
{
    int32 Low = 0;
    int32 High = UE_ARRAY_COUNT(verse_classes::class_names) - 1;
    while (Low <= High)
    {
        const int32 Mid = Low + ((High - Low) / 2);
        const FUtf8StringView Candidate(reinterpret_cast<const UTF8CHAR*>(verse_classes::class_names[Mid].godot_name));
        const int32 Order = Candidate.Compare(GodotName);
        if (Order == 0)
        {
            return verse_classes::class_names[Mid].verse_name;
        }
        if (Order < 0)
        {
            Low = Mid + 1;
        }
        else
        {
            High = Mid - 1;
        }
    }
    return nullptr;
}

/// The class a live handle should cross as, cached per handle.
///
/// Four questions, in this order, and the order is the point: a binding is *more derived* than the
/// mirrored class the handle would otherwise become, and a script binding is more derived still.
///
///   1. Does a script on this object name a binding? `get_class()` answers a script's native base,
///      so this one can only be asked of GetScriptClassOf.
///   2. Does this object's ClassDB class name a binding? A third-party GDExtension's class.
///   3. Is it one of the mirror's own?
///   4. Nothing -- the caller's declared type, or a bare vh_object for a cast to decline.
AUTORTFM_DISABLE UClass* MirroredClassForHandle(int64 Handle)
{
    if (UClass** Cached = GHandleClassCache.Find(Handle))
    {
        return *Cached;
    }

    UClass* Found = nullptr;
    GodotVerse::FHostState& Host = GodotVerse::GetHost();

    if (!GBindingByScriptClass.IsEmpty() && Host.Godot.GetScriptClassOf)
    {
        GodotVerse::FCallArena Arena;
        vh_value ScriptName{};
        if (Host.Godot.GetScriptClassOf(Host.Godot.Ctx, Handle, &Arena, &ScriptName) == VH_CALL_OK
            && ScriptName.Type == VH_TYPE_STRING)
        {
            const FUtf8String Named(GodotVerse::MakeView(ScriptName.String.Utf8, ScriptName.String.Len));
            if (const FUtf8String* const VerseName = GBindingByScriptClass.Find(Named))
            {
                Found = FindBindingClass(*VerseName);
            }
        }
    }

    if (!Found && Host.Godot.GetClassOf)
    {
        GodotVerse::FCallArena Arena;
        vh_value ClassName{};
        if (Host.Godot.GetClassOf(Host.Godot.Ctx, Handle, &Arena, &ClassName) == VH_CALL_OK
            && ClassName.Type == VH_TYPE_STRING)
        {
            const FUtf8StringView GodotName = GodotVerse::MakeView(ClassName.String.Utf8, ClassName.String.Len);
            if (const FUtf8String* const VerseName = GBindingByGodotClass.Find(FUtf8String(GodotName)))
            {
                Found = FindBindingClass(*VerseName);
            }
            if (!Found)
            {
                if (const char* VerseName = MirroredNameForGodotClass(GodotName))
                {
                    Found = FindMirroredClass(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(VerseName)));
                }
            }
        }
    }

    GHandleClassCache.Add(Handle, Found);
    return Found;
}

/// The Godot container a wrapper class's own `Ref` default mints -- an Array for `godot_array` and
/// every `typed_array(t)`, a Dictionary for `dictionary` and every `typed_dictionary(k, v)` -- or 0
/// for a wrapper whose default names nothing, which is `callable` and `signal_ref`.
///
/// Keyed on the package and the base name, the test GodotPeerClassFor makes, because a parametric
/// instantiation is a class of its own with nothing else in common with its siblings.
AUTORTFM_DISABLE int32 MintedContainerTag(const UClass* Class)
{
    for (const UClass* Cursor = Class; Cursor; Cursor = Cursor->GetSuperClass())
    {
        const UVerseClass* const VerseClass = Cast<UVerseClass>(Cursor);
        const Verse::VClass* const VClass = VerseClass ? VerseClass->Class.Get() : nullptr;
        if (!VClass
            || !VClass->GetPackage().GetRootPath().AsStringView().Equals(FUtf8StringView(GodotVersePath)))
        {
            continue;
        }
        const FUtf8StringView Name = VClass->GetBaseName().AsStringView();
        if (Name.Equals(UTF8TEXT("godot_array")) || Name.StartsWith(UTF8TEXT("typed_array")))
        {
            return VH_VARIANT_ARRAY;
        }
        if (Name.Equals(UTF8TEXT("dictionary")) || Name.StartsWith(UTF8TEXT("typed_dictionary")))
        {
            return VH_VARIANT_DICTIONARY;
        }
    }
    return 0;
}

/// Gives each container an object's member defaults hold a reference id of its own (B42).
///
/// NewObject does not evaluate a class's member defaults. It copies them off the class default
/// object, and a wrapper that default expression built is instanced into the new object as a
/// subobject of it: a fresh wrapper holding the *same* id. Worse, that id is not even the class
/// default's own -- a wrapper built while a class default is being constructed takes `Ref` off its
/// own class's default object rather than running `VhRefNewDefault` -- so every scripted node shared
/// one Godot Array with every other, and in a cooked game, whose defaults the cooker read with no
/// Godot to mint against, held the dead reference 0.
///
/// Decided per wrapper by what its default was:
///
///  - still its own class's default id: the member's default was a fresh container (`godot_array{}`,
///    `MakeNodeArray()`), so this object gets a fresh one, as evaluating it would have given it.
///  - any other id: the default named a container that already existed, which is shared by
///    design, so the copy keeps it and takes a claim of its own -- without which its collection
///    released a claim it never held.
///  - under FSuppressMintScope, 0, which is what `VhRefNewDefault` answers there, and which leaves
///    the reading device nothing to release. That instance is built off the game thread, where the
///    reference table cannot be asked for anything.
///
/// The minted or retained id is the wrapper's to release, by the godot_ref::BeginDestroy rule.
/// Nothing compensates on abort, and nothing needs to: the write is open, so the wrapper keeps the
/// id whatever the construction's transaction does and releases it when it is collected.
///
/// Must run before any block clause reads a member, which is why AdoptOrMintPeer's adopt branch
/// calls it: vh_object's own block is the first to run. A member *object's* block runs earlier
/// still, while the instance is being built, and sees the copied id for the length of that block.
AUTORTFM_DISABLE void ClaimDefaultContainers(UObject* Object, bool bSuppressed)
{
    GodotVerse::FHostState& Host = GodotVerse::GetHost();
    ForEachObjectWithOuter(
        Object,
        [&](UObject* Subobject) {
            verse::godot_ref* const Wrapper = Cast<verse::godot_ref>(Subobject);
            if (!Wrapper)
            {
                return;
            }
            if (bSuppressed)
            {
                Wrapper->Ref.Init(0, Wrapper);
                return;
            }

            const int64 Held = Wrapper->Ref.Get();
            const verse::godot_ref* const ClassDefault =
                Cast<verse::godot_ref>(Wrapper->GetClass()->GetDefaultObject(/*bCreateIfNeeded*/ false));
            const bool bIsClassDefault = !ClassDefault || Held == ClassDefault->Ref.Get();
            const int32 Tag = MintedContainerTag(Wrapper->GetClass());
            if (Tag != 0 && bIsClassDefault)
            {
                Wrapper->Ref.Init(Host.Godot.NewRef ? Host.Godot.NewRef(Host.Godot.Ctx, Tag) : 0, Wrapper);
            }
            else if (Held != 0)
            {
                Wrapper->Ref.Init(Host.Godot.RetainRef ? Host.Godot.RetainRef(Host.Godot.Ctx, Held) : 0, Wrapper);
            }
        },
        EGetObjectsFlags::IncludeNestedObjects);
}

} // namespace

AUTORTFM_DISABLE UClass* GodotVerse::MirroredClassFor(FUtf8StringView GodotClassName)
{
    const char* const VerseName = MirroredNameForGodotClass(GodotClassName);
    return VerseName
        ? FindMirroredClass(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(VerseName)))
        : nullptr;
}

/// The Verse object a Godot handle crosses as: the script's own instance where the node carries
/// one, and a fresh mirror wrapper of the handle's actual Godot class otherwise (R-SCN-6).
///
/// Never null. Fallback is what to build when Godot will not say what the handle is -- an object
/// it has already freed, or a Godot class outside this mirror. A caller with a declared type to
/// fall back on passes it; the generated cast path passes null and gets a bare `vh_object`, which
/// every downcast then declines. That is the shape the cast asked for: a failure is an answer,
/// where a raise from here would be a different question's error.
///
/// Mirror wrappers are deliberately *not* cached, so two crossings of one un-scripted handle are
/// two Verse objects. Equality is by handle, and a wrapper cache would need a rule for what
/// happens to one across a hot-reload generation, which nothing yet needs.
AUTORTFM_DISABLE UObject* GodotVerse::ObjectForHandle(int64 Handle, UClass* Fallback)
{
    if (FInstance** Bound = GInstancesByHandle.Find(Handle))
    {
        if (*Bound && (*Bound)->Object.IsValid())
        {
            return (*Bound)->Object.Get();
        }
    }

    // An object Verse minted crosses back as the very object that minted it, which is what makes
    // `H` the same value after a round trip through a Godot Array. The same rule the scripted-node
    // row above states, for the other half of R-SCN-6's identity question.
    if (FMintedPeer* Minted = GMintedByHandle.Find(Handle))
    {
        if (UObject* Live = Minted->Object.Get())
        {
            return Live;
        }
    }

    UClass* Resolved = MirroredClassForHandle(Handle);
    if (!Resolved)
    {
        Resolved = Fallback;
    }
    if (UObject* Wrapper = NewMirroredWrapper(Resolved, Handle))
    {
        return Wrapper;
    }

    // A bare vh_object, which every cast then declines. It has no peer and must not mint one:
    // this is the answer to "Godot would not say what that handle is", not a request for an object.
    FAdoptPeerScope Adopting(verse::vh_object::StaticClass(), 0);
    return NewObject<verse::vh_object>(GetTransientPackage());
}

AUTORTFM_DISABLE int64 GodotVerse::AdoptOrMintPeer(verse::vh_object* Self, const char*& OutRefusedClass)
{
    OutRefusedClass = nullptr;
    if (!Self)
    {
        return 0;
    }

    // A class default object is not a live object and has nothing to be the peer of. It is guarded
    // rather than assumed: UVerseClass::NeedsInit runs the init functions for one, and this bridge
    // creates a CDO for every mirrored class it ever names.
    if (Self->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
    {
        return 0;
    }

    // The host is the side doing the constructing, and the peer -- or the deliberate absence of one
    // -- is already decided. Consumed, so that a member of the class being built still mints its
    // own; the class test is what makes that true whichever order the VM runs the two in.
    if (GAdopt.bActive && GAdopt.Class == Self->GetClass())
    {
        GAdopt.bActive = false;
        ClaimDefaultContainers(Self, GSuppressMintDepth > 0);
        return GAdopt.Handle;
    }

    if (GSuppressMintDepth > 0)
    {
        ClaimDefaultContainers(Self, /*bSuppressed*/ true);
        return 0;
    }

    const char* const GodotClass = GodotPeerClassFor(Self->GetClass());
    if (!GodotClass)
    {
        // `vh_object` itself, which is the one class below the mirror and which nothing a script
        // writes should name. Not an error: ObjectForHandle builds one deliberately when Godot will
        // not say what a handle is, and that is a value for a cast to decline rather than a request
        // for an object.
        return 0;
    }

    FHostState& Host = GetHost();
    const FUtf8StringView ClassView(reinterpret_cast<const UTF8CHAR*>(GodotClass));
    const int64 Handle = Host.Godot.InstantiateClass
        ? Host.Godot.InstantiateClass(Host.Godot.Ctx, GodotClass, ClassView.Len())
        : 0;
    if (Handle == 0)
    {
        OutRefusedClass = GodotClass;
        return 0;
    }

    GMintedByHandle.Add(Handle, FMintedPeer{TWeakObjectPtr<UObject>(Self), Self});

    // Compensated rather than deferred, exactly as SubscribeSignal is and for the same reason: this
    // mutates Godot and answers a value. `SameAsClosed` is the load-bearing half -- the mint above
    // ran inside an AutoRTFM::Open, and a plain OnAbort from open code is ignored.
    //
    // bDiscard, because an aborted transaction's object is one nothing outside it can ever have
    // seen: even the Object-derived peer this bridge otherwise leaks on purpose (GDScript's own
    // rule) is freed here, where there is nobody left to free it by hand.
    AutoRTFM::OnAbort<AutoRTFM::EOpenBehavior::SameAsClosed>(
        [Handle] { GodotVerse::ReleaseMintedPeer(nullptr, Handle, /*bDiscard*/ true); });
    return Handle;
}

AUTORTFM_DISABLE FUtf8String GodotVerse::ClassBaseType(FUtf8StringView ClassName)
{
    // Resolving the class is a VM entry, and a running analysis has the VM blocked. The consumer
    // only asks where the source text is gone, which is a runtime host, which never analyses --
    // but the rule is the file's and not the caller's, so it is checked here.
    if (IsBackgroundCheckRunning())
    {
        return FUtf8String();
    }
    const char* const GodotClass = GodotPeerClassFor(FindGodotClass(ClassName));
    return GodotClass ? FUtf8String(GodotClass) : FUtf8String();
}

AUTORTFM_DISABLE bool GodotVerse::IsMintSuppressed()
{
    return GSuppressMintDepth > 0;
}

AUTORTFM_DISABLE void GodotVerse::ReleaseMintedPeer(const UObject* Owner, int64 Handle, bool bDiscard)
{
    if (Handle == 0)
    {
        return;
    }
    const FMintedPeer* const Minted = GMintedByHandle.Find(Handle);
    if (!Minted)
    {
        // Not ours. Every object crossing *from* Godot is a vh_object too, and this is the path its
        // collection takes -- releasing here would free a node the scene owns.
        return;
    }
    // Owner is null from the abort compensation, which has no object left to name: the transaction
    // that built it is being undone. From BeginDestroy it is the object being collected, and the
    // row has to be the one it made.
    if (Owner && Minted->Owner != Owner)
    {
        return;
    }
    GMintedByHandle.Remove(Handle);

    FHostState& Host = GetHost();
    if (Host.Godot.ReleaseObject)
    {
        Host.Godot.ReleaseObject(Host.Godot.Ctx, Handle, bDiscard ? 1 : 0);
    }
}

namespace {

using FFieldValueBuilder = TFunctionRef<Verse::VValue(Verse::FRunningContext Context, Verse::VValue Current)>;

AUTORTFM_DISABLE bool WriteFieldWith(UObject* Object, FUtf8StringView FieldName, EFieldWrite Mode, FFieldValueBuilder MakeValue)
{
    if (!Object)
    {
        return false;
    }

    if (Mode == EFieldWrite::Assign && !IsVarMember(QualifiedClassName(Object->GetClass()), FieldName))
    {
        return false;
    }

    bool bWrote = false;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    EnterVerse(Context, [&] {
        FUtf8String DeclaringClass;
        const Verse::VShape::VEntry* Field = FindShapeField(Context, Object, FieldName, DeclaringClass);

        // A Constant entry lives in the shape itself rather than in the object, so it is shared by
        // every instance and cannot be assigned to.
        if (Field == nullptr || !Field->IsProperty())
        {
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

        const Verse::VValue NewValue = MakeValue(Context, Current);
        if (NewValue.IsUninitialized())
        {
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
            bWrote = true;
        }
        else
        {
            // Both FProperty and FPropertyVar are native storage behind an FProperty, and
            // VNativeRef::Set is the write for either -- for a var it is the assignment, and for a
            // non-var it is what the interpreter itself uses to initialize one.
            const Verse::FOpResult Result = Verse::VNativeRef::New(Context, Object, Field->UProperty).Set(Context, NewValue);
            bWrote = Result.IsReturn();
        }
    });
    return bWrote;
}

/// Writes the object an optional reference member should hold, or nothing for null.
///
/// The caller is the one that has checked Referenced against the member's declared class. Neither
/// the shape nor the slot will: a slot told it holds a `?sprite2d` takes whatever object is put in
/// it, and the mistake surfaces the first time compiled code calls a method that is not there.
AUTORTFM_DISABLE bool WriteReferenceField(UObject* Object, FUtf8StringView FieldName, EFieldWrite Mode, UObject* Referenced)
{
    return WriteFieldWith(Object, FieldName, Mode, [Referenced](Verse::FRunningContext Context, Verse::VValue) {
        return ReferenceOption(Context, Referenced);
    });
}

AUTORTFM_DISABLE bool WriteFieldOf(UObject* Object, FUtf8StringView FieldName, const vh_value& Value, EFieldWrite Mode)
{
    if (!Object)
    {
        return false;
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
            return false;
        }
        // Built before the VM scope is entered, because constructing it runs the class's Verse
        // constructor through UVerseClass::PostInitInstance, which takes a context of its own.
        const int64 Handle = Value.Type == VH_TYPE_INT ? Value.Int : 0;
        UClass* const DeclaredClass = FindMirroredClass(FUtf8StringView(Declared.ReferenceName));
        UObject* Referenced = Handle != 0 ? GodotVerse::ObjectForHandle(Handle, DeclaredClass) : nullptr;
        if (Handle != 0 && (!Referenced || !Referenced->IsA(DeclaredClass)))
        {
            return false;
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
            return false;
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
            return false;
        }

        if (Value.Type == VH_TYPE_TUPLE)
        {
            const FStructLayout* const Layout = Declared.Struct;
            if (!Layout)
            {
                return false;
            }
            return WriteFieldWith(Object, FieldName, Mode, [&Value, Layout](Verse::FRunningContext Context, Verse::VValue Current) {
                // The class is taken from the struct already in the slot, which is the discipline
                // every write here follows: the new value cannot be of a class the compiled code was
                // not already expecting.
                Verse::VRef* const Box = Current.DynamicCast<Verse::VRef>();
                const Verse::VValue Inner = Box ? Box->Get(Context) : Current;
                Verse::VValueObject* const Struct = Inner.DynamicCast<Verse::VValueObject>();
                return Struct ? NewStructValue(Context, Struct->GetClass(), *Layout, Value.Seq.Items, Value.Seq.Count)
                              : Verse::VValue();
            });
        }

        const int32 Tag = Declared.Described.VariantTag;
        const int32 ElementTag = Declared.Described.ElementVariantTag;
        return WriteFieldWith(Object, FieldName, Mode, [&Value, Tag, ElementTag](Verse::FRunningContext Context, Verse::VValue Current) {
            Verse::VRef* const Box = Current.DynamicCast<Verse::VRef>();
            const Verse::VValue Inner = Box ? Box->Get(Context) : Current;
            return NewArrayValue(Context, Inner.IsCellOfType<Verse::VMutableArray>(), Tag, ElementTag,
                                 Value.Seq.Items, Value.Seq.Count);
        });
    }

    return WriteFieldWith(Object, FieldName, Mode, [&Value](Verse::FRunningContext Context, Verse::VValue Current) {
        switch (Value.Type)
        {
        case VH_TYPE_LOGIC:
            return Verse::VValue::FromBool(Value.Logic != 0);
        case VH_TYPE_INT:
        {
            // An enum member holds an enumerator rather than a number, and the enumeration it belongs
            // to is reachable only from the enumerator already in the slot -- the usual rule here,
            // that the new value's kind comes from the one it replaces. WriteFieldOf has already
            // bounded the ordinal against the enum the author declared.
            Verse::VRef* const Box = Current.DynamicCast<Verse::VRef>();
            const Verse::VValue Inner = Box ? Box->Get(Context) : Current;
            if (Verse::VEnumerator* Enumerator = Inner.DynamicCast<Verse::VEnumerator>())
            {
                Verse::VEnumeration* const Enumeration = Enumerator->GetEnumeration();
                if (!Enumeration || Value.Int < 0 || Value.Int >= Enumeration->NumEnumerators)
                {
                    return Verse::VValue();
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
            // around it: `var Label:string` holds a VMutableArray where a plain one holds a VArray.
            const FUtf8StringView Utf8(reinterpret_cast<const UTF8CHAR*>(Value.String.Utf8), Value.String.Len);
            Verse::VRef* const Box = Current.DynamicCast<Verse::VRef>();
            const Verse::VValue Inner = Box ? Box->Get(Context) : Current;
            return Inner.IsCellOfType<Verse::VMutableArray>()
                ? Verse::VValue(Verse::VMutableArray::New(Context, Utf8))
                : Verse::VValue(Verse::VArray::New(Context, Utf8));
        }
        default:
            return Verse::VValue();
        }
    });
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::WriteInstanceField(FInstance* Instance, FUtf8StringView FieldName, const vh_value& Value)
{
    if (!Instance || !Instance->Object.IsValid())
    {
        return false;
    }
    return WriteFieldOf(Instance->Object.Get(), FieldName, Value,
                        Instance->bSealed ? EFieldWrite::Assign : EFieldWrite::Initialize);
}

AUTORTFM_DISABLE bool GodotVerse::WriteInstanceFieldInstance(FInstance* Instance, FUtf8StringView FieldName, const FInstance* Value)
{
    if (!Instance || !Instance->Object.IsValid())
    {
        return false;
    }

    UObject* Referenced = Value && Value->Object.IsValid() ? Value->Object.Get() : nullptr;
    const FMemberType Declared = DescribeMemberType(QualifiedClassName(Instance->Object->GetClass()), FieldName);
    if (Declared.ReferenceName.IsEmpty())
    {
        return false;
    }

    // The class check the slot will not do. A mirrored member accepts an instance too, and should:
    // a `?node2d` assigned a node that carries a script is better off holding that script's own
    // object than a second wrapper around the same handle, which would give one node two identities.
    if (Referenced)
    {
        UClass* MemberClass = Declared.ReferenceOrigin == EClassOrigin::Script
            ? FindGodotClass(FUtf8StringView(Declared.ReferenceQualifiedName))
            : FindMirroredClass(FUtf8StringView(Declared.ReferenceName));
        if (!MemberClass || !Referenced->GetClass()->IsChildOf(MemberClass))
        {
            return false;
        }
    }

    return WriteReferenceField(Instance->Object.Get(), FieldName,
                               Instance->bSealed ? EFieldWrite::Assign : EFieldWrite::Initialize, Referenced);
}

AUTORTFM_DISABLE bool GodotVerse::ReadInstanceField(const FInstance* Instance, FUtf8StringView FieldName, vh_value& OutValue, FFieldStorage& OutStorage)
{
    if (!Instance || !Instance->Object.IsValid())
    {
        return false;
    }
    return ReadFieldOf(Instance->Object.Get(), FieldName, OutValue, OutStorage);
}

namespace {

/// One class's declared defaults, read off a transient instance of the published class.
///
/// The instance is built once per class rather than once per member, which is what makes reading
/// every `@export` into the snapshot cost about what reading one used to: UVerseClass runs the
/// Verse constructor from PostInitInstance, which NewObject drives and class-default-object
/// construction does not, so a CDO's members read back uninitialized and an instance is the only
/// place a declared default actually exists. Handle is left unset: reading a plain data member
/// never consults it.
AUTORTFM_DISABLE UObject* NewDefaultsObject(FUtf8StringView ClassName)
{
    UClass* const NativeClass = FindGodotClass(ClassName);
    // Nothing under here gets a Godot object -- not this instance, and not whatever its member
    // initializers construct, which is the half that matters. See FSuppressMintScope.
    FSuppressMintScope Reading;
    return NativeClass ? NewObject<UObject>(GetTransientPackage(), NativeClass) : nullptr;
}

AUTORTFM_DISABLE TSharedPtr<const GodotVerse::FFieldValue> ReadDefaultFieldOf(UObject* Defaults, FUtf8StringView FieldName)
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

} // namespace

AUTORTFM_DISABLE bool GodotVerse::ReadClassDefaultField(FUtf8StringView ClassName,
                                                        FUtf8StringView FieldName,
                                                        TSharedPtr<const FFieldValue>& OutValue)
{
    OutValue.Reset();

    if (GSnapshot)
    {
        if (const FAnalysisSnapshot::FClass* const Found = GSnapshot->Classes.Find(FUtf8String(ClassName)))
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

namespace {

/// Godot's name for the virtual a Verse method of this name overrides: `Ready` -> `_ready`,
/// `PhysicsProcess` -> `_physics_process`.
///
/// The exact inverse of the rule gen_verse_api.py names a mirrored virtual by, which is what makes
/// this a rule rather than a table: every virtual the generator emits round-trips through it, so a
/// virtual added by a future Godot version needs no change here.
AUTORTFM_DISABLE FUtf8String GodotVirtualNameOf(FUtf8StringView VerseName)
{
    FUtf8String Result;
    for (int32 Index = 0; Index < VerseName.Len(); ++Index)
    {
        const UTF8CHAR Char = VerseName[Index];
        if (Char >= UTF8CHAR('A') && Char <= UTF8CHAR('Z'))
        {
            // One separator, never two: since Phase 4 a virtual is spelled `_Ready`, so the
            // underscore Godot's own name starts with is already in hand and adding another would
            // ask Godot about `__ready`.
            if (Result.IsEmpty() || Result[Result.Len() - 1] != UTF8CHAR('_'))
            {
                Result.AppendChar(UTF8CHAR('_'));
            }
            Result.AppendChar(UTF8CHAR(Char - 'A' + 'a'));
        }
        else
        {
            Result.AppendChar(Char);
        }
    }
    return Result;
}

/// Whether the definition this function ultimately overrides was declared in the generated Godot
/// mirror, which is what makes it one of Godot's virtuals rather than the script's own method.
AUTORTFM_DISABLE bool OverridesMirroredDefinition(const uLang::CFunction& Function)
{
    const uLang::CFunction* Base = Function.GetBaseOverriddenDefinition().GetPrototypeDefinition();
    if (!Base || Base == &Function)
    {
        return false;
    }
    const FUtf8String ScopePath = FULangConversionUtils::ULangStrToFUtf8String(
        Base->_EnclosingScope.GetScopePath('/', uLang::CScope::EPathMode::PrefixSeparator));
    return FUtf8StringView(ScopePath).StartsWith(FUtf8StringView(GodotVersePath));
}

} // namespace

namespace {

AUTORTFM_DISABLE bool GetClassMethodsLive(FUtf8StringView ClassName, TArray<GodotVerse::FMethodDesc>& OutMethods)
{
    using GodotVerse::FMethodDesc;
    using GodotVerse::FParamDesc;

    OutMethods.Reset();

    if (!GIde.IsValid())
    {
        return false;
    }
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager();
    if (!BuildManager.IsValid())
    {
        return false;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;

    const FUtf8String ClassPath = FUtf8String(ScriptVersePath) + UTF8TEXT("/") + FUtf8String(ClassName);
    const uLang::CClass* Class = Program->FindDefinitionByVersePath<uLang::CClass>(
        FULangConversionUtils::FUtf8StringViewToULangStringView(ClassPath));
    if (!Class)
    {
        return false;
    }

    // Only the class's own declarations. What it inherits from the mirrored API is Godot's to
    // dispatch, not the script's -- and an override *is* a declaration, so a script's Ready is
    // here while the empty one it overrides is not.
    for (const uLang::TSRef<uLang::CFunction>& Function : Class->GetDefinitionsOfKind<uLang::CFunction>())
    {
        const uLang::CFunctionType* const Type = Function->_Signature.GetFunctionType();
        if (!Type)
        {
            continue;
        }

        FMethodDesc Desc;
        Desc.Name = FUtf8String(Function->AsNameCString());
        Desc.DecoratedName = FULangConversionUtils::ULangStrToFUtf8String(Function->GetDecoratedName());

        const uLang::SEffectSet Effects = Function->_Signature.GetEffects();
        Desc.bCanFail = Effects[uLang::EEffect::decides];
        Desc.bSuspends = Effects[uLang::EEffect::suspends];

        for (const uLang::CDataDefinition* Param : Function->_Signature.GetParams())
        {
            if (!Param)
            {
                continue;
            }
            const FMemberType ParamType = DescribeType(Param->GetType(), *Program);
            FParamDesc& Out = Desc.Params.AddDefaulted_GetRef();
            Out.Name = FUtf8String(Param->AsNameCString());
            Out.Type = ParamType.Described.Type;
            // A Verse array names no single Godot type: `[]float` is a PackedFloat32Array, a
            // PackedFloat64Array or a plain Array, and the converters take any of them. Reporting
            // one would have Godot refuse the other two before the call was ever made, so the
            // parameter is reported untyped and checked where it is converted instead.
            Out.VariantTag = ParamType.Described.Type == VH_TYPE_ARRAY
                ? VH_VARIANT_NIL
                : ParamType.Described.VariantTag;
            DescribeClassOf(ParamType, *Program, Out.ClassName, Out.ClassKind);
            Out.bHasDefault = Param->HasInitializer();
            if (!Out.bHasDefault)
            {
                Desc.RequiredParamCount = Desc.Params.Num();
            }
        }

        const FMemberType ResultType = DescribeType(&Type->GetReturnType(), *Program);
        Desc.ResultType = ResultType.Described.Type;
        Desc.ResultVariantTag = ResultType.Described.VariantTag;
        DescribeClassOf(ResultType, *Program, Desc.ResultClassName, Desc.ResultClassKind);

        if (OverridesMirroredDefinition(*Function))
        {
            Desc.GodotVirtual = GodotVirtualNameOf(FUtf8StringView(Desc.Name));
        }

        FUtf8String DeclaredIn;
        FillLocation(*Function, DeclaredIn, Desc.Line, Desc.Column);

        OutMethods.Add(MoveTemp(Desc));
    }
    return true;
}

AUTORTFM_DISABLE bool GetClassExportsLive(FUtf8StringView ClassName, TArray<GodotVerse::FExportDesc>& OutExports)
{
    using GodotVerse::FExportDesc;

    OutExports.Reset();

    if (!GIde.IsValid())
    {
        return false;
    }

    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager();
    if (!BuildManager.IsValid())
    {
        return false;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;

    // Absent when the attribute package did not make it into the program, which also means no
    // script could have applied the attribute -- an empty list would claim the script exports
    // nothing, so this reports "cannot answer" instead.
    const uLang::CClass* ExportAttribute =
        Program->FindDefinitionByVersePath<uLang::CClass>(ExportAttributePath);
    if (!ExportAttribute)
    {
        return false;
    }

    const FUtf8String ClassPath = FUtf8String(ScriptVersePath) + UTF8TEXT("/") + FUtf8String(ClassName);
    const uLang::CClass* Class = Program->FindDefinitionByVersePath<uLang::CClass>(
        FULangConversionUtils::FUtf8StringViewToULangStringView(ClassPath));
    if (!Class)
    {
        return false;
    }

    // R-EXP-1's five, which say what a `string` or an `int` is *for* -- the one thing a declared
    // type cannot. Looked up once for the whole class rather than per member.
    struct FHintAttribute
    {
        const char* Path;
        int32 Hint;
        /// The vh_type the member must be declared as. An attribute that describes a path on an
        /// int describes nothing, and this is the only place that pairing can be checked.
        int32 RequiredType;
        /// Whether the attribute carries a hint string, or is a marker like `@tool`.
        bool bHasText;
    };
    const FHintAttribute HintAttributes[] = {
        {ExportFileAttributePath, VH_EXPORT_HINT_FILE, VH_TYPE_STRING, true},
        {ExportDirAttributePath, VH_EXPORT_HINT_DIR, VH_TYPE_STRING, false},
        {ExportMultilineAttributePath, VH_EXPORT_HINT_MULTILINE, VH_TYPE_STRING, false},
        {ExportFlagsAttributePath, VH_EXPORT_HINT_FLAGS, VH_TYPE_INT, true},
        {ExportNodePathAttributePath, VH_EXPORT_HINT_NODE_PATH, VH_TYPE_STRING, true},
    };

    const uLang::CClass* CategoryAttribute = Program->FindDefinitionByVersePath<uLang::CClass>(ExportCategoryAttributePath);
    const uLang::CClass* GroupAttribute = Program->FindDefinitionByVersePath<uLang::CClass>(ExportGroupAttributePath);
    const uLang::CClass* SubgroupAttribute = Program->FindDefinitionByVersePath<uLang::CClass>(ExportSubgroupAttributePath);

    // The class's own members *and* those of any script class it derives from, base first -- which
    // is the order an inspector draws them in, and the order a reader expects.
    //
    // The walk stops at the first class outside the script package, because everything above that is
    // generated API: a mirrored class' members are Godot's own properties, which Godot already draws
    // and which carry no @export. Before script-to-script inheritance was tested there was no class
    // between the two, and this loop read only the one class.
    const TArray<const uLang::CClass*> Chain = ClassChainOfOrigin(*Class, *Program, EClassOrigin::Script);

    TArray<const uLang::TSRef<uLang::CDataDefinition>> Members;
    for (const uLang::CClass* Link : Chain)
    {
        for (const uLang::TSRef<uLang::CDataDefinition>& Member : Link->GetDefinitionsOfKind<uLang::CDataDefinition>())
        {
            Members.Add(Member);
        }
    }

    for (const uLang::TSRef<uLang::CDataDefinition>& Member : Members)
    {
        if (!Member->HasAttributeSubclass(ExportAttribute, *Program))
        {
            continue;
        }

        FExportDesc Desc;
        Desc.Name = FUtf8String(Member->AsNameCString());
        Desc.bIsVar = Member->IsVar();
        DescribeExportType(Member->GetType(), *Program, Desc);

        // The outermost section a member opens wins, since a member cannot be the first of a group
        // and the first of the category above it at once -- and the deeper one would be drawn
        // inside whatever came before, which is not what naming a category asks for.
        const FUtf8String Category = AttributeText(*Member, CategoryAttribute, *Program);
        const FUtf8String Group = AttributeText(*Member, GroupAttribute, *Program);
        const FUtf8String Subgroup = AttributeText(*Member, SubgroupAttribute, *Program);
        if (Member->HasAttributeSubclass(CategoryAttribute, *Program))
        {
            Desc.GroupKind = VH_EXPORT_GROUP_CATEGORY;
            Desc.GroupName = Category;
        }
        else if (Member->HasAttributeSubclass(GroupAttribute, *Program))
        {
            Desc.GroupKind = VH_EXPORT_GROUP_GROUP;
            Desc.GroupName = Group;
        }
        else if (Member->HasAttributeSubclass(SubgroupAttribute, *Program))
        {
            Desc.GroupKind = VH_EXPORT_GROUP_SUBGROUP;
            Desc.GroupName = Subgroup;
        }

        // One of R-EXP-1's five, which *replaces* whatever the type implied: the whole reason to
        // write one is that the type said nothing useful. Applied after the type description rather
        // than before it, so that the range a bounded int would have got is the thing overridden
        // and not the other way round.
        //
        // Only the first is taken. Two of these on one member is two descriptions of one field and
        // Godot draws one, so the alternative is choosing silently between them.
        for (const FHintAttribute& Attribute : HintAttributes)
        {
            const uLang::CClass* const HintClass =
                Program->FindDefinitionByVersePath<uLang::CClass>(Attribute.Path);
            if (!HintClass || !Member->HasAttributeSubclass(HintClass, *Program))
            {
                continue;
            }

            // Recorded even when the type is wrong, because the consumer's message names the
            // attribute the author wrote and there is nowhere else to carry it.
            Desc.Hint = Attribute.Hint;
            Desc.HintString = Attribute.bHasText ? AttributeText(*Member, HintClass, *Program) : FUtf8String();
            if (Desc.Reject == VH_EXPORT_OK && Desc.Type != Attribute.RequiredType)
            {
                Desc.Reject = VH_EXPORT_HINT_WRONG_TYPE;
            }
            break;
        }

        FUtf8String DeclaredIn;
        FillLocation(*Member, DeclaredIn, Desc.Line, Desc.Column);

        OutExports.Add(MoveTemp(Desc));
    }
    return true;
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::GetClassMethods(FUtf8StringView ClassName, TArray<FMethodDesc>& OutMethods)
{
    OutMethods.Reset();
    const FAnalysisSnapshot::FClass* const Found =
        GSnapshot ? GSnapshot->Classes.Find(FUtf8String(ClassName)) : nullptr;
    if (!Found)
    {
        return false;
    }
    OutMethods = Found->Methods;
    return true;
}

AUTORTFM_DISABLE bool GodotVerse::GetClassExports(FUtf8StringView ClassName, TArray<FExportDesc>& OutExports)
{
    OutExports.Reset();
    const FAnalysisSnapshot::FClass* const Found =
        GSnapshot ? GSnapshot->Classes.Find(FUtf8String(ClassName)) : nullptr;
    if (!Found || !Found->bExportsHarvested)
    {
        return false;
    }
    OutExports = Found->Exports;
    return true;
}

namespace {

/// Whether a declared type is a `signal(...)`: a class whose chain reaches the native
/// `vh_signal`, which is the one thing every signal type has in common and the one thing a script
/// cannot accidentally be.
AUTORTFM_DISABLE bool IsSignalClass(const uLang::CClass& Declared)
{
    for (const uLang::CClass* Cursor = &Declared; Cursor != nullptr; Cursor = Cursor->GetSuperClass())
    {
        if (FUtf8StringView(Cursor->AsNameCString()).Equals(UTF8TEXT("vh_signal")))
        {
            return true;
        }
    }
    return false;
}

/// Whether a declared type is a `/Verse.org/Verse` `event(t)`: a class whose chain reaches
/// `event_base_intrnl`, which is the root Epic gave the event family for exactly this kind of test.
///
/// The `@export_signal` half of R-SIG-1. Keyed on the base rather than on `event` itself so a
/// subclass of one still counts, and on the *name* rather than on an interface: `listenable(t)` is
/// `awaitable` + `subscribable` and does not extend `signalable`, so it can neither be signalled
/// into nor heard out of (docs/signal-declaration.md 3).
AUTORTFM_DISABLE bool IsEventClass(const uLang::CClass& Declared)
{
    for (const uLang::CClass* Cursor = &Declared; Cursor != nullptr; Cursor = Cursor->GetSuperClass())
    {
        if (FUtf8StringView(Cursor->AsNameCString()).Equals(UTF8TEXT("event_base_intrnl")))
        {
            return true;
        }
    }
    return false;
}

/// The payload type of a signal class: the type argument the member's declaration instantiated it
/// with.
///
/// The declared type comes back as the *generic* `signal(t)` -- `AsCode` prints it that way
/// and its `Signal` method's parameter is still the type variable -- but the instantiation is
/// recorded on the class as a substitution table, with one entry per polarity. Both carry the same
/// type for a class this shape, so the first is the answer.
AUTORTFM_DISABLE const uLang::CTypeBase* SignalPayloadType(const uLang::CClass& Declared)
{
    for (const uLang::STypeVariableSubstitution& Substitution : Declared._TypeVariableSubstitutions)
    {
        if (Substitution._PositiveType)
        {
            return Substitution._PositiveType;
        }
    }
    return nullptr;
}

/// The argument name Godot is told, for a payload that carries no names of its own.
///
/// Verse tuples cannot name their elements -- `tuple(Damage:int, ...)` is "Expected a type, got
/// data definition instead" -- so a tuple payload gets positional names and a bare one is named
/// for its type, which is what the connect dialog and `_make_function` then write. A struct payload
/// is the spelling that *does* carry names, and never reaches here.
AUTORTFM_DISABLE FUtf8String SignalArgName(const FMemberType& Arg, int32 Index, bool bIsTuple)
{
    if (bIsTuple)
    {
        return FUtf8String(UTF8TEXT("Arg")) + FUtf8String::FromInt(Index);
    }
    switch (Arg.Described.Type)
    {
    case VH_TYPE_LOGIC:  return FUtf8String(UTF8TEXT("Logic"));
    case VH_TYPE_INT:    return FUtf8String(UTF8TEXT("Int"));
    case VH_TYPE_FLOAT:  return FUtf8String(UTF8TEXT("Float"));
    case VH_TYPE_STRING: return FUtf8String(UTF8TEXT("Text"));
    case VH_TYPE_ARRAY:  return FUtf8String(UTF8TEXT("Items"));
    case VH_TYPE_REF:    return FUtf8String(UTF8TEXT("Ref"));
    default:             return FUtf8String(UTF8TEXT("Value"));
    }
}

/// Whether the wire can carry one payload argument of this declared type, and it is deliberately
/// *not* `Described.Reject == VH_EXPORT_OK`.
///
/// Four of the export rejections are rules about the inspector rather than about the wire, and a
/// signal argument is subject to none of them:
///
///   - VH_EXPORT_OBJECT_NOT_OPTIONAL is "the inspector can leave a slot empty". Nothing leaves a
///     signal argument empty -- the emitter supplies it -- and ValueToWire has the bare-object
///     branch for exactly this case, added when `signal(node2d)` emitted nothing.
///   - VH_EXPORT_SCRIPT_CLASS_NOT_GLOBAL and VH_EXPORT_BINDING_CLASS_UNSUPPORTED are both
///     "ClassDB cannot filter a picker by this name" -- the latter for a generated-binding class
///     rather than an unregistered project one. An emission carries a handle; nobody filters
///     anything, and DeclaredReferenceClass's FindBindingClass arm resolves the class the same way
///     an ordinary method argument does.
///   - VH_EXPORT_UNSUPPORTED_TYPE over a *reference* wrapper, a `variant` or a `rid` is "the
///     inspector has no editor for this". Each crosses perfectly well as one Godot value, which is
///     why DescribeExportType types it before rejecting it.
///
/// What is left really is unrepresentable: an option around a non-object (ValueToWire reads a
/// cleared option as a null reference, so `?int` would arrive as nothing), and a type with no lane.
AUTORTFM_DISABLE bool PayloadArgCrosses(const FMemberType& Arg)
{
    VH_EXHAUSTIVE_SWITCH_BEGIN
    switch (static_cast<vh_export_reject>(Arg.Described.Reject))
    {
    case VH_EXPORT_OK:
    case VH_EXPORT_OBJECT_NOT_OPTIONAL:
    case VH_EXPORT_SCRIPT_CLASS_NOT_GLOBAL:
    case VH_EXPORT_BINDING_CLASS_UNSUPPORTED:
        return true;
    case VH_EXPORT_UNSUPPORTED_TYPE:
        return Arg.Kind == EDeclaredKind::Container || Arg.Kind == EDeclaredKind::Variant
            || Arg.Kind == EDeclaredKind::Rid;
    case VH_EXPORT_OPTION_NOT_OBJECT:
    case VH_EXPORT_HINT_WRONG_TYPE:
        return false;
    }
    VH_EXHAUSTIVE_SWITCH_END
    return false;
}

/// What a payload becomes on Godot's side (phase-4-design 6.2), and why it cannot become anything.
///
/// A `tuple()` is no arguments; a tuple of N is N; a struct is one per top-level field, named by
/// the field; anything else is one. The mapping is one level only -- a `vector2` payload is one
/// Vector2 argument, not two floats -- and it is the same list the signal descriptor reports and an
/// emission fills, so the arguments a generated handler is written for and the arguments that
/// arrive cannot disagree.
///
/// This is the one place that decides, so G1-G4's rejections are decidable from the declaration:
/// the editor reports them at the member and no emission has to discover them at runtime.
AUTORTFM_DISABLE void DescribePayload(const uLang::CTypeBase* Payload,
                                      const uLang::CSemanticProgram& Program,
                                      FPayloadShape& OutShape)
{
    OutShape = FPayloadShape{};
    if (!Payload)
    {
        return;
    }

    const auto AddArg = [&OutShape](FUtf8String Name, FUtf8String FieldKey, FMemberType Type) {
        FPayloadArg& Arg = OutShape.Args.AddDefaulted_GetRef();
        Arg.Name = MoveTemp(Name);
        Arg.FieldKey = MoveTemp(FieldKey);
        Arg.Type = MoveTemp(Type);
    };

    const FDeclaredType Declared = ClassifyDeclaredType(Payload, Program);

    // The payload as one value, for the direction that has to reassemble it. A tuple has no
    // description of its own -- DescribeType would answer "nothing" for it -- so Kind and Args are
    // what the tuple case is rebuilt from and this is only read for Bare and Struct.
    OutShape.Whole = DescribeType(Payload, Program);

    if (const uLang::CTupleType* Tuple = Declared.Normal->AsNullable<uLang::CTupleType>())
    {
        OutShape.Kind = EPayloadShape::Tuple;
        for (const uLang::CTypeBase* Element : Tuple->GetElements())
        {
            FMemberType Described = DescribeType(Element, Program);
            AddArg(SignalArgName(Described, OutShape.Args.Num(), true), FUtf8String(), MoveTemp(Described));
        }
    }
    else if (const uLang::CClass* const Struct = PayloadStructClass(Declared))
    {
        OutShape.Kind = EPayloadShape::Struct;
        OutShape.StructClass = Struct;

        // The same walk the inbound direction uses, so the order Godot is told the arguments come
        // in and the order they are read back cannot disagree.
        FUserStructLayout Layout;
        Layout.DecoratedName = DecoratedNameOf(*Struct);
        CollectStructFields(*Struct, Program, Layout);

        for (int32 Index = 0; Index < Layout.FieldNames.Num(); ++Index)
        {
            if (Layout.FieldTypes[Index].UserStruct.IsValid())
            {
                // One level, and no more. Godot has no argument shape for "a struct", so the second
                // level has nothing to decompose into and silently dropping it would be the
                // accepted-but-broken failure this whole pass exists to remove.
                OutShape.Reject = VH_SIGNAL_PAYLOAD_NESTED_STRUCT;
                OutShape.RejectDetail = Layout.FieldNames[Index];
                return;
            }
            AddArg(Layout.FieldNames[Index], Layout.FieldKeys[Index], Layout.FieldTypes[Index]);
        }
    }
    else
    {
        OutShape.Kind = EPayloadShape::Bare;
        FMemberType Described = DescribeType(Payload, Program);
        AddArg(SignalArgName(Described, 0, false), FUtf8String(), MoveTemp(Described));
    }

    for (const FPayloadArg& Arg : OutShape.Args)
    {
        if (!PayloadArgCrosses(Arg.Type))
        {
            OutShape.Reject = VH_SIGNAL_PAYLOAD_UNSUPPORTED;
            OutShape.RejectDetail = Arg.Name;
            return;
        }
    }
}

std::string_view DiagArg(const FUtf8String& Text)
{
    return std::string_view(reinterpret_cast<const char*>(*Text), (size_t)Text.Len());
}

/// Reports one of the registry's sentences (include/verse_diagnostics.def), the one vm/ and src/
/// print for the same thing.
AUTORTFM_DISABLE void ReportVerseDiag(verse_diag Id, std::initializer_list<verse_diag_arg> Args = {})
{
    const std::string Text = verse_diag_text(Id, Args);
    GodotVerse::ReportError(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text.data()), (int32)Text.size()));
}

/// Why a signal was refused: the VG20xx sentence the editor shows at the member's own line, which
/// is what a game running outside the editor has instead of that line.
AUTORTFM_DISABLE std::string SignalRejectReason(const FUtf8String& Name, int32 Reject, const FUtf8String& Detail)
{
    return verse_signal_rejection(DiagArg(Name), Reject, DiagArg(Detail));
}

/// The UObject a class-typed member holds, or null.
///
/// The read half of WriteFieldOf, narrowed to the one case signal binding needs: a `signal`
/// member's own object, so the host can write the id into it.
AUTORTFM_DISABLE UObject* PeekFieldObject(UObject* Object, FUtf8StringView FieldName)
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

} // namespace

AUTORTFM_DISABLE void GodotVerse::AdoptCookedGeneration(FUtf8StringView PackageName, int32 Generation)
{
    GScriptPackageName = FUtf8String(PackageName);
    GScriptSourcePackageName = GScriptPackageName;
    GScriptGeneration = Generation;
    GProjectBuilt = true;
}

AUTORTFM_DISABLE const TSharedPtr<const GodotVerse::FAnalysisSnapshot>& GodotVerse::GetAnalysisSnapshot()
{
    return GSnapshot;
}

AUTORTFM_DISABLE void GodotVerse::SetAnalysisSnapshot(TSharedRef<const GodotVerse::FAnalysisSnapshot> Snapshot)
{
    GSnapshot = Snapshot;
}

AUTORTFM_DISABLE bool GodotVerse::IsClassAbstract(FUtf8StringView ClassName)
{
    const FAnalysisSnapshot::FClass* const Found =
        GSnapshot ? GSnapshot->Classes.Find(FUtf8String(ClassName)) : nullptr;
    return Found != nullptr && Found->bAbstract;
}

namespace {
/// The two things `@statics` was chosen over a naming convention to make checkable (R-NODE-4).
///
/// A convention produces a silently empty statics module when it is mistyped, and that was the whole
/// argument for a declared association -- so an association naming a class that does not exist, and
/// two modules claiming one class, have to actually say so or the attribute bought nothing.
///
/// Run once per analysis rather than from GetClassStatics, which is asked about one class at a time
/// and so can never see a module naming a class that is not there.
AUTORTFM_DISABLE void ReportStaticsDiagnostics()
{
    if (!GIde.IsValid())
    {
        return;
    }
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager();
    if (!BuildManager.IsValid())
    {
        return;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;

    const uLang::CClass* const StaticsAttribute =
        Program->FindDefinitionByVersePath<uLang::CClass>(StaticsAttributePath);
    const uLang::CModule* const Root = Program->FindDefinitionByVersePath<uLang::CModule>(ScriptVersePath);
    if (!StaticsAttribute || !Root)
    {
        return;
    }

    // Claimed class name -> the module that claimed it first, so the second one has something to
    // name. Modules are walked in declaration order, so "first" is stable across analyses.
    TMap<FUtf8String, FUtf8String> ClaimedBy;

    for (const uLang::TSRef<uLang::CModule>& Module : Root->GetDefinitionsOfKind<uLang::CModule>())
    {
        const uLang::TOptional<uLang::CUTF8String> Text =
            Module->GetAttributes().GetAttributeTextValue(StaticsAttribute, *Program);
        if (!Text.IsSet())
        {
            continue;
        }
        const FUtf8String ClassName = FULangConversionUtils::ULangStrToFUtf8String(*Text);
        const FUtf8String ModuleName = FUtf8String(Module->AsNameCString());

        FUtf8String Path;
        int32 Line = -1;
        int32 Column = -1;
        FillLocation(*Module, Path, Line, Column);

        const FUtf8String ClassPath = FUtf8String(ScriptVersePath) + UTF8TEXT("/") + ClassName;
        if (!Program->FindDefinitionByVersePath<uLang::CClass>(
                FULangConversionUtils::FUtf8StringViewToULangStringView(ClassPath)))
        {
            GodotVerse::ReportDiagnostic(VH_SEVERITY_ERROR,
                FUtf8StringView(FUtf8String(UTF8TEXT("`@statics(\"")) + ClassName + UTF8TEXT("\")` on module `")
                    + ModuleName + UTF8TEXT("` names a class no script declares, so nothing will ever read these ")
                    + UTF8TEXT("constants. The name is the class's Verse name, module-qualified the way every ")
                    + UTF8TEXT("other one is -- `gameplay/player` for a class in a module.")),
                FUtf8StringView(Path), Line, Column, Line, Column, FUtf8StringView(), 0);
            continue;
        }

        if (const FUtf8String* const First = ClaimedBy.Find(ClassName))
        {
            GodotVerse::ReportDiagnostic(VH_SEVERITY_ERROR,
                FUtf8StringView(FUtf8String(UTF8TEXT("Modules `")) + *First + UTF8TEXT("` and `") + ModuleName
                    + UTF8TEXT("` both declare themselves the statics of `") + ClassName
                    + UTF8TEXT("`. A class has one statics module: Godot asks it for a single constant map, so ")
                    + UTF8TEXT("which of the two answers is not a question the bridge can decide. Merge them.")),
                FUtf8StringView(Path), Line, Column, Line, Column, FUtf8StringView(), 0);
            continue;
        }
        ClaimedBy.Add(ClassName, ModuleName);
    }
}
}

namespace {

/// The walk GetClassStatics used to be, off the semantic program and the published package. Still
/// the only implementation: what changed is that it runs once per analysis, on the game thread,
/// rather than once per ask -- reading a constant's value enters the VM, which a running build
/// forbids and which is why this used to join the worker first.
AUTORTFM_DISABLE bool GetClassStaticsLive(FUtf8StringView ClassName,
                                          TArray<GodotVerse::FStaticDesc>& OutStatics,
                                          TArray<vh_value>& OutValues,
                                          TArray<GodotVerse::FFieldStorage>& OutStorage)
{
    using GodotVerse::FStaticDesc;

    OutStatics.Reset();
    OutValues.Reset();
    OutStorage.Reset();

    if (!GIde.IsValid())
    {
        return false;
    }
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager();
    if (!BuildManager.IsValid())
    {
        return false;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;

    const FUtf8String ClassPath = FUtf8String(ScriptVersePath) + UTF8TEXT("/") + FUtf8String(ClassName);
    if (!Program->FindDefinitionByVersePath<uLang::CClass>(
            FULangConversionUtils::FUtf8StringViewToULangStringView(ClassPath)))
    {
        return false;
    }

    const uLang::CClass* const StaticsAttribute =
        Program->FindDefinitionByVersePath<uLang::CClass>(StaticsAttributePath);
    const uLang::CModule* const Root = Program->FindDefinitionByVersePath<uLang::CModule>(ScriptVersePath);
    if (!StaticsAttribute || !Root)
    {
        // No attribute package means no script could have applied the attribute, so "no statics"
        // is the truthful answer rather than a refusal.
        return true;
    }

    // The undecorated class name, because that is what `@statics("player")` carries: the module
    // path is the script's own, and a module in a Verse module says `gameplay/player` the way every
    // other ClassNameUtf8 in the ABI does.
    for (const uLang::TSRef<uLang::CModule>& Module : Root->GetDefinitionsOfKind<uLang::CModule>())
    {
        const uLang::TOptional<uLang::CUTF8String> Text =
            Module->GetAttributes().GetAttributeTextValue(StaticsAttribute, *Program);
        if (!Text.IsSet())
        {
            continue;
        }
        if (!FUtf8StringView(FULangConversionUtils::ULangStrToFUtf8String(*Text)).Equals(ClassName))
        {
            continue;
        }

        for (const uLang::TSRef<uLang::CFunction>& Function : Module->GetDefinitionsOfKind<uLang::CFunction>())
        {
            FStaticDesc Desc;
            Desc.Name = FUtf8String(Function->AsNameCString());
            Desc.bIsFunction = true;
            FUtf8String DeclaredIn;
            FillLocation(*Function, DeclaredIn, Desc.Line, Desc.Column);
            OutStatics.Add(MoveTemp(Desc));
            OutValues.AddZeroed();
            OutStorage.AddDefaulted();
        }

        for (const uLang::TSRef<uLang::CDataDefinition>& Member : Module->GetDefinitionsOfKind<uLang::CDataDefinition>())
        {
            FStaticDesc Desc;
            Desc.Name = FUtf8String(Member->AsNameCString());
            Desc.bIsFunction = false;
            FUtf8String DeclaredIn;
            FillLocation(*Member, DeclaredIn, Desc.Line, Desc.Column);

            const int32 Slot = OutStatics.Add(MoveTemp(Desc));
            // Zeroed, not defaulted: ValueToWire writes only the lanes the value fills, so an int
            // or a string left its VariantTag as whatever the heap held, and the cook wrote that.
            OutValues.AddZeroed();
            OutStorage.AddDefaulted();

            // The value, read out of the published package rather than evaluated: a module's
            // constants are definitions of that package, looked up by the same decorated path the
            // enum reader builds. A build that has not happened yet has no package, and the name
            // is still reported -- which is the same bargain an `@export` default makes.
            const FUtf8String Decorated = FUtf8String(UTF8TEXT("("))
                + FULangConversionUtils::ULangStrToFUtf8String(
                      Member->_EnclosingScope.GetScopePath('/', uLang::CScope::EPathMode::PrefixSeparator))
                + UTF8TEXT(":)") + FUtf8String(Member->AsNameCString());

            const FMemberType Declared = DescribeType(Member->GetType(), *Program);
            Verse::FRunningContext Context = Verse::FRunningContextPromise{};
            EnterVerse(Context, [&] {
                Verse::VPackage* const Package =
                    Verse::GlobalProgram ? Verse::GlobalProgram->LookupPackage(GScriptPackageName) : nullptr;
                if (!Package)
                {
                    return;
                }
                const Verse::VValue Value = Package->LookupDefinition(FUtf8StringView(Decorated));
                if (Value.IsUninitialized())
                {
                    return;
                }
                ValueToWire(Context, Value, Declared, OutStorage[Slot], OutValues[Slot]);
            });
        }
    }
    return true;
}

AUTORTFM_DISABLE bool GetClassSignalsLive(FUtf8StringView ClassName, TArray<GodotVerse::FSignalDesc>& OutSignals)
{
    using GodotVerse::FParamDesc;
    using GodotVerse::FSignalDesc;

    OutSignals.Reset();

    if (!GIde.IsValid())
    {
        return false;
    }
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager();
    if (!BuildManager.IsValid())
    {
        return false;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;

    // Absent when the attribute package did not make it into the program, which also means no
    // script could have applied it. Unlike GetClassExportsLive's, this is not a reason to refuse
    // the whole answer: the `signal(t)` spelling needs no attribute, so the list is still correct
    // for every class that has not adopted the newer one.
    const uLang::CClass* const ExportSignalAttribute =
        Program->FindDefinitionByVersePath<uLang::CClass>(ExportSignalAttributePath);

    const FUtf8String ClassPath = FUtf8String(ScriptVersePath) + UTF8TEXT("/") + FUtf8String(ClassName);
    const uLang::CClass* Class = Program->FindDefinitionByVersePath<uLang::CClass>(
        FULangConversionUtils::FUtf8StringViewToULangStringView(ClassPath));
    if (!Class)
    {
        return false;
    }

    // Whether anything ever hands this class a handle. A handle arrives exactly one way -- Godot
    // attaching the script to an object it made -- and Instantiate refuses a class that does not
    // derive from `object`, so a signal on one is a member that can never be registered, connected
    // or emitted. GDScript has no equivalent of this because every GDScript class extends Object
    // and so carries a signal table of its own; a plain Verse class is a VM object with no Godot
    // counterpart at all. Asked of the leaf, which is the class Godot instantiates.
    const bool bHasGodotOwner = !NativeClassOf(*Class, *Program).IsEmpty();

    // Base first, and the whole chain: **signals inherit**. Phase 2 shipped exactly this bug once
    // already, for @export on a base script class, in two places that had been correct right up
    // until a script could derive from a script.
    const TArray<const uLang::CClass*> Chain = ClassChainOfOrigin(*Class, *Program, EClassOrigin::Script);

    for (const uLang::CClass* Link : Chain)
    {
        for (const uLang::TSRef<uLang::CDataDefinition>& Member : Link->GetDefinitionsOfKind<uLang::CDataDefinition>())
        {
            bool bIsOption = false;
            const uLang::CTypeBase* const MemberType = Member->GetType();
            const uLang::CNormalType* const Normal = MemberType ? &UnwrapDeclaredType(*MemberType, bIsOption) : nullptr;
            const uLang::CClass* const Declared = Normal ? Normal->AsNullable<uLang::CClass>() : nullptr;

            // `@export_signal` is what registers a member with Godot, whichever type declares it --
            // the same bargain `@export` makes for the inspector (R-SIG-1). What the two types
            // differ in is what *silence* means, which is why they are told apart here rather than
            // folded into one test:
            //
            //   - an `event(t)` is useful purely between Verse tasks, so one without the attribute
            //     is not a signal and not a complaint. It is absent from the list entirely.
            //   - a `signal(t)` has no purpose but Godot, so one without the attribute is far
            //     likelier to have forgotten it than to have meant it. It is listed and refused,
            //     which puts the sentence at the member's own line instead of leaving the Node
            //     panel empty for no stated reason.
            const bool bIsSignalType = Declared && IsSignalClass(*Declared);
            const bool bIsEventType = Declared && IsEventClass(*Declared);
            if (!bIsSignalType && !bIsEventType)
            {
                continue;
            }
            const bool bCarriesAttribute = ExportSignalAttribute
                && Member->GetAttributes().HasAttributeClass(ExportSignalAttribute, *Program);
            if (bIsEventType && !bCarriesAttribute)
            {
                continue;
            }

            FSignalDesc Desc;
            Desc.Name = FUtf8String(Member->AsNameCString());

            FPayloadShape Shape;
            DescribePayload(SignalPayloadType(*Declared), *Program, Shape);
            for (const FPayloadArg& Arg : Shape.Args)
            {
                FParamDesc Param;
                Param.Name = Arg.Name;
                Param.Type = Arg.Type.Described.Type;
                Param.VariantTag = Arg.Type.Described.VariantTag;
                DescribeClassOf(Arg.Type, *Program, Param.ClassName, Param.ClassKind);
                Desc.Args.Add(MoveTemp(Param));
            }

            // Member first, payload second: "this cannot be a signal at all" is a better sentence
            // than "its third argument has no Godot type", and the author fixes the member either
            // way. Order within the two is declaration order.
            //
            // The missing attribute sits after those two and before the payload, and both sides of
            // that are deliberate. A member with no Godot owner is not fixed by an attribute, so
            // telling the author to write one there would be the wrong edit; and complaining about
            // the payload of a member Godot was never told about is noise before the edit that
            // matters.
            //
            // **The member's access level is not tested, and VH_SIGNAL_NOT_PUBLIC is retired.** It
            // read as a third rung here, on the reading that connecting is something done from
            // outside the class. Connecting is not done from Verse at all: a designer connects in
            // the Node panel and GDScript connects by string name, and neither consults a Verse
            // specifier. Nothing else in the bridge tests one either -- GetClassExportsLive and
            // GetClassMethodsLive never have, so a non-public `@export` member has always reached
            // the inspector and a non-public method has always been callable from Godot. What the
            // specifier still governs is which *Verse* code may name the member, which is the whole
            // of what it ever promised. tests/verse_probe/signal_access_probe.verse is the
            // measurement, and signal_shadow_probe.verse is why binding by name is still safe: the
            // compiler refuses a member shadowing an inaccessible one of the same name (3593), so
            // two members of one name cannot reach the list.
            if (!bHasGodotOwner)
            {
                Desc.Reject = VH_SIGNAL_NO_GODOT_OWNER;
            }
            else if (Member->IsVar())
            {
                Desc.Reject = VH_SIGNAL_IS_VAR;
            }
            else if (!bCarriesAttribute)
            {
                Desc.Reject = VH_SIGNAL_NEEDS_ATTRIBUTE;
            }
            else
            {
                Desc.Reject = Shape.Reject;
                Desc.RejectDetail = Shape.RejectDetail;
            }

            FUtf8String DeclaredIn;
            FillLocation(*Member, DeclaredIn, Desc.Line, Desc.Column);
            OutSignals.Add(MoveTemp(Desc));
        }
    }
    return true;
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::GetClassStatics(FUtf8StringView ClassName, TSharedPtr<const FClassStatics>& OutStatics)
{
    OutStatics.Reset();
    const FAnalysisSnapshot::FClass* const Found =
        GSnapshot ? GSnapshot->Classes.Find(FUtf8String(ClassName)) : nullptr;
    if (!Found || !Found->Statics)
    {
        return false;
    }
    OutStatics = Found->Statics;
    return true;
}

namespace {

/// One `@rpc` word, and which of the three categories it settles.
///
/// Godot's own seven and nothing else, matched exactly as GDScript matches them: the words are the
/// API here, and a near miss has to be a diagnostic rather than a default quietly applied.
struct FRpcWord
{
    const char* Word;
    /// 0 permission, 1 locality, 2 transfer mode. Two words of one category is an error, which is
    /// the only reason the category is carried rather than just the value.
    int32 Category;
    int32 Value;
};

constexpr FRpcWord RpcWords[] = {
    {"any_peer", 0, 1},   // MultiplayerAPI::RPC_MODE_ANY_PEER
    {"authority", 0, 2},  // MultiplayerAPI::RPC_MODE_AUTHORITY
    {"call_local", 1, 1},
    {"call_remote", 1, 0},
    {"unreliable", 2, 0}, // MultiplayerPeer::TRANSFER_MODE_UNRELIABLE
    {"unreliable_ordered", 2, 1},
    {"reliable", 2, 2},
};

constexpr const char* RpcCategoryNames[] = {
    "the permission (any_peer/authority)",
    "the locality (call_local/call_remote)",
    "the transfer mode (reliable/unreliable/unreliable_ordered)",
};

/// Splits an `@rpc` config string into its words. Spaces and commas both separate, so a GDScript
/// author who writes Godot's own `"any_peer", "call_local"` inside one string still gets what they
/// meant rather than an unknown word with quotes in it.
AUTORTFM_DISABLE void SplitRpcWords(const FUtf8String& Config, ::TArray<FUtf8String>& OutWords)
{
    FUtf8String Current;
    const auto Flush = [&Current, &OutWords] {
        if (!Current.IsEmpty())
        {
            OutWords.Add(Current);
            Current.Reset();
        }
    };
    for (int32 Index = 0; Index < Config.Len(); ++Index)
    {
        const UTF8CHAR Ch = Config[Index];
        if (Ch == UTF8CHAR(' ') || Ch == UTF8CHAR(',') || Ch == UTF8CHAR('\t')
            || Ch == UTF8CHAR('"') || Ch == UTF8CHAR('\n') || Ch == UTF8CHAR('\r'))
        {
            Flush();
            continue;
        }
        Current.AppendChar(Ch);
    }
    Flush();
}

/// Reads one `@rpc`'s config string into a description.
///
/// The words are matched rather than positional, which is GDScript's rule too, and a number is the
/// channel wherever it appears -- GDScript reads position 3 as the channel, but position means
/// nothing once the four arguments are one string, and "a number is the channel" is the only rule
/// left that a reader can state in one line.
AUTORTFM_DISABLE void ReadRpcConfig(const FUtf8String& Config, GodotVerse::FRpcDesc& OutDesc)
{
    ::TArray<FUtf8String> Words;
    SplitRpcWords(Config, Words);

    bool bCategorySeen[3] = {false, false, false};
    bool bChannelSeen = false;
    for (const FUtf8String& Word : Words)
    {
        // A number is the channel. Tested before the word table so that a Godot release adding a
        // numeric-looking word would be a compile failure here rather than a silent reinterpretation.
        bool bAllDigits = !Word.IsEmpty();
        for (int32 Index = 0; Index < Word.Len(); ++Index)
        {
            if (Word[Index] < UTF8CHAR('0') || Word[Index] > UTF8CHAR('9'))
            {
                bAllDigits = false;
                break;
            }
        }
        if (bAllDigits)
        {
            if (bChannelSeen)
            {
                OutDesc.Reject = VH_RPC_DUPLICATE_CATEGORY;
                OutDesc.RejectDetail = UTF8TEXT("the channel");
                return;
            }
            bChannelSeen = true;
            OutDesc.Channel = FCStringUtf8::Atoi(*Word);
            continue;
        }

        const FRpcWord* Matched = nullptr;
        for (const FRpcWord& Candidate : RpcWords)
        {
            if (Word.Equals(FUtf8String(Candidate.Word)))
            {
                Matched = &Candidate;
                break;
            }
        }
        if (!Matched)
        {
            OutDesc.Reject = VH_RPC_UNKNOWN_ARGUMENT;
            OutDesc.RejectDetail = Word;
            return;
        }
        if (bCategorySeen[Matched->Category])
        {
            OutDesc.Reject = VH_RPC_DUPLICATE_CATEGORY;
            OutDesc.RejectDetail = FUtf8String(RpcCategoryNames[Matched->Category]);
            return;
        }
        bCategorySeen[Matched->Category] = true;

        switch (Matched->Category)
        {
        case 0: OutDesc.RpcMode = Matched->Value; break;
        case 1: OutDesc.bCallLocal = Matched->Value != 0; break;
        default: OutDesc.TransferMode = Matched->Value; break;
        }
    }
}


/// Every method of ClassName carrying an `@rpc`, out of the program the analysis just built.
AUTORTFM_DISABLE bool GetClassRpcsLive(FUtf8StringView ClassName, TArray<GodotVerse::FRpcDesc>& OutRpcs)
{
    using namespace uLang;

    OutRpcs.Reset();
    if (!GIde.IsValid())
    {
        return false;
    }
    const TSPtr<CProgramBuildManager> BuildManager = GIde->GetBuildManager();
    if (!BuildManager.IsValid())
    {
        return false;
    }
    const TSRef<CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;
    const CClass* const RpcAttribute = Program->FindDefinitionByVersePath<CClass>(RpcAttributePath);
    if (!RpcAttribute)
    {
        // No attribute package in this program, which is the state before the first analysis --
        // not "this class has no RPCs", so it is false rather than an empty list.
        return false;
    }

    const FUtf8String ClassPath = FUtf8String(ScriptVersePath) + UTF8TEXT("/") + FUtf8String(ClassName);
    const CClass* const Class = Program->FindDefinitionByVersePath<CClass>(
        FULangConversionUtils::FUtf8StringViewToULangStringView(ClassPath));
    if (!Class)
    {
        return false;
    }

    for (const TSRef<CFunction>& Function : Class->GetDefinitionsOfKind<CFunction>())
    {
        if (!Function->GetAttributes().HasAttributeClass(RpcAttribute, *Program))
        {
            continue;
        }

        GodotVerse::FRpcDesc Desc;
        Desc.Name = FUtf8String(Function->AsNameCString());
        if (OverridesMirroredDefinition(*Function))
        {
            // The name Godot dispatches by, for the reason the method list reports one: an author
            // who writes `@rpc` over `_Process` means Godot's `_process`, and a config keyed by the
            // Verse spelling would name a method Godot never looks for.
            Desc.Name = GodotVirtualNameOf(FUtf8StringView(Desc.Name));
        }
        FUtf8String DeclaredIn;
        FillLocation(*Function, DeclaredIn, Desc.Line, Desc.Column);

        // One string, so GetAttributeTextValue reads it -- that function refuses anything whose
        // argument is a MakeTuple, which is every attribute of more than one argument, and is
        // the whole reason the words travel together rather than as GDScript's four.
        const uLang::TOptional<uLang::CUTF8String> Config =
            Function->GetAttributes().GetAttributeTextValue(RpcAttribute, *Program);
        if (Config.IsSet())
        {
            ReadRpcConfig(FULangConversionUtils::ULangStrToFUtf8String(*Config), Desc);
        }
        OutRpcs.Add(MoveTemp(Desc));
    }
    return true;
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::GetClassRpcs(FUtf8StringView ClassName, TArray<FRpcDesc>& OutRpcs)
{
    OutRpcs.Reset();
    const FAnalysisSnapshot::FClass* const Found =
        GSnapshot ? GSnapshot->Classes.Find(FUtf8String(ClassName)) : nullptr;
    if (!Found)
    {
        return false;
    }
    OutRpcs = Found->Rpcs;
    return true;
}

AUTORTFM_DISABLE bool GodotVerse::GetClassSignals(FUtf8StringView ClassName, TArray<FSignalDesc>& OutSignals)
{
    OutSignals.Reset();
    const FAnalysisSnapshot::FClass* const Found =
        GSnapshot ? GSnapshot->Classes.Find(FUtf8String(ClassName)) : nullptr;
    if (!Found)
    {
        return false;
    }
    OutSignals = Found->Signals;
    return true;
}

namespace {

/// Mints the binding rows for every `signal` member of a fresh instance, and writes each id
/// into the member's own object.
///
/// This is where a signal stops being a declaration and becomes a thing that can be emitted: the
/// member's *type* said what the payload is and its *name* is the signal's name, and both are
/// resolved here once rather than at every emission. S-A is the spike that says the write survives
/// -- two production paths already fill a class-typed member at construction.
/// Defined below, beside the await it was factored out of. Declared here because an
/// `@export_signal` member's connection is made at bind time rather than at a wait.
AUTORTFM_DISABLE int64 ConnectDelivery(int64 OwnerHandle,
                                       const FUtf8String& Name,
                                       FCallbackTarget Target,
                                       int32 ConnectFlags,
                                       int64& OutCallableRef);

AUTORTFM_DISABLE void BindSignals(UObject* Instance,
                                  FUtf8StringView ClassName,
                                  int64 Handle,
                                  TArray<int64>& OutEventBindings)
{
    TArray<GodotVerse::FSignalDesc> Signals;
    if (!GodotVerse::GetClassSignals(ClassName, Signals) || Signals.IsEmpty())
    {
        return;
    }

    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde.IsValid() ? GIde->GetBuildManager() : nullptr;
    const uLang::CSemanticProgram* const Program =
        BuildManager.IsValid() ? &*BuildManager->GetProgramContext()._Program : nullptr;
    const GodotVerse::FDeclaredTypes* const Recorded = Program ? nullptr : RecordedTypes(ClassName);
    if (!Program && !Recorded)
    {
        return;
    }

    for (const GodotVerse::FSignalDesc& Signal : Signals)
    {
        UObject* const Held = PeekFieldObject(Instance, FUtf8StringView(Signal.Name));
        verse::vh_signal* const Shadow = Cast<verse::vh_signal>(Held);
        verse::event* const Event = Shadow ? nullptr : Cast<verse::event>(Held);
        if (!Shadow && !Event)
        {
            // A declared signal whose member holds nothing. Saying so beats emitting into the void
            // later, which is what an unbound id does.
            GodotVerse::ReportError(FUtf8String(UTF8TEXT("The signal `")) + Signal.Name
                + UTF8TEXT("` on ") + FUtf8String(ClassName)
                + UTF8TEXT(" could not be bound: its member holds no signal object."));
            continue;
        }

        const FMemberType Declared = DescribeMemberType(ClassName, FUtf8StringView(Signal.Name));
        FSignalBinding Binding;
        Binding.OwnerHandle = Handle;
        Binding.Name = Signal.Name;
        Binding.Reject = Signal.Reject;
        Binding.RejectDetail = Signal.RejectDetail;
        if (Program && Declared.ReferenceClass)
        {
            DescribePayload(SignalPayloadType(*Declared.ReferenceClass), *Program, Binding.Payload);
        }
        else if (Recorded)
        {
            if (const FPayloadShape* const Shape = Recorded->Signals.Find(Signal.Name))
            {
                Binding.Payload = *Shape;
            }
        }

        const int64 Id = GNextSignalId++;
        if (Shadow)
        {
            GSignalBindings.Add(Id, MoveTemp(Binding));
            Shadow->Id.Init(Id, Shadow);
            continue;
        }

        // An `@export_signal` event member. There is no `Id` field to write -- `event(t)` is
        // Verse's own class and cannot be reopened -- so the binding holds the event instead, and
        // the emit and subscribe natives find the row by the object they are handed.
        Binding.Event = TStrongObjectPtr<UObject>(Held);
        const bool bRegistered = Binding.Reject == VH_SIGNAL_OK;
        GSignalBindings.Add(Id, MoveTemp(Binding));
        GEventBindingIds.Add(Held, Id);
        OutEventBindings.Add(Id);

        // The connection is *not* made here. This runs inside vh_instantiate, which the consumer
        // calls before it installs the script instance on the object, so Godot does not yet know
        // the script has a signal of this name and answers "Attempt to connect nonexistent signal".
        // AttachInstance below is the hook that runs once it does.
    }
}

} // namespace

AUTORTFM_DISABLE int64 GodotVerse::BindEngineSignal(int64 Handle,
                                                    FUtf8StringView ClassName,
                                                    FUtf8StringView AccessorName,
                                                    FUtf8StringView SignalName)
{
    // One row per (owner, signal). An accessor is a method, so it runs on every `Timer.Timeout()`
    // -- minting per call would grow the table for as long as the game does.
    const FUtf8String Key = FUtf8String::FromInt(Handle) + UTF8TEXT(":") + FUtf8String(SignalName);
    if (const int64* Existing = GEngineSignalIds.Find(Key))
    {
        return *Existing;
    }

    FSignalBinding Binding;
    Binding.OwnerHandle = Handle;
    Binding.Name = FUtf8String(SignalName);

    // The payload, off the accessor's own return type, cached per accessor rather than per object:
    // every Timer's `timeout` carries the same nothing.
    const FUtf8String Shape = FUtf8String(ClassName) + UTF8TEXT(".") + FUtf8String(AccessorName);
    if (const FSignalBinding* Cached = GEngineSignalShapes.Find(Shape))
    {
        Binding.Payload = Cached->Payload;
    }
    else if (GIde.IsValid())
    {
        if (const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager())
        {
            const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;
            const FUtf8String Path = FUtf8String(GodotVersePath) + UTF8TEXT("/") + FUtf8String(ClassName);
            if (const uLang::CClass* const Mirrored = Program->FindDefinitionByVersePath<uLang::CClass>(
                    FULangConversionUtils::FUtf8StringViewToULangStringView(Path)))
            {
                for (const uLang::TSRef<uLang::CFunction>& Function : Mirrored->GetDefinitionsOfKind<uLang::CFunction>())
                {
                    if (!FUtf8StringView(Function->AsNameCString()).Equals(AccessorName))
                    {
                        continue;
                    }
                    const uLang::CFunctionType* const Type = Function->_Signature.GetFunctionType();
                    bool bIsOption = false;
                    const uLang::CNormalType* const Returned =
                        Type ? &UnwrapDeclaredType(Type->GetReturnType(), bIsOption) : nullptr;
                    if (const uLang::CClass* const Signal = Returned ? Returned->AsNullable<uLang::CClass>() : nullptr)
                    {
                        DescribePayload(SignalPayloadType(*Signal), *Program, Binding.Payload);
                    }
                    break;
                }
            }
        }
        GEngineSignalShapes.Add(Shape, Binding);
    }
    else if (GRecordedEngineSignals)
    {
        if (const FPayloadShape* const Recorded = GRecordedEngineSignals->Shapes.Find(Shape))
        {
            Binding.Payload = *Recorded;
        }
        GEngineSignalShapes.Add(Shape, Binding);
    }

    const int64 Id = GNextSignalId++;
    GSignalBindings.Add(Id, MoveTemp(Binding));
    GEngineSignalIds.Add(Key, Id);
    return Id;
}

AUTORTFM_DISABLE void GodotVerse::EmitSignal(int64 SignalId, const FVerseValue& Payload)
{
    const FSignalBinding* const Binding = GSignalBindings.Find(SignalId);
    if (!Binding)
    {
        ReportVerseDiag(verse_diag::VG2105);
        return;
    }

    // A signal the editor already refused. Saying so again here is not redundant: the editor
    // warning is the only report a *tools* build makes, and a game running outside it would
    // otherwise get the generic "names nothing" for a member that was declared perfectly visibly.
    if (Binding->Reject != VH_SIGNAL_OK)
    {
        const std::string Reason = SignalRejectReason(Binding->Name, Binding->Reject, Binding->RejectDetail);
        ReportVerseDiag(verse_diag::VG2101, {{"signal", DiagArg(Binding->Name)}, {"reason", Reason}});
        return;
    }

    FHostState& Host = GetHost();
    if (!Host.Godot.EmitSignal)
    {
        return;
    }

    // One storage per argument: FFieldStorage carries a single Text, so two string arguments
    // sharing one would clobber each other.
    const FPayloadShape& Shape = Binding->Payload;
    const int32 Count = Shape.Args.Num();
    TArray<FFieldStorage> Storages;
    Storages.SetNum(Count);
    TArray<vh_value> Args;
    Args.SetNum(Count);

    bool bConverted = true;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    EnterVerse(Context, [&] {
        const Verse::VValue Value = Payload.GetValue();
        const Verse::VArrayBase* const Tuple = Shape.Kind == EPayloadShape::Tuple
            ? Value.DynamicCast<Verse::VArrayBase>()
            : nullptr;
        Verse::VValueObject* const Struct = Shape.Kind == EPayloadShape::Struct
            ? Value.DynamicCast<Verse::VValueObject>()
            : nullptr;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const FPayloadArg& Arg = Shape.Args[Index];
            Verse::VValue Element;
            switch (Shape.Kind)
            {
            case EPayloadShape::Tuple:
                // A Verse tuple is an array at runtime, and its elements are the arguments Godot sees.
                Element = Tuple && Index < (int32)Tuple->Num() ? Tuple->GetValue((uint32)Index) : Verse::VValue();
                break;
            case EPayloadShape::Struct:
            {
                // By name rather than by position, the way ReadStructComponents reads a mirrored
                // math type: a struct value carries its fields under decorated keys and nothing in
                // it says what order the declaration wrote them in.
                if (!Struct)
                {
                    bConverted = false;
                    return;
                }
                Verse::VUniqueString& Key = Verse::VUniqueString::New(Context, FUtf8StringView(Arg.FieldKey));
                const Verse::FOpResult Read = Struct->LoadField(Context, Key);
                if (!Read.IsReturn())
                {
                    bConverted = false;
                    return;
                }
                Element = Read.Value;
                break;
            }
            case EPayloadShape::Bare:
                Element = Value;
                break;
            }

            if (!ValueToWire(Context, Element, Arg.Type, Storages[Index], Args[Index]))
            {
                bConverted = false;
                return;
            }
        }
    });

    if (!bConverted)
    {
        ReportVerseDiag(verse_diag::VG2104, {{"signal", DiagArg(Binding->Name)}});
        return;
    }

    Host.Godot.EmitSignal(Host.Godot.Ctx,
                          Binding->OwnerHandle,
                          reinterpret_cast<const char*>(*Binding->Name),
                          Binding->Name.Len(),
                          Args.GetData(),
                          Args.Num());
}

AUTORTFM_DISABLE int64 GodotVerse::SubscribeSignal(int64 SignalId, const FVerseValue& Callback)
{
    const FSignalBinding* const Binding = GSignalBindings.Find(SignalId);
    if (!Binding)
    {
        ReportVerseDiag(verse_diag::VG2106);
        return 0;
    }

    // Same reason as the emission half: Godot was never told this signal exists, so `connect` would
    // refuse the name, and "connect failed" is a worse sentence than the one that says why.
    if (Binding->Reject != VH_SIGNAL_OK)
    {
        const std::string Reason = SignalRejectReason(Binding->Name, Binding->Reject, Binding->RejectDetail);
        ReportVerseDiag(verse_diag::VG2102, {{"signal", DiagArg(Binding->Name)}, {"reason", Reason}});
        return 0;
    }

    FHostState& Host = GetHost();
    if (!Host.Godot.ConnectSignal || !Host.Godot.ReleaseRef)
    {
        return 0;
    }

    const int64 CallableRef = MakeCallableFor(Callback);
    if (CallableRef == 0)
    {
        return 0;
    }

    vh_value Target{};
    Target.Type = VH_TYPE_REF;
    Target.VariantTag = VH_VARIANT_CALLABLE;
    Target.Ref = CallableRef;

    const int64 OwnerHandle = Binding->OwnerHandle;
    const FUtf8String Name = Binding->Name;
    const int32 Status = Host.Godot.ConnectSignal(
        Host.Godot.Ctx, OwnerHandle, reinterpret_cast<const char*>(*Name), Name.Len(), &Target, 0);
    if (Status != VH_CALL_OK)
    {
        Host.Godot.ReleaseRef(Host.Godot.Ctx, CallableRef);
        return 0;
    }

    const int64 Id = GNextSubscriptionId++;
    GSubscriptions.Add(Id, FSubscription{OwnerHandle, Name, CallableRef});

    // Compensated rather than deferred. This mutates Godot *and* returns a value, so it can be
    // neither queued for commit nor ignored -- and without the compensation a failed transaction
    // leaves a live connection the script believes it never made. The host's only rollback
    // compensation, and the shape to copy for anything later that mutates Godot and cannot defer.
    //
    // **Not `Verse::Stm::OnRollback`**, which is what Phase 4 wrote and what Phase 4.5's S-3
    // measured as doing nothing: that is the Solaris *interpreter's* STM, and `VerseStm.h` says of
    // it "Noop if StmActive() returns false" -- StmActive being "true if in a failure context",
    // which is a BPVM-era notion VerseVM never sets from here. The connection survived all three
    // kinds of failure.
    //
    // `SameAsClosed` is the load-bearing half. Every Godot callback reaches C++ through
    // `AutoRTFM::Open` (see VhSignalSubscribe), and a plain `OnAbort` from open code is documented
    // to be *ignored*; `SameAsClosed` registers it against the active transaction as if the call
    // had been closed, which is the one spelling that survives the Open this call is inside.
    AutoRTFM::OnAbort<AutoRTFM::EOpenBehavior::SameAsClosed>(
        [Id] { GodotVerse::CancelSubscription(Id); });
    return Id;
}

namespace {
/// The binding an `@export_signal` member's event names, or 0.
///
/// 0 means the member was never bound: a `signal(t)` a script built for itself answers the same
/// way, and both reach the "names nothing" sentence rather than silently emitting into the void.
AUTORTFM_DISABLE int64 EventBindingFor(UObject* Event)
{
    if (!Event)
    {
        return 0;
    }
    const int64* const Found = GEventBindingIds.Find(Event);
    return Found ? *Found : 0;
}
}

AUTORTFM_DISABLE void GodotVerse::EmitEventSignal(UObject* Event, const FVerseValue& Payload)
{
    const int64 SignalId = EventBindingFor(Event);
    if (SignalId == 0)
    {
        ReportVerseDiag(verse_diag::VG2107);
        return;
    }
    EmitSignal(SignalId, Payload);
}

AUTORTFM_DISABLE int64 GodotVerse::SubscribeEventSignal(UObject* Event, const FVerseValue& Callback)
{
    const int64 SignalId = EventBindingFor(Event);
    if (SignalId == 0)
    {
        ReportVerseDiag(verse_diag::VG2108);
        return 0;
    }
    return SubscribeSignal(SignalId, Callback);
}

AUTORTFM_DISABLE void GodotVerse::CancelSubscription(int64 SubscriptionId)
{
    const FSubscription* const Found = GSubscriptions.Find(SubscriptionId);
    if (!Found)
    {
        // Idempotent, as event_subscription::Cancel is in UEFN: a second Cancel does nothing and
        // says nothing.
        return;
    }
    const FSubscription Subscription = *Found;
    GSubscriptions.Remove(SubscriptionId);

    GodotVerse::FHostState& Host = GodotVerse::GetHost();
    if (Host.Godot.DisconnectSignal)
    {
        vh_value Target{};
        Target.Type = VH_TYPE_REF;
        Target.VariantTag = VH_VARIANT_CALLABLE;
        Target.Ref = Subscription.CallableRef;
        Host.Godot.DisconnectSignal(Host.Godot.Ctx,
                                    Subscription.OwnerHandle,
                                    reinterpret_cast<const char*>(*Subscription.Name),
                                    Subscription.Name.Len(),
                                    &Target);
    }
    if (Host.Godot.ReleaseRef)
    {
        Host.Godot.ReleaseRef(Host.Godot.Ctx, Subscription.CallableRef);
    }
}

namespace {

/// The `/Verse.org/Verse` event a `signal` holds, read off the object rather than named.
///
/// Found by walking the shape rather than by building the field's decorated key. The key of a data
/// member is `(<declaring class' scope path>:)<name>`, and for a member of a *parametric* class
/// there is more than one plausible spelling of that path -- so the walk asks the only question
/// that cannot be got wrong: which field holds an event.
AUTORTFM_DISABLE verse::event* FindEventField(Verse::FRunningContext Context, UObject* Object)
{
    if (!Object)
    {
        return nullptr;
    }
    Verse::VShape& Shape = UVerseClass::GetShapeForLoadField(Context, Object->GetClass());
    for (Verse::VShape::FieldsMap::TIterator It = Shape.CreateFieldsIterator(); It; ++It)
    {
        const Verse::VShape::VEntry& Entry = It.Value();
        Verse::VValue Value = Entry.Type == Verse::EFieldType::FPropertyVar
            ? Verse::VNativeRef::Peek(Context, Object, Entry.UProperty)
            : UVerseClass::PeekField(Context, Object, &Entry);
        if (Verse::VRef* Ref = Value.DynamicCast<Verse::VRef>())
        {
            Value = Ref->Get(Context);
        }
        if (verse::event* const Event = Cast<verse::event>(Value.ExtractUObject()))
        {
            return Event;
        }
    }
    return nullptr;
}

/// A Godot Array holding an emission's arguments, as the reference wrapper a `godot_array` is.
///
/// The one payload a foreign signal can carry: nothing declares its arguments, so there is no
/// per-argument type to convert against and the whole list crosses as the container Godot itself
/// would have put them in. Ownership of the fresh reference passes to the wrapper, whose
/// BeginDestroy releases it when Verse drops the value.
AUTORTFM_DISABLE bool ArgumentArrayValue(Verse::FRunningContext Context,
                                         const vh_value* Args,
                                         int32 ArgCount,
                                         Verse::VValue& OutValue)
{
    GodotVerse::FHostState& Host = GodotVerse::GetHost();
    if (!Host.Godot.NewRef || !Host.Godot.RefSet)
    {
        return false;
    }
    const int64 Ref = Host.Godot.NewRef(Host.Godot.Ctx, VH_VARIANT_ARRAY);
    if (Ref == 0)
    {
        return false;
    }
    for (int32 Index = 0; Index < ArgCount; ++Index)
    {
        vh_value Key{};
        Key.Type = VH_TYPE_INT;
        Key.Int = Index;
        Host.Godot.RefSet(Host.Godot.Ctx, Ref, &Key, &Args[Index]);
    }
    UObject* const Wrapper = NewReferenceWrapper(FindReferenceClass(VH_VARIANT_ARRAY), Ref);
    if (!Wrapper)
    {
        if (Host.Godot.ReleaseRef)
        {
            Host.Godot.ReleaseRef(Host.Godot.Ctx, Ref);
        }
        return false;
    }
    OutValue = Verse::VValue(Wrapper);
    return true;
}

/// Puts an emission's arguments back together as the one value the payload's type names.
///
/// The exact inverse of what DescribePayload took apart, and it has to be: `Await` answers `t`,
/// and `t` is what the declaration said rather than the argument list Godot carried.
AUTORTFM_DISABLE bool PayloadValue(Verse::FRunningContext Context,
                                   const FPayloadShape& Shape,
                                   const vh_value* Args,
                                   int32 ArgCount,
                                   Verse::VValue& OutValue)
{
    switch (Shape.Kind)
    {
    case EPayloadShape::Bare:
        return Shape.Args.Num() == 1 && ArgCount == 1
            && WireToValue(Context, Args[0], Shape.Args[0].Type, OutValue);

    case EPayloadShape::Tuple:
    {
        // A Verse tuple is an array at runtime, and a `tuple()` payload is an empty one -- which is
        // what an engine signal carrying nothing answers, and the commonest case there is.
        if (ArgCount != Shape.Args.Num())
        {
            return false;
        }
        TArray<Verse::VValue> Elements;
        Elements.Reserve(ArgCount);
        for (int32 Index = 0; Index < ArgCount; ++Index)
        {
            Verse::VValue Element;
            if (!WireToValue(Context, Args[Index], Shape.Args[Index].Type, Element))
            {
                return false;
            }
            Elements.Add(Element);
        }
        const auto Init = [&Elements](uint32 Index) { return Elements[(int32)Index]; };
        OutValue = Verse::VValue(Verse::VArray::New(Context, (uint32)Elements.Num(), Init));
        return true;
    }

    case EPayloadShape::Struct:
    {
        // The same rule InstanceCall applies to a struct parameter: N Godot arguments satisfy one
        // struct of N fields, and WireToValue is what builds it. Borrowed for the call, like every
        // other pointer on this wire.
        vh_value Packed{};
        Packed.Type = VH_TYPE_TUPLE;
        Packed.Seq.Items = Args;
        Packed.Seq.Count = ArgCount;
        return WireToValue(Context, Packed, Shape.Whole, OutValue);
    }
    }
    return false;
}

/// Resumes whatever is waiting on one await token, with the emission's arguments as its payload.
///
/// The resumption happens **inside the emission**, synchronously, which is where GDScript resumes a
/// coroutine too (`GDScriptFunctionState::_signal_callback` calls `resume()` from the connected
/// Callable). Nothing is queued and nothing is budgeted -- see vh_tick.
///
/// Its own `AutoRTFM::Transact`, nested inside whatever transaction the emitting call is already
/// in. Without that a raise in the resumed task would abort the *emitter's* transaction and drop
/// writes that had nothing to do with it; with it, "a failure undoes the failing computation's
/// writes" stays literally true for a task as well as for a call.
AUTORTFM_DISABLE int32 DeliverToAwaiter(int64 Token, const vh_value* Args, int32 ArgCount)
{
    const FAwaiter* const Found = GAwaiters.Find(Token);
    if (!Found)
    {
        // The wait ended between Godot queueing the emission and delivering it. Not an error: a
        // cancelled task is exactly a wait that stopped waiting.
        return VH_OK;
    }
    UObject* const Waiter = Found->Waiter.Get();
    const int64 SignalId = Found->SignalId;
    const FPayloadShape* const Shape = SignalId != 0
        ? (GSignalBindings.Contains(SignalId) ? &GSignalBindings[SignalId].Payload : nullptr)
        : nullptr;
    if (!Waiter || (SignalId != 0 && !Shape))
    {
        return VH_ERR_NOT_FOUND;
    }

    int32 Status = VH_OK;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    const AutoRTFM::ETransactionResult TransactionResult = AutoRTFM::Transact([&] {
        AutoRTFM::Open([&] {
            EnterVerse(Context, [&] {
                verse::event* const Event = FindEventField(Context, Waiter);
                if (!Event)
                {
                    Status = VH_ERR_NOT_FOUND;
                    return;
                }
                Verse::VValue Payload;
                const bool bBuilt = Shape ? PayloadValue(Context, *Shape, Args, ArgCount, Payload)
                                          : ArgumentArrayValue(Context, Args, ArgCount, Payload);
                if (!bBuilt)
                {
                    Status = VH_ERR_ARGUMENT;
                    return;
                }
                // event::Signal resumes the suspended awaits in FIFO order, under each task's own
                // content scope, skipping any whose scope was terminated -- Epic's code, and the
                // reason `Await` needed no scheduler of its own.
                Event->Signal(FVerseValue(Payload));
            });
        });
    });
    if (TransactionResult != AutoRTFM::ETransactionResult::Committed)
    {
        return VH_ERR_RUNTIME;
    }
    return Status;
}

/// Signals an `@export_signal` member's event with an emission Godot just delivered.
///
/// The permanent-connection counterpart of DeliverToAwaiter, and deliberately the same shape: the
/// payload is rebuilt against the same recorded `FPayloadShape` the descriptor was generated from,
/// so a struct payload comes back a struct and a tuple comes back a tuple whichever spelling
/// declared it. What differs is only where the event comes from -- the binding holds it, rather than
/// it being found by walking a `signal` object's shape.
///
/// Every emission arrives here, including the script's own `Emit`: the emit verb goes out to Godot
/// and Godot dispatches back, which is what makes a Verse handler and a GDScript handler see the
/// same ordering.
AUTORTFM_DISABLE int32 DeliverToEvent(int64 SignalId, const vh_value* Args, int32 ArgCount)
{
    const FSignalBinding* const Binding = GSignalBindings.Find(SignalId);
    if (!Binding)
    {
        return VH_ERR_NOT_FOUND;
    }
    UObject* const Held = Binding->Event.Get();
    if (!Held)
    {
        // The instance was released between Godot queueing the emission and delivering it, which
        // is the event-member analogue of a wait that stopped waiting.
        return VH_OK;
    }
    const FPayloadShape Shape = Binding->Payload;

    int32 Status = VH_OK;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    const AutoRTFM::ETransactionResult TransactionResult = AutoRTFM::Transact([&] {
        AutoRTFM::Open([&] {
            EnterVerse(Context, [&] {
                verse::event* const Event = Cast<verse::event>(Held);
                if (!Event)
                {
                    Status = VH_ERR_NOT_FOUND;
                    return;
                }
                Verse::VValue Payload;
                if (!PayloadValue(Context, Shape, Args, ArgCount, Payload))
                {
                    Status = VH_ERR_ARGUMENT;
                    return;
                }
                Event->Signal(FVerseValue(Payload));
            });
        });
    });
    if (TransactionResult != AutoRTFM::ETransactionResult::Committed)
    {
        return VH_ERR_RUNTIME;
    }
    return Status;
}

/// Mints the Callable an await or a foreign subscription is delivered through, and connects it.
///
/// Answers the callback id, with OutCallableRef holding the reference Godot keeps. 0 for a
/// connection Godot refused, having released whatever it had minted.
AUTORTFM_DISABLE int64 ConnectDelivery(int64 OwnerHandle,
                                       const FUtf8String& Name,
                                       FCallbackTarget Target,
                                       int32 ConnectFlags,
                                       int64& OutCallableRef)
{
    OutCallableRef = 0;
    GodotVerse::FHostState& Host = GodotVerse::GetHost();
    if (!Host.Godot.MakeCallable || !Host.Godot.ConnectSignal || !Host.Godot.ReleaseRef || OwnerHandle == 0)
    {
        return 0;
    }

    const int64 CallbackId = GNextCallbackId++;
    {
        FScopeLock Lock(&GCallbacksLock);
        GCallbacks.Add(CallbackId, MoveTemp(Target));
    }
    const int64 CallableRef = Host.Godot.MakeCallable(Host.Godot.Ctx, CallbackId, OwnerHandle);
    if (CallableRef == 0)
    {
        FScopeLock Lock(&GCallbacksLock);
        GCallbacks.Remove(CallbackId);
        return 0;
    }

    vh_value Callable{};
    Callable.Type = VH_TYPE_REF;
    Callable.VariantTag = VH_VARIANT_CALLABLE;
    Callable.Ref = CallableRef;
    const int32 Status = Host.Godot.ConnectSignal(Host.Godot.Ctx,
                                                  OwnerHandle,
                                                  reinterpret_cast<const char*>(*Name),
                                                  Name.Len(),
                                                  &Callable,
                                                  ConnectFlags);
    if (Status != VH_CALL_OK)
    {
        Host.Godot.ReleaseRef(Host.Godot.Ctx, CallableRef);
        FScopeLock Lock(&GCallbacksLock);
        GCallbacks.Remove(CallbackId);
        return 0;
    }
    OutCallableRef = CallableRef;
    return CallbackId;
}

/// The object and signal name a Godot Signal *value* stands for. False for a reference that is not
/// a Signal, or for a consumer built before v6.0 declared the callback.
AUTORTFM_DISABLE bool ResolveSignalRef(int64 Ref, int64& OutHandle, FUtf8String& OutName)
{
    GodotVerse::FHostState& Host = GodotVerse::GetHost();
    const char* NameUtf8 = nullptr;
    if (!Host.Godot.SignalTarget
        || Host.Godot.SignalTarget(Host.Godot.Ctx, Ref, &OutHandle, &NameUtf8) != VH_CALL_OK
        || !NameUtf8)
    {
        return false;
    }
    // Copied now: the consumer owns those bytes only until its next call, and the name outlives
    // this in an awaiter row.
    OutName = FUtf8String(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(NameUtf8)));
    return !OutName.IsEmpty();
}

/// Defined below, beside MakeCallableFor, which is its only other caller.
AUTORTFM_DISABLE bool DescribeBoundFunctionFwd(Verse::VFunction* Function, int64& OutHandle, FUtf8String& OutDecorated);

/// Registers one wait and connects what feeds it. OwnerHandle/Name say what to connect to.
AUTORTFM_DISABLE int64 BeginAwait(UObject* Waiter, int64 SignalId, int64 OwnerHandle, const FUtf8String& Name)
{
    if (!Waiter || OwnerHandle == 0 || Name.IsEmpty())
    {
        return 0;
    }
    const int64 Token = GNextAwaitToken++;

    FCallbackTarget Target;
    Target.OwnerHandle = OwnerHandle;
    Target.AwaitToken = Token;

    int64 CallableRef = 0;
    // One-shot: a single `Await()` resumes once, so Godot dropping the connection as it fires is
    // exactly right and saves the disconnect. `loop { X.Await() }` reconnects per iteration, which
    // is what the source says it does.
    const int64 CallbackId = ConnectDelivery(OwnerHandle, Name, MoveTemp(Target), VH_CONNECT_ONE_SHOT, CallableRef);
    if (CallbackId == 0)
    {
        return 0;
    }

    FAwaiter Awaiter;
    Awaiter.Waiter = TStrongObjectPtr<UObject>(Waiter);
    Awaiter.SignalId = SignalId;
    Awaiter.OwnerHandle = OwnerHandle;
    Awaiter.Name = Name;
    Awaiter.CallableRef = CallableRef;
    Awaiter.CallbackId = CallbackId;

    // The active scope is the awaiting task's own: an InstanceCall pushed the instance's before the
    // spawn, and a resumption pushes the task's again (TVerseCall::Return does it itself). So the
    // wait is anchored to exactly the thing whose death should end it.
    if (verse::FContentScopeGuard::IsActive())
    {
        const TSharedRef<verse::FContentScope>& Scope = verse::FContentScopeGuard::GetActiveScope();
        Awaiter.Scope = Scope;
        Awaiter.Cleanup = Scope->OnContentScopeCleanup.AddLambda(
            [Token](bool) { GodotVerse::EndSignalAwait(Token); });
    }

    GAwaiters.Add(Token, MoveTemp(Awaiter));
    return Token;
}
}

AUTORTFM_DISABLE int64 GodotVerse::BeginSignalAwait(UObject* Signal)
{
    verse::vh_signal* const Shadow = Cast<verse::vh_signal>(Signal);
    if (!Shadow)
    {
        return 0;
    }
    const int64 SignalId = Shadow->Id.Get();
    const FSignalBinding* const Binding = GSignalBindings.Find(SignalId);
    if (!Binding)
    {
        ReportVerseDiag(verse_diag::VG2109);
        return 0;
    }
    if (Binding->Reject != VH_SIGNAL_OK)
    {
        const std::string Reason = SignalRejectReason(Binding->Name, Binding->Reject, Binding->RejectDetail);
        ReportVerseDiag(verse_diag::VG2103, {{"signal", DiagArg(Binding->Name)}, {"reason", Reason}});
        return 0;
    }
    return BeginAwait(Signal, SignalId, Binding->OwnerHandle, Binding->Name);
}

AUTORTFM_DISABLE int64 GodotVerse::BeginSignalRefAwait(int64 Ref, UObject* Waiter)
{
    int64 OwnerHandle = 0;
    FUtf8String Name;
    if (!ResolveSignalRef(Ref, OwnerHandle, Name))
    {
        ReportVerseDiag(verse_diag::VG2110);
        return 0;
    }
    return BeginAwait(Waiter, 0, OwnerHandle, Name);
}

AUTORTFM_DISABLE void GodotVerse::EndSignalAwait(int64 Token)
{
    const FAwaiter* const Found = GAwaiters.Find(Token);
    if (!Found)
    {
        // Idempotent. A one-shot connection has already gone by the time a resumed wait ends, and
        // a `defer` that runs twice -- once on cancel, once on scope teardown -- must not say so.
        return;
    }
    const FAwaiter Awaiter = *Found;
    GAwaiters.Remove(Token);

    // Removed before anything else, so a wait that ended normally does not leave the scope holding
    // a lambda for the rest of its life. Harmless if this *is* the cleanup running -- the broadcast
    // clears its own list, and removing a handle that is already gone does nothing.
    if (const TSharedPtr<verse::FContentScope> Scope = Awaiter.Scope.Pin(); Scope.IsValid() && Awaiter.Cleanup.IsValid())
    {
        Scope->OnContentScopeCleanup.Remove(Awaiter.Cleanup);
    }

    {
        FScopeLock Lock(&GCallbacksLock);
        GCallbacks.Remove(Awaiter.CallbackId);
    }

    GodotVerse::FHostState& Host = GodotVerse::GetHost();
    if (Host.Godot.DisconnectSignal)
    {
        vh_value Callable{};
        Callable.Type = VH_TYPE_REF;
        Callable.VariantTag = VH_VARIANT_CALLABLE;
        Callable.Ref = Awaiter.CallableRef;
        // Refuses quietly for the one-shot Godot has already dropped, which is the resumed case.
        Host.Godot.DisconnectSignal(Host.Godot.Ctx,
                                    Awaiter.OwnerHandle,
                                    reinterpret_cast<const char*>(*Awaiter.Name),
                                    Awaiter.Name.Len(),
                                    &Callable);
    }
    if (Host.Godot.ReleaseRef)
    {
        Host.Godot.ReleaseRef(Host.Godot.Ctx, Awaiter.CallableRef);
    }
}

AUTORTFM_DISABLE int64 GodotVerse::SubscribeSignalRef(int64 Ref, const FVerseValue& Callback)
{
    int64 OwnerHandle = 0;
    FUtf8String Name;
    if (!ResolveSignalRef(Ref, OwnerHandle, Name))
    {
        ReportVerseDiag(verse_diag::VG2111);
        return 0;
    }

    int64 OwnerOfCallback = 0;
    FUtf8String Decorated;
    if (!DescribeBoundFunctionFwd(Callback.GetValue().DynamicCast<Verse::VFunction>(), OwnerOfCallback, Decorated))
    {
        ReportVerseDiag(verse_diag::VG2112);
        return 0;
    }

    FCallbackTarget Target;
    Target.OwnerHandle = OwnerOfCallback;
    Target.DecoratedName = Decorated;
    // Nothing declares a foreign signal's payload, so the handler takes the arguments as one Godot
    // Array and the packing happens on the way in.
    Target.bArgsAsArray = true;

    int64 CallableRef = 0;
    const int64 CallbackId = ConnectDelivery(OwnerHandle, Name, MoveTemp(Target), 0, CallableRef);
    if (CallbackId == 0)
    {
        return 0;
    }

    const int64 Id = GNextSubscriptionId++;
    GSubscriptions.Add(Id, FSubscription{OwnerHandle, Name, CallableRef});

    // Compensated exactly as signal.Subscribe is, and for the same reason: this mutates Godot
    // and answers a value, so it can be neither deferred to commit nor ignored. It is what closes
    // `Object.Connect`'s rollback gap -- that one stays an unforgiving direct call, and this is the
    // spelling a script reaches first.
    AutoRTFM::OnAbort<AutoRTFM::EOpenBehavior::SameAsClosed>(
        [Id] { GodotVerse::CancelSubscription(Id); });
    return Id;
}

namespace {

/// The `ToString` extension method the project wrote for this class, as a decorated name.
///
/// R-NODE-10's first hook, and it is looked for in the class's *enclosing scope* rather than among
/// the class's own definitions because that is where Verse puts it. `(X:my_class).ToString()` is an
/// **extension method**: a module-level `operator'.ToString'(:my_class, :tuple())`, the receiver
/// first and the call's own arguments as a tuple second. A class member of that name cannot be
/// written at all -- glitch 3532 against /Verse.org/Verse's own ToString, which is reachable as an
/// extension method itself -- so no class method list will ever carry one and GetClassMethodsLive
/// is the wrong place to look. `tests/verse_probe/tostring_probe.verse` is the record.
///
/// The receiver is matched against the class *and its bases*, so a ToString written once for a base
/// class serves every script deriving from it, which is what anything method-shaped should do.
///
/// Empty when the project declares none, which is the common case: Godot then keeps `<Node2D#27>`.
AUTORTFM_DISABLE FUtf8String FindToStringExtensionLive(FUtf8StringView ClassName)
{
    const uLang::CClass* const Class = FindScriptClassLive(ClassName);
    if (!Class)
    {
        return FUtf8String();
    }

    const uLang::CLogicalScope& Scope = Class->_EnclosingScope.GetLogicalScope();
    for (const uLang::TSRef<uLang::CFunction>& Function : Scope.GetDefinitionsOfKind<uLang::CFunction>())
    {
        if (!FUtf8StringView(Function->AsNameCString()).Equals(UTF8TEXTVIEW("operator'.ToString'")))
        {
            continue;
        }

        // The receiver is parameter 0; the call's own arguments are the tuple in parameter 1.
        const uLang::SSignature::ParamDefinitions& Params = Function->_Signature.GetParams();
        if (Params.IsEmpty() || !Params[0])
        {
            continue;
        }
        const uLang::CDataDefinition* const Receiver = Params[0];
        const uLang::CTypeBase* const ParamType = Receiver->GetType();
        if (!ParamType)
        {
            continue;
        }
        const uLang::CClass* const ReceiverClass = ParamType->GetNormalType().AsNullable<uLang::CClass>();
        if (!ReceiverClass)
        {
            continue;
        }
        for (const uLang::CClass* Cursor = Class; Cursor != nullptr; Cursor = Cursor->GetSuperClass())
        {
            if (Cursor == ReceiverClass)
            {
                return ExtensionMethodDecoratedName(*Function);
            }
        }
    }
    return FUtf8String();
}

} // namespace

namespace {

/// Every class the script package declares, module-qualified the way every ClassNameUtf8 in the ABI
/// is: `player` at the root, `gameplay/player` under a `.vmodule` marker.
///
/// Modules are recursed and classes are not. A class nested inside a class resolves by verse path
/// and so used to be describable, but nothing addresses one -- only the class named after its file
/// can go on a node -- and recursing would harvest the archetype the compiler generates per class
/// along with it.
AUTORTFM_DISABLE void CollectSnapshotClassNames(const uLang::CModule& Module,
                                                const FUtf8String& Path,
                                                TArray<FUtf8String>& Out)
{
    for (const uLang::TSRef<uLang::CClass>& Class : Module.GetDefinitionsOfKind<uLang::CClass>())
    {
        const FUtf8String Name = FUtf8String(Class->AsNameCString());
        Out.Add(Path.IsEmpty() ? Name : Path + UTF8TEXT("/") + Name);
    }

    for (const uLang::TSRef<uLang::CModule>& Submodule : Module.GetDefinitionsOfKind<uLang::CModule>())
    {
        const FUtf8String Name = FUtf8String(Submodule->AsNameCString());
        CollectSnapshotClassNames(*Submodule, Path.IsEmpty() ? Name : Path + UTF8TEXT("/") + Name, Out);
    }
}


} // namespace

namespace {

// --- the JSON round trip ----------------------------------------------------------------------
//
// One shape, written and read by two functions that have to be changed together. Every field that
// survives is one a *runtime* lookup reads: the uLang pointers (FMemberType::ReferenceClass and
// ::Member, FPayloadShape::StructClass) are the analysis's own and are deliberately dropped --
// nothing outside an analysis reads them, which is what made carrying the rest possible at all.

AUTORTFM_DISABLE TSharedPtr<FJsonObject> WriteMemberType(const FMemberType& Type);
AUTORTFM_DISABLE FMemberType ReadMemberType(const TSharedPtr<FJsonObject>& Object);

/// The kind a description read back out of a sidecar is, recovered from the fields it carries.
///
/// The sidecar has no field for the kind -- its format is shared with the interpreter -- and does
/// not need one: DescribeType fills a different combination of fields for every kind but one, and
/// the order of the tests is the order those combinations overlap in. The one is TypedContainer,
/// which describes exactly as Other does and reads back as Other (RecordsAs).
AUTORTFM_DISABLE EDeclaredKind RecordedKind(const FMemberType& Type)
{
    const GodotVerse::FExportDesc& Described = Type.Described;
    if (Described.Reject == VH_EXPORT_OPTION_NOT_OBJECT)
    {
        return EDeclaredKind::Option;
    }
    if (Described.VariantTag == VH_VARIANT_OBJECT)
    {
        return EDeclaredKind::Reference;
    }
    if (!Type.ReferenceName.IsEmpty())
    {
        return EDeclaredKind::OtherClass;
    }
    if (Type.UserStruct.IsValid())
    {
        return EDeclaredKind::UserStruct;
    }
    if (!Type.StructName.IsEmpty())
    {
        return EDeclaredKind::MathStruct;
    }
    if (Described.Hint == VH_EXPORT_HINT_ENUM)
    {
        return EDeclaredKind::Enum;
    }

    VH_EXHAUSTIVE_SWITCH_BEGIN
    switch (Described.Type)
    {
    case VH_TYPE_VARIANT:
        return EDeclaredKind::Variant;
    case VH_TYPE_REF:
        return EDeclaredKind::Container;
    case VH_TYPE_INT:
        return Described.VariantTag == VH_VARIANT_RID ? EDeclaredKind::Rid : EDeclaredKind::Int;
    case VH_TYPE_LOGIC:
        return EDeclaredKind::Logic;
    case VH_TYPE_FLOAT:
        return EDeclaredKind::Float;
    case VH_TYPE_CHAR:
        return EDeclaredKind::Char;
    case VH_TYPE_STRING:
        return EDeclaredKind::String;
    case VH_TYPE_ARRAY:
        return EDeclaredKind::Array;
    case VH_TYPE_MAP:
        return EDeclaredKind::Map;
    case VH_TYPE_TUPLE:
        return EDeclaredKind::Tuple;
    case VH_TYPE_VOID:
    case VH_TYPE_OPTION:
        return EDeclaredKind::Other;
    }
    VH_EXHAUSTIVE_SWITCH_END
    return EDeclaredKind::Other;
}

/// `refOrigin` as the sidecar spells it, which the interpreter reads too and which has no Binding:
/// one is recorded as Other, which is what it was before it had a name, and DeclaredReferenceClass
/// resolves the two alike.
AUTORTFM_DISABLE int32 SidecarOriginCode(EClassOrigin Origin)
{
    VH_EXHAUSTIVE_SWITCH_BEGIN
    switch (Origin)
    {
    case EClassOrigin::Other:
    case EClassOrigin::Binding:
        return (int32)EClassOrigin::Other;
    case EClassOrigin::Mirrored:
    case EClassOrigin::Script:
        return (int32)Origin;
    }
    VH_EXHAUSTIVE_SWITCH_END
    return (int32)EClassOrigin::Other;
}

/// What RecordedKind answers for a description of Kind, so the cook can check the round trip.
AUTORTFM_DISABLE EDeclaredKind RecordsAs(EDeclaredKind Kind)
{
    VH_EXHAUSTIVE_SWITCH_BEGIN
    switch (Kind)
    {
    // Every converter treats the two alike, so nothing is lost.
    case EDeclaredKind::TypedContainer:
        return EDeclaredKind::Other;
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
    case EDeclaredKind::Option:
    case EDeclaredKind::Variant:
    case EDeclaredKind::Rid:
    case EDeclaredKind::MathStruct:
    case EDeclaredKind::UserStruct:
    case EDeclaredKind::Container:
    case EDeclaredKind::Reference:
    case EDeclaredKind::OtherClass:
        return Kind;
    }
    VH_EXHAUSTIVE_SWITCH_END
    return Kind;
}

AUTORTFM_DISABLE TSharedPtr<FJsonObject> WriteMemberType(const FMemberType& Type)
{
    ensureMsgf(RecordedKind(Type) == RecordsAs(Type.Kind),
               TEXT("A declared type of kind %d reads back out of the sidecar as kind %d."),
               (int32)Type.Kind, (int32)RecordedKind(Type));

    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetObjectField(TEXT("described"), GodotVerse::WriteExportDesc(Type.Described));
    if (Type.bIsVar)
    {
        Object->SetBoolField(TEXT("var"), true);
    }
    if (!Type.ReferenceName.IsEmpty())
    {
        Object->SetStringField(TEXT("ref"), FString(Type.ReferenceName));
        Object->SetStringField(TEXT("refPath"), FString(Type.ReferenceQualifiedName));
        Object->SetNumberField(TEXT("refOrigin"), SidecarOriginCode(Type.ReferenceOrigin));
        Object->SetBoolField(TEXT("refOption"), Type.bReferenceIsOption);
    }
    if (!Type.StructName.IsEmpty())
    {
        Object->SetStringField(TEXT("struct"), FString(Type.StructName));
    }
    if (Type.EnumeratorCount > 0)
    {
        Object->SetNumberField(TEXT("enumerators"), Type.EnumeratorCount);
        Object->SetStringField(TEXT("enum"), FString(Type.EnumerationName));
    }
    if (Type.UserStruct.IsValid())
    {
        TSharedPtr<FJsonObject> Layout = MakeShared<FJsonObject>();
        Layout->SetStringField(TEXT("name"), FString(Type.UserStruct->DecoratedName));
        TArray<TSharedPtr<FJsonValue>> Names;
        TArray<TSharedPtr<FJsonValue>> Keys;
        TArray<TSharedPtr<FJsonValue>> Types;
        for (const FUtf8String& Name : Type.UserStruct->FieldNames)
        {
            Names.Add(MakeShared<FJsonValueString>(FString(Name)));
        }
        for (const FUtf8String& Key : Type.UserStruct->FieldKeys)
        {
            Keys.Add(MakeShared<FJsonValueString>(FString(Key)));
        }
        for (const FMemberType& Field : Type.UserStruct->FieldTypes)
        {
            Types.Add(MakeShared<FJsonValueObject>(WriteMemberType(Field)));
        }
        Layout->SetArrayField(TEXT("fieldNames"), Names);
        Layout->SetArrayField(TEXT("fieldKeys"), Keys);
        Layout->SetArrayField(TEXT("fieldTypes"), Types);
        Object->SetObjectField(TEXT("userStruct"), Layout);
    }
    return Object;
}

AUTORTFM_DISABLE FMemberType ReadMemberType(const TSharedPtr<FJsonObject>& Object)
{
    FMemberType Type;
    if (!Object.IsValid())
    {
        return Type;
    }

    const TSharedPtr<FJsonObject>* Described = nullptr;
    if (Object->TryGetObjectField(TEXT("described"), Described))
    {
        Type.Described = GodotVerse::ReadExportDesc(*Described);
    }
    Object->TryGetBoolField(TEXT("var"), Type.bIsVar);

    FString Text;
    if (Object->TryGetStringField(TEXT("ref"), Text))
    {
        Type.ReferenceName = FUtf8String(Text);
        Type.ReferenceQualifiedName = FUtf8String(Object->GetStringField(TEXT("refPath")));
        Type.ReferenceOrigin = (EClassOrigin)(int32)Object->GetNumberField(TEXT("refOrigin"));
        Type.bReferenceIsOption = Object->GetBoolField(TEXT("refOption"));
    }
    if (Object->TryGetStringField(TEXT("struct"), Text))
    {
        Type.StructName = FUtf8String(Text);
        // The layout itself is a generated table every host links, so it is found again rather
        // than carried: what the sidecar has to remember is only which one.
        Type.Struct = FindStructLayout(FUtf8StringView(Type.StructName));
    }
    if (Object->TryGetStringField(TEXT("enum"), Text))
    {
        Type.EnumerationName = FUtf8String(Text);
        Type.EnumeratorCount = (int32)Object->GetNumberField(TEXT("enumerators"));
    }

    const TSharedPtr<FJsonObject>* Layout = nullptr;
    if (Object->TryGetObjectField(TEXT("userStruct"), Layout))
    {
        Type.UserStruct = MakeShared<FUserStructLayout>();
        Type.UserStruct->DecoratedName = FUtf8String((*Layout)->GetStringField(TEXT("name")));
        const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        if ((*Layout)->TryGetArrayField(TEXT("fieldNames"), Items))
        {
            for (const TSharedPtr<FJsonValue>& Item : *Items)
            {
                Type.UserStruct->FieldNames.Add(FUtf8String(Item->AsString()));
            }
        }
        if ((*Layout)->TryGetArrayField(TEXT("fieldKeys"), Items))
        {
            for (const TSharedPtr<FJsonValue>& Item : *Items)
            {
                Type.UserStruct->FieldKeys.Add(FUtf8String(Item->AsString()));
            }
        }
        if ((*Layout)->TryGetArrayField(TEXT("fieldTypes"), Items))
        {
            for (const TSharedPtr<FJsonValue>& Item : *Items)
            {
                Type.UserStruct->FieldTypes.Add(ReadMemberType(Item->AsObject()));
            }
        }
    }
    Type.Kind = RecordedKind(Type);
    return Type;
}

AUTORTFM_DISABLE TSharedPtr<FJsonObject> WritePayloadShape(const FPayloadShape& Shape)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("kind"), (int32)Shape.Kind);
    Object->SetNumberField(TEXT("reject"), Shape.Reject);
    Object->SetStringField(TEXT("rejectDetail"), FString(Shape.RejectDetail));
    Object->SetObjectField(TEXT("whole"), WriteMemberType(Shape.Whole));
    TArray<TSharedPtr<FJsonValue>> Args;
    for (const FPayloadArg& Arg : Shape.Args)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), FString(Arg.Name));
        Entry->SetStringField(TEXT("key"), FString(Arg.FieldKey));
        Entry->SetObjectField(TEXT("type"), WriteMemberType(Arg.Type));
        Args.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Object->SetArrayField(TEXT("args"), Args);
    return Object;
}

AUTORTFM_DISABLE FPayloadShape ReadPayloadShape(const TSharedPtr<FJsonObject>& Object)
{
    FPayloadShape Shape;
    if (!Object.IsValid())
    {
        return Shape;
    }
    Shape.Kind = (EPayloadShape)(uint8)(int32)Object->GetNumberField(TEXT("kind"));
    Shape.Reject = (int32)Object->GetNumberField(TEXT("reject"));
    Shape.RejectDetail = FUtf8String(Object->GetStringField(TEXT("rejectDetail")));
    const TSharedPtr<FJsonObject>* Whole = nullptr;
    if (Object->TryGetObjectField(TEXT("whole"), Whole))
    {
        Shape.Whole = ReadMemberType(*Whole);
    }
    const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
    if (Object->TryGetArrayField(TEXT("args"), Items))
    {
        for (const TSharedPtr<FJsonValue>& Item : *Items)
        {
            const TSharedPtr<FJsonObject> Entry = Item->AsObject();
            FPayloadArg& Arg = Shape.Args.AddDefaulted_GetRef();
            Arg.Name = FUtf8String(Entry->GetStringField(TEXT("name")));
            Arg.FieldKey = FUtf8String(Entry->GetStringField(TEXT("key")));
            const TSharedPtr<FJsonObject>* Type = nullptr;
            if (Entry->TryGetObjectField(TEXT("type"), Type))
            {
                Arg.Type = ReadMemberType(*Type);
            }
        }
    }
    return Shape;
}

} // namespace

AUTORTFM_DISABLE TSharedPtr<FJsonObject> GodotVerse::WriteDeclaredTypes(const FDeclaredTypes& Types)
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    TSharedRef<FJsonObject> Members = MakeShared<FJsonObject>();
    for (const TPair<FUtf8String, FMemberType>& Pair : Types.Members)
    {
        Members->SetObjectField(FString(Pair.Key), WriteMemberType(Pair.Value));
    }
    Root->SetObjectField(TEXT("members"), Members);

    TSharedRef<FJsonObject> Methods = MakeShared<FJsonObject>();
    for (const TPair<FUtf8String, FMethodSignatureTypes>& Pair : Types.Methods)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Params;
        for (const FMemberType& Param : Pair.Value.Params)
        {
            Params.Add(MakeShared<FJsonValueObject>(WriteMemberType(Param)));
        }
        Entry->SetArrayField(TEXT("params"), Params);
        Entry->SetObjectField(TEXT("result"), WriteMemberType(Pair.Value.Result));
        Methods->SetObjectField(FString(Pair.Key), Entry);
    }
    Root->SetObjectField(TEXT("methods"), Methods);

    TSharedRef<FJsonObject> Signals = MakeShared<FJsonObject>();
    for (const TPair<FUtf8String, FPayloadShape>& Pair : Types.Signals)
    {
        Signals->SetObjectField(FString(Pair.Key), WritePayloadShape(Pair.Value));
    }
    Root->SetObjectField(TEXT("signals"), Signals);

    return Root;
}

AUTORTFM_DISABLE TSharedPtr<GodotVerse::FEngineSignalTypes> GodotVerse::CollectEngineSignalTypes()
{
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager =
        GIde.IsValid() ? GIde->GetBuildManager() : nullptr;
    if (!BuildManager.IsValid())
    {
        return nullptr;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;
    const uLang::CModule* const Mirror = Program->FindDefinitionByVersePath<uLang::CModule>(GodotVersePath);
    if (!Mirror)
    {
        return nullptr;
    }

    TSharedPtr<FEngineSignalTypes> Types = MakeShared<FEngineSignalTypes>();
    for (const uLang::TSRef<uLang::CClass>& Class : Mirror->GetDefinitionsOfKind<uLang::CClass>())
    {
        const FUtf8String ClassName(Class->AsNameCString());
        for (const uLang::TSRef<uLang::CFunction>& Function : Class->GetDefinitionsOfKind<uLang::CFunction>())
        {
            const uLang::CFunctionType* const Type = Function->_Signature.GetFunctionType();
            bool bIsOption = false;
            const uLang::CNormalType* const Returned =
                Type ? &UnwrapDeclaredType(Type->GetReturnType(), bIsOption) : nullptr;
            const uLang::CClass* const Signal = Returned ? Returned->AsNullable<uLang::CClass>() : nullptr;
            if (!Signal || !IsSignalClass(*Signal))
            {
                continue;
            }
            FPayloadShape Shape;
            DescribePayload(SignalPayloadType(*Signal), *Program, Shape);
            Types->Shapes.Add(ClassName + UTF8TEXT(".") + FUtf8String(Function->AsNameCString()),
                              MoveTemp(Shape));
        }
    }
    return Types;
}

AUTORTFM_DISABLE TSharedPtr<FJsonObject> GodotVerse::WriteEngineSignalTypes(const FEngineSignalTypes& Types)
{
    // Deduplicated, because 503 accessors carry about eighty distinct payloads between them --
    // half of Godot's signals are `signal(tuple())` -- and the identity that separates them is the
    // JSON itself. Writing one entry per accessor instead costs a third of a megabyte of sidecar
    // for the same information.
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Shapes;
    TMap<FString, int32> IndexByText;
    TSharedRef<FJsonObject> Keys = MakeShared<FJsonObject>();

    for (const TPair<FUtf8String, FPayloadShape>& Pair : Types.Shapes)
    {
        const TSharedPtr<FJsonObject> Shape = WritePayloadShape(Pair.Value);
        FString Text;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
        FJsonSerializer::Serialize(Shape.ToSharedRef(), Writer);

        int32* Existing = IndexByText.Find(Text);
        if (!Existing)
        {
            Existing = &IndexByText.Add(Text, Shapes.Num());
            Shapes.Add(MakeShared<FJsonValueObject>(Shape));
        }
        Keys->SetNumberField(FString(Pair.Key), *Existing);
    }

    Root->SetArrayField(TEXT("shapes"), Shapes);
    Root->SetObjectField(TEXT("keys"), Keys);
    return Root;
}

AUTORTFM_DISABLE TSharedPtr<GodotVerse::FEngineSignalTypes> GodotVerse::ReadEngineSignalTypes(
    const TSharedPtr<FJsonObject>& Object)
{
    const TArray<TSharedPtr<FJsonValue>>* ShapeArray = nullptr;
    const TSharedPtr<FJsonObject>* Keys = nullptr;
    if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("shapes"), ShapeArray)
        || !Object->TryGetObjectField(TEXT("keys"), Keys))
    {
        return nullptr;
    }

    TArray<FPayloadShape> Shapes;
    Shapes.Reserve(ShapeArray->Num());
    for (const TSharedPtr<FJsonValue>& Item : *ShapeArray)
    {
        Shapes.Add(ReadPayloadShape(Item->AsObject()));
    }

    TSharedPtr<FEngineSignalTypes> Types = MakeShared<FEngineSignalTypes>();
    for (const auto& Pair : (*Keys)->Values)
    {
        const int32 Index = (int32)Pair.Value->AsNumber();
        if (Shapes.IsValidIndex(Index))
        {
            Types->Shapes.Add(FUtf8String(Pair.Key), Shapes[Index]);
        }
    }
    return Types;
}

AUTORTFM_DISABLE void GodotVerse::SetRecordedEngineSignalTypes(TSharedPtr<FEngineSignalTypes> Types)
{
    GRecordedEngineSignals = MoveTemp(Types);
}

AUTORTFM_DISABLE TSharedPtr<GodotVerse::FDeclaredTypes> GodotVerse::ReadDeclaredTypes(
    const TSharedPtr<FJsonObject>& Object)
{
    if (!Object.IsValid())
    {
        return nullptr;
    }
    TSharedPtr<FDeclaredTypes> Types = MakeShared<FDeclaredTypes>();

    const TSharedPtr<FJsonObject>* Section = nullptr;
    if (Object->TryGetObjectField(TEXT("members"), Section))
    {
        for (const auto& Pair : (*Section)->Values)
        {
            Types->Members.Add(FUtf8String(Pair.Key), ReadMemberType(Pair.Value->AsObject()));
        }
    }
    if (Object->TryGetObjectField(TEXT("methods"), Section))
    {
        for (const auto& Pair : (*Section)->Values)
        {
            const TSharedPtr<FJsonObject> Entry = Pair.Value->AsObject();
            FMethodSignatureTypes Signature;
            const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
            if (Entry->TryGetArrayField(TEXT("params"), Params))
            {
                for (const TSharedPtr<FJsonValue>& Param : *Params)
                {
                    Signature.Params.Add(ReadMemberType(Param->AsObject()));
                }
            }
            const TSharedPtr<FJsonObject>* Result = nullptr;
            if (Entry->TryGetObjectField(TEXT("result"), Result))
            {
                Signature.Result = ReadMemberType(*Result);
            }
            Types->Methods.Add(FUtf8String(Pair.Key), MoveTemp(Signature));
        }
    }
    if (Object->TryGetObjectField(TEXT("signals"), Section))
    {
        for (const auto& Pair : (*Section)->Values)
        {
            Types->Signals.Add(FUtf8String(Pair.Key), ReadPayloadShape(Pair.Value->AsObject()));
        }
    }
    return Types;
}

namespace {

/// The declared types of one class, recorded while a semantic program still exists.
///
/// Everything here is re-derived per call in an editor host, off the program the last analysis
/// built. A runtime host has no program and no way to make one, so this runs once per analysis and
/// the sidecar carries the answer into the exported game. The three tables are the three questions
/// the VM cannot answer for itself: what type is this member, what does this method take and
/// answer, and what does this signal's payload decompose into.
AUTORTFM_DISABLE void CollectDeclaredTypes(FUtf8StringView ClassName, GodotVerse::FDeclaredTypes& Out)
{
    if (!GIde.IsValid())
    {
        return;
    }
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager();
    if (!BuildManager.IsValid())
    {
        return;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;

    const FUtf8String ClassPath = FUtf8String(ScriptVersePath) + UTF8TEXT("/") + FUtf8String(ClassName);
    const uLang::CClass* const Class = Program->FindDefinitionByVersePath<uLang::CClass>(
        FULangConversionUtils::FUtf8StringViewToULangStringView(ClassPath));
    if (!Class)
    {
        return;
    }

    // The same chain DescribeMemberType walks, and stopping where it stops: above the script
    // package the members are Godot's own properties, which the mirror describes and this does not.
    // Derived class first, so a member a subclass redeclares wins the way a lookup from the
    // subclass would have found it.
    const TArray<const uLang::CClass*> Chain = ClassChainOfOrigin(*Class, *Program, EClassOrigin::Script);
    for (int32 Link = Chain.Num() - 1; Link >= 0; --Link)
    {
        for (const uLang::TSRef<uLang::CDataDefinition>& Member : Chain[Link]->GetDefinitionsOfKind<uLang::CDataDefinition>())
        {
            const FUtf8String Name(Member->AsNameCString());
            if (Out.Members.Contains(Name))
            {
                continue;
            }
            FMemberType Described = DescribeType(Member->GetType(), *Program);
            Described.bIsVar = Member->IsVar();
            // Both spellings, and unconditionally rather than only for a member carrying
            // `@export_signal`: this table is what a runtime host reads *instead of* a semantic
            // program, and recording a shape for a member that turns out not to be registered
            // costs a map entry, where missing one costs an emission that silently carries nothing.
            if (Described.ReferenceClass
                && (IsSignalClass(*Described.ReferenceClass) || IsEventClass(*Described.ReferenceClass)))
            {
                FPayloadShape Shape;
                DescribePayload(SignalPayloadType(*Described.ReferenceClass), *Program, Shape);
                Out.Signals.Add(Name, MoveTemp(Shape));
            }
            Out.Members.Add(Name, MoveTemp(Described));
        }
    }

    // The class's own functions, which is the set InstanceCall looks a method up in.
    for (const uLang::TSRef<uLang::CFunction>& Function : Class->GetDefinitionsOfKind<uLang::CFunction>())
    {
        const uLang::CFunctionType* const Type = Function->_Signature.GetFunctionType();
        if (!Type)
        {
            continue;
        }
        FMethodSignatureTypes Signature;
        for (const uLang::CDataDefinition* Param : Function->_Signature.GetParams())
        {
            Signature.Params.Add(Param ? DescribeType(Param->GetType(), *Program) : FMemberType{});
        }
        Signature.Result = DescribeType(&Type->GetReturnType(), *Program);
        Out.Methods.Add(FULangConversionUtils::ULangStrToFUtf8String(Function->GetDecoratedName()),
                        MoveTemp(Signature));
    }
}

AUTORTFM_DISABLE const GodotVerse::FDeclaredTypes* RecordedTypes(FUtf8StringView ClassName)
{
    if (!GSnapshot)
    {
        return nullptr;
    }
    const GodotVerse::FAnalysisSnapshot::FClass* const Entry = GSnapshot->Classes.Find(FUtf8String(ClassName));
    return Entry ? Entry->Types.Get() : nullptr;
}

AUTORTFM_DISABLE void TakeAnalysisSnapshot()
{
    const double Started = FPlatformTime::Seconds();

    TSharedRef<FAnalysisSnapshot> Snapshot = MakeShared<FAnalysisSnapshot>();
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager =
        GIde.IsValid() ? GIde->GetBuildManager() : nullptr;
    if (!BuildManager.IsValid())
    {
        // No program to describe. The empty snapshot still replaces whatever was current, because
        // the alternative is answering about a program that no longer exists.
        GPendingSnapshot = Snapshot;
        return;
    }

    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;

    TArray<FUtf8String> ClassNames;
    if (const uLang::CModule* const Root = Program->FindDefinitionByVersePath<uLang::CModule>(ScriptVersePath))
    {
        CollectSnapshotClassNames(*Root, FUtf8String(), ClassNames);
    }

    int32 Candidates = 0;
    double CandidateSeconds = 0.0;

    for (const FUtf8String& ClassName : ClassNames)
    {
        FAnalysisSnapshot::FClass& Entry = Snapshot->Classes.Add(ClassName);

        const uLang::CClass* const Class = FindScriptClassLive(ClassName);
        Entry.bAbstract = Class != nullptr && Class->IsAbstract();

        GetClassMethodsLive(ClassName, Entry.Methods);
        GetClassSignalsLive(ClassName, Entry.Signals);
        GetClassRpcsLive(ClassName, Entry.Rpcs);
        Entry.ToStringDecorated = FindToStringExtensionLive(ClassName);

        // The declared types, which are the analysis's to record and nothing else's to re-derive.
        Entry.Types = MakeShared<GodotVerse::FDeclaredTypes>();
        CollectDeclaredTypes(FUtf8StringView(ClassName), *Entry.Types);
        Entry.bExportsHarvested = GetClassExportsLive(ClassName, Entry.Exports);
        ClassMembersLive(ClassName, Entry.Members);

        // Timed apart from the rest: it describes the whole inherited surface of a mirrored Godot
        // class, which is a different order of work from the handful of names beside it.
        const double CandidatesStarted = FPlatformTime::Seconds();
        ClassOverrideCandidatesLive(ClassName, Entry.Members, Entry.OverrideCandidates);
        CandidateSeconds += FPlatformTime::Seconds() - CandidatesStarted;
        Candidates += Entry.OverrideCandidates.Num();
    }

    // The generated bindings, which the walk above does not reach: it is of the script package
    // alone, so `main_script{}.` opened an empty popup and filled only once the analysis for
    // that one keystroke had landed.
    //
    // Members alone. None of the describing work above applies to a binding -- it declares no
    // exports, no RPCs and no signals of its own, and nothing overrides one -- and
    // ClassOverrideCandidatesLive is the expensive half of that loop, which is why this is not
    // the same pass with a different module handed to it.
    if (const uLang::CModule* const BindingsModule =
            Program->FindDefinitionByVersePath<uLang::CModule>(BindingsVersePath))
    {
        for (const uLang::TSRef<uLang::CClass>& Binding : BindingsModule->GetDefinitionsOfKind<uLang::CClass>())
        {
            TArray<GodotVerse::FCompleteItem> Members;
            ClassOwnMembers(*Binding, Members);
            Snapshot->BindingMembers.Add(FUtf8String(Binding->AsNameCString()), MoveTemp(Members));
        }
    }

    if (Program->_AstProject)
    {
        Snapshot->bAstAvailable = true;
        for (const uLang::CAstCompilationUnit* CompilationUnit : Program->_AstProject->OrderedCompilationUnits())
        {
            for (const uLang::CAstPackage* Package : CompilationUnit->Packages())
            {
                // The project's own packages only. A name that needs an import from the Godot
                // mirror is a different question, and the mirror is one module the whole project
                // already imports.
                const bool bIsUserPackage = Package->_VerseScope == uLang::EVerseScope::PublicUser
                    || Package->_VerseScope == uLang::EVerseScope::InternalUser;
                if (!bIsUserPackage || !Package->_RootModule)
                {
                    continue;
                }
                if (const uLang::CModule* const Root = Package->_RootModule->GetModule())
                {
                    CollectModuleDeclarations(*Root, FUtf8String(), Snapshot->ModulesDeclaring);
                }
            }
        }
    }

    GPendingSnapshot = Snapshot;

    if (AnalysisTraceEnabled())
    {
        fprintf(stderr,
                "[vh-trace]   snapshot: %d class(es), %d name(s), %.2f ms semantic"
                " (%d override candidate(s), %.2f ms)\n",
                Snapshot->Classes.Num(),
                Snapshot->ModulesDeclaring.Num(),
                (FPlatformTime::Seconds() - Started) * 1000.0,
                Candidates,
                CandidateSeconds * 1000.0);
        fflush(stderr);
    }
}

AUTORTFM_DISABLE void PublishAnalysisSnapshot()
{
    if (!GPendingSnapshot)
    {
        return;
    }

    const double Started = FPlatformTime::Seconds();
    int32 Defaults = 0;
    int32 Constants = 0;

    for (TPair<FUtf8String, FAnalysisSnapshot::FClass>& Pair : GPendingSnapshot->Classes)
    {
        FAnalysisSnapshot::FClass& Entry = Pair.Value;

        // The published generation's view, which is not the analysis's: a class the buffer has
        // renamed is in one and not the other, and an inspector default is generated code and so
        // only ever comes from a build.
        Entry.bInPublishedProgram = FindGodotClass(Pair.Key) != nullptr;
        if (Entry.bInPublishedProgram && !Entry.Exports.IsEmpty())
        {
            // One transient instance for the whole class rather than one per member, which is what
            // makes reading every default cost about what reading one used to.
            TStrongObjectPtr<UObject> DefaultsObject(NewDefaultsObject(Pair.Key));
            for (const GodotVerse::FExportDesc& Export : Entry.Exports)
            {
                Entry.Defaults.Add(Export.Name, ReadDefaultFieldOf(DefaultsObject.Get(), Export.Name));
                ++Defaults;
            }
        }

        TSharedRef<GodotVerse::FClassStatics> Statics = MakeShared<GodotVerse::FClassStatics>();
        if (GetClassStaticsLive(Pair.Key, Statics->Statics, Statics->Values, Statics->Storage))
        {
            Constants += Statics->Statics.Num();
            Entry.Statics = Statics;
        }
    }

    GSnapshot = GPendingSnapshot;
    GPendingSnapshot.Reset();

    if (AnalysisTraceEnabled())
    {
        fprintf(stderr,
                "[vh-trace]   snapshot: %d default(s), %d static(s), %.2f ms vm\n",
                Defaults,
                Constants,
                (FPlatformTime::Seconds() - Started) * 1000.0);
        fflush(stderr);
    }
}

} // namespace

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
    TSharedRef<verse::FContentScope> Scope = verse::MakeContentScope(GScopeOuter);

    UObject* Instance = nullptr;
    {
        verse::FContentScopeGuard Guard(Scope);
        // The node Godot already made is this instance's peer, and the block clause on vh_object
        // adopts it rather than minting a second one -- which is the whole of docs/phase-4b-
        // design.md 4.3, and the failure that would not have announced itself.
        FAdoptPeerScope Adopting(NativeClass, Handle);
        // UVerseClass::PostInitInstance runs the Verse constructor from inside NewObject, so fields
        // are initialised by the time this returns.
        Instance = NewObject<UObject>(GetTransientPackage(), NativeClass);
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

/// Connects each `@export_signal` event member, once, at the first entry into the instance.
///
/// **Not at vh_instantiate, and the reason is Godot's own ordering.** The consumer builds the Verse
/// object *before* it installs the script instance on the node, and `Object::has_signal` answers
/// off the installed instance -- so a connect there is refused with "Attempt to connect nonexistent
/// signal", and the member would register, emit normally, and silently never deliver back. Nor at
/// the end of the consumer's create(): the object does not hold the script instance until
/// `_instance_create` has *returned* to Godot, which is later still and not a point this side can
/// name.
///
/// First entry is both late enough and early enough. It is late enough because a call into the
/// instance is Godot dispatching to an installed script instance; and it is early enough because a
/// Verse awaiter can only exist after Verse code has run on this object, and running Verse code on
/// it *is* an entry. The one ordering left uncovered -- Godot emits before any Verse code runs --
/// has nothing on the Verse side to deliver to.
AUTORTFM_DISABLE void EnsureEventConnections(GodotVerse::FInstance& Instance)
{
    for (const int64 Id : Instance.EventBindings)
    {
        FSignalBinding* const Binding = GSignalBindings.Find(Id);
        // Idempotent, and a refused signal is skipped: Godot was never told about one, so `connect`
        // would fail on the name and "connect failed" is a worse sentence than the one the editor
        // already gave at the member's line.
        if (!Binding || Binding->CallbackId != 0 || Binding->Reject != VH_SIGNAL_OK)
        {
            continue;
        }

        FCallbackTarget Target;
        Target.OwnerHandle = Binding->OwnerHandle;
        Target.EventSignalId = Id;
        int64 CallableRef = 0;
        const int64 CallbackId =
            ConnectDelivery(Binding->OwnerHandle, Binding->Name, MoveTemp(Target), 0, CallableRef);
        if (CallbackId != 0)
        {
            Binding->CallableRef = CallableRef;
            Binding->CallbackId = CallbackId;
            continue;
        }

        // Silence here would be the worst answer available: the member registers, emissions still
        // reach Godot, and only delivery *back into the event* is missing -- so every await on it
        // hangs and nothing says why.
        ReportVerseDiag(verse_diag::VG2113, {{"signal", DiagArg(Binding->Name)}});
    }
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
        FHostState& Host = GetHost();
        for (const int64 Id : Instance->EventBindings)
        {
            if (const FSignalBinding* const Binding = GSignalBindings.Find(Id))
            {
                if (Binding->CallableRef != 0 && Host.Godot.ReleaseRef)
                {
                    Host.Godot.ReleaseRef(Host.Godot.Ctx, Binding->CallableRef);
                }
                if (Binding->CallbackId != 0)
                {
                    FScopeLock Lock(&GCallbacksLock);
                    GCallbacks.Remove(Binding->CallbackId);
                }
                if (const UObject* const Event = Binding->Event.Get())
                {
                    GEventBindingIds.Remove(Event);
                }
            }
            GSignalBindings.Remove(Id);
        }
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
        return VH_ERR_STATE;
    }

    // The first entry into this instance is the earliest point at which Godot will accept a connect
    // to one of the script's own signals, so it is where an `@export_signal` event member's
    // connection is made. A no-op for every instance that declares none, and for every call after
    // the first.
    EnsureEventConnections(*Instance);

    // The signature, for the parameter and result types. Asked of the semantic program rather than
    // of the VM because that is the only view that carries declared types -- the bytecode has
    // erased them by the time a VValue exists.
    const FUtf8String ClassName = QualifiedClassName(Instance->Object->GetClass());
    TArray<FMethodDesc> Methods;
    if (!GetClassMethods(FUtf8StringView(ClassName), Methods))
    {
        return VH_ERR_NOT_FOUND;
    }
    const FMethodDesc* Method = Methods.FindByPredicate(
        [DecoratedName](const FMethodDesc& Candidate) { return FUtf8StringView(Candidate.DecoratedName).Equals(DecoratedName); });
    if (!Method)
    {
        return VH_ERR_NOT_FOUND;
    }

    // One shape cannot be settled yet: a single *struct* parameter is satisfied by one Godot
    // argument per field, and which parameters are structs is not known until the declared types
    // are read below. Everything else is decided here, as it always was.
    const bool bArityMayBeStructPack = Method->Params.Num() == 1 && ArgCount != 1;
    if (!bArityMayBeStructPack && (ArgCount < Method->RequiredParamCount || ArgCount > Method->Params.Num()))
    {
        return VH_ERR_ARGUMENT;
    }

    FVerseFunction Resolved = LookupMethod(Instance, DecoratedName);
    if (!Resolved.IsValid())
    {
        return VH_ERR_NOT_FOUND;
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
    if (const GodotVerse::FDeclaredTypes* const Recorded = RecordedTypes(ClassName))
    {
        if (const FMethodSignatureTypes* const Signature = Recorded->Methods.Find(FUtf8String(DecoratedName)))
        {
            ParamTypes = Signature->Params;
            ResultTypeDesc = Signature->Result;
        }
    }
    if (ParamTypes.Num() != Method->Params.Num())
    {
        return VH_ERR_NOT_FOUND;
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
        return VH_ERR_ARGUMENT;
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
                if (!WireToValue(Context, Args[Index], ParamTypes[Index], Value))
                {
                    Status = VH_ERR_ARGUMENT;
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
                if (ResultTypeDesc.Described.Type != VH_TYPE_VOID
                    && !ValueToWire(Context, OpResult.Value, ResultTypeDesc, OutStorage, OutResult))
                {
                    Status = VH_ERR_ARGUMENT;
                }
                break;

            case Verse::FOpResult::Fail:
                // A <decides> method that ran and declined. Distinct from VH_ERR_NOT_FOUND, which
                // would say there had been nothing to call.
                Status = VH_ERR_FAILED;
                break;

            case Verse::FOpResult::Yield:
                // A <suspends> method started a task instead of completing. Nothing is wrong and
                // there is no value; the task runs on under vh_tick.
                break;

            default:
                Status = VH_ERR_RUNTIME;
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
        return VH_ERR_RUNTIME;
    }
    return bBodyRan ? Status : VH_ERR_HALTED;
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
        return VH_ERR_STATE;
    }

    // From the snapshot, so this costs no analysis and never waits -- Godot asks for an object's
    // text from the remote inspector and from `print`, neither of which is a moment to block on.
    const FUtf8String ClassName = QualifiedClassName(Instance->Object->GetClass());
    const FAnalysisSnapshot::FClass* const Found =
        GSnapshot ? GSnapshot->Classes.Find(ClassName) : nullptr;
    if (!Found || Found->ToStringDecorated.IsEmpty())
    {
        return VH_ERR_NOT_FOUND;
    }

    Verse::VFunction* const Function = FindVFunctionByDecoratedName(FUtf8StringView(Found->ToStringDecorated));
    if (!Function)
    {
        return VH_ERR_NOT_FOUND;
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
                if (!ValueToWire(Context, OpResult.Value, StringType, OutStorage, OutResult))
                {
                    Status = VH_ERR_ARGUMENT;
                }
                break;

            case Verse::FOpResult::Fail:
                Status = VH_ERR_FAILED;
                break;

            default:
                Status = VH_ERR_RUNTIME;
                break;
            }
        });
        });
    });

    if (TransactionResult != AutoRTFM::ETransactionResult::Committed)
    {
        return VH_ERR_RUNTIME;
    }
    if (!bBodyRan)
    {
        return VH_ERR_HALTED;
    }
    return Status;
}

/// The decorated name of a bound Verse method, found by asking the object for each method its
/// class declares and comparing the function that comes back.
///
/// There is no reading the semantic program's spelling back off a VFunction -- the bytecode has
/// erased it -- so the comparison is the lookup, which is the same thing InstanceHasFunction does
/// to tell an override from an inherited body. Once per Subscribe, never per emission.
AUTORTFM_DISABLE bool DescribeBoundFunction(Verse::VFunction* Function, int64& OutHandle, FUtf8String& OutDecorated)
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

namespace {
/// The forward-declared spelling the awaiting path above uses, declared where this file reaches it
/// and defined here, where the function it forwards to exists.
AUTORTFM_DISABLE bool DescribeBoundFunctionFwd(Verse::VFunction* Function, int64& OutHandle, FUtf8String& OutDecorated)
{
    return DescribeBoundFunction(Function, OutHandle, OutDecorated);
}
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

    const int64 Id = GNextCallbackId++;
    {
        FScopeLock Lock(&GCallbacksLock);
        GCallbacks.Add(Id, FCallbackTarget{OwnerHandle, Decorated});
    }
    const int64 Ref = Host.Godot.MakeCallable(Host.Godot.Ctx, Id, OwnerHandle);
    if (Ref == 0)
    {
        FScopeLock Lock(&GCallbacksLock);
        GCallbacks.Remove(Id);
    }
    return Ref;
}

AUTORTFM_DISABLE void GodotVerse::ReleaseCallback(int64 CallbackId)
{
    FScopeLock Lock(&GCallbacksLock);
    GCallbacks.Remove(CallbackId);
}

AUTORTFM_DISABLE int32 GodotVerse::InvokeCallback(int64 CallbackId,
                                                  const vh_value* Args,
                                                  int32 ArgCount,
                                                  vh_value& OutResult,
                                                  FFieldStorage& OutStorage)
{
    // Copied out under the lock rather than held as a pointer: the call below runs Verse, and a
    // Callable released on another thread mid-call would take the row -- and the pointer -- with it.
    FCallbackTarget Target;
    {
        FScopeLock Lock(&GCallbacksLock);
        const FCallbackTarget* const Found = GCallbacks.Find(CallbackId);
        if (!Found)
        {
            return VH_ERR_NOT_FOUND;
        }
        Target = *Found;
    }

    // A Callable the host minted to feed a suspended task rather than to call a script method. It
    // resumes inside this emission, which is where GDScript resumes a coroutine too.
    if (Target.AwaitToken != 0)
    {
        return DeliverToAwaiter(Target.AwaitToken, Args, ArgCount);
    }

    // The permanent connection an `@export_signal` event member holds. Before the instance lookup
    // below for the same reason the await branch is: this Callable feeds an event rather than
    // calling a method, so there is no decorated name to resolve.
    if (Target.EventSignalId != 0)
    {
        return DeliverToEvent(Target.EventSignalId, Args, ArgCount);
    }

    FInstance** const Bound = GInstancesByHandle.Find(Target.OwnerHandle);
    if (!Bound || !*Bound)
    {
        // The node was freed. Godot's own is_valid() should have caught this first; answering
        // rather than raising is what keeps a late emission from taking the frame down.
        return VH_ERR_NOT_FOUND;
    }

    // A foreign signal's subscriber: nothing declares that signal's payload, so the arguments cross
    // as the one container Godot itself would have put them in and the handler takes a godot_array.
    if (Target.bArgsAsArray)
    {
        FHostState& Host = GetHost();
        if (!Host.Godot.NewRef || !Host.Godot.RefSet)
        {
            return VH_ERR_STATE;
        }
        const int64 Ref = Host.Godot.NewRef(Host.Godot.Ctx, VH_VARIANT_ARRAY);
        if (Ref == 0)
        {
            return VH_ERR_STATE;
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

namespace {
// Await invokes this from a closed transactional nest, so it must stay AutoRTFM-enabled.
void OnMainFinished(FVerseTask Task)
{
    if (Task.Completed())
    {
        GodotVerse::RequestExit(GodotVerse::FRunExit::Completed());
    }
}
}

AUTORTFM_DISABLE int32 GodotVerse::RunMain(const TArray<verse::string>& Args, int64& OutExitCode)
{
    const verse::FExecutionContext Context = verse::FExecutionContext::GetActiveContext();

    FMainFunction MainFunction{ScriptPackageLoaded()
                                   ? FVerseFunction(Context, GScriptPackageName, ScriptVersePath, MainFunctionName)
                                   : FVerseFunction(EDefaultConstructVerseFunction::UnsafeDoNotUse)};
    if (!MainFunction.IsValid())
    {
        ReportError(UTF8TEXT("The script has no Main(:[]string, :[string]string) function."));
        return VH_ERR_NOT_FOUND;
    }

    const TMap<verse::string, verse::string> Env;

    EnqueueAsyncJob([MainFunction, Args, Env](const verse::FExecutionContext& ExecContext) {
        const AutoRTFM::ETransactionResult TransactionResult = AutoRTFM::Transact([&] {
            FVerseTask ScriptTask = MainFunction(ExecContext, Args, Env);
            ScriptTask.Await(ExecContext, OnMainFinished);
        });
        if (TransactionResult != AutoRTFM::ETransactionResult::Committed)
        {
            RequestExit(FRunExit::Error());
        }
    });

    PumpEventLoop(Context, 0.0);

    if (!HasPendingExit())
    {
        ReportError(UTF8TEXT("Main suspended on something the host does not drive, and never completed."));
        return VH_ERR_RUNTIME;
    }

    const FRunExit Exit = ConsumeExit();
    OutExitCode = Exit.ExitCode;
    return Exit.Reason == FRunExit::EReason::Error ? VH_ERR_RUNTIME : VH_OK;
}

AUTORTFM_DISABLE void GodotVerse::NoteRuntimeErrorRaised()
{
    // The scope UE is about to terminate is the active one, which under R-ASYNC-4 is the raising
    // instance's. Said here because this delegate is the last moment the task group can be asked
    // what is about to be thrown away; the scope is replaced at that instance's next call.
    if (!verse::FContentScopeGuard::IsActive())
    {
        return;
    }
    const TSharedRef<verse::FContentScope>& Scope = verse::FContentScopeGuard::GetActiveScope();
    if (Scope->HasActiveTasks())
    {
        ReportInfo(&Scope.Get() == GProjectScope.Get()
            ? UTF8TEXT("Suspended work that was not started by any one script instance was "
                       "cancelled by the runtime error above.")
            : UTF8TEXT("This script instance's suspended work was cancelled by the runtime error "
                       "above. Other instances are unaffected (R-ASYNC-4)."));
    }
}

AUTORTFM_DISABLE void GodotVerse::TickScripts(double BudgetSeconds, vh_tick_stats* OutStats)
{
    // The pump's own row (R-DIAG-5). One synthetic signature rather than a row per resumed task,
    // because a `Sleep` resumption has no Godot event behind it to name it after -- what the
    // profiler can honestly say about queued work is how much of the frame it took.
    const GodotVerse::FProfileScope ProfileScope(
        GodotVerse::IsProfilingEnabled() ? FUtf8StringView(UTF8TEXT("<verse>::0::vh_tick"))
                                         : FUtf8StringView());

    PumpEventLoop(verse::FExecutionContext::GetActiveContext(), BudgetSeconds, OutStats);

    // OQ-13 chose observability over a cap, and this is the observation: the largest number of
    // live tasks any one instance's scope holds, which is what a `spawn` in `_Process` runs away
    // with. Nothing else in these numbers separates that from many instances with one task each --
    // a suspended task is not queued work, so JobsPending never sees it.
    //
    // GetNumActive is documented as an implementation detail meant for unit tests. It is used here
    // anyway, and only as a number to *show*: nothing branches on it, so the cost of Epic changing
    // what it counts is a monitor that reads differently, not behaviour that changes.
    if (OutStats
        && OutStats->StructSize >= (int32_t)(offsetof(vh_tick_stats, PeakInstanceTasks) + sizeof(int32_t)))
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
        OutStats->PeakInstanceTasks = Peak;
    }
}
