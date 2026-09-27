// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostBuild.h"
#include "HostScript.h"
#include "HostSnapshot.h"
#include "HostCallbacks.h"
#include "HostEngineAdapters.h"
#include "HostInstances.h"
#include "HostMarshal.h"
#include "HostPeers.h"
#include "HostScriptState.h"
#include "HostSignals.h"
#include "HostTypeModel.h"
#include "HostVerseEntry.h"
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
using GodotVerse::DescribeMemberType;
using GodotVerse::DescribePayload;
using GodotVerse::DescribeType;
using GodotVerse::EClassOrigin;
using GodotVerse::EDeclaredKind;
using GodotVerse::EPayloadShape;
using GodotVerse::EnterVerse;
using GodotVerse::FVerseEntry;
using GodotVerse::DecoratedNameOf;
using GodotVerse::ExtensionMethodDecoratedName;
using GodotVerse::FCallbackTarget;
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
using GodotVerse::ForgetCachedClasses;
using GodotVerse::ForgetMirrorDefinitions;
using GodotVerse::FPayloadArg;
using GodotVerse::FPayloadShape;
using GodotVerse::FStructLayout;
using GodotVerse::FUserStructLayout;
using GodotVerse::GodotVersePath;
using GodotVerse::MirrorDefinitionsRecorded;
using GodotVerse::NativeClassOf;
using GodotVerse::NearestAncestorOfOrigin;
using GodotVerse::NewArrayValue;
using GodotVerse::NewDefaultsObject;
using GodotVerse::NewReferenceWrapper;
using GodotVerse::NewStructValue;
using GodotVerse::PayloadStructClass;
using GodotVerse::QualifiedNameOf;
using GodotVerse::ReadDefaultFieldOf;
using GodotVerse::ReferenceOption;
using GodotVerse::ValueToWire;
using GodotVerse::WireToValue;
using GodotVerse::RecordMirrorDefinitions;
using GodotVerse::ScriptVersePath;
using GodotVerse::SignalPayloadType;
using GodotVerse::SubjectTypeOfDiagnostic;
using GodotVerse::UnwrapDeclaredType;
using GodotVerse::PublishAnalysisSnapshot;
using GodotVerse::ReportStaticsDiagnostics;
using GodotVerse::TakeAnalysisSnapshot;

namespace {

/// What every generation's package name starts with; the generation number finishes it.
///
/// Not ISolIdeDataSource::DefaultDataSourceName, which is what ISolarisIde::AddDataSource would
/// have picked: the name has to be the host's to choose, because publishing a package marks its
/// exports LoaderImport and publishing that same package again asserts on the flag. A generation
/// is therefore a name no publish has used, and a name the IDE owns cannot be one.
constexpr const char* ScriptPackageBaseName = "GodotScripts";

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

} // namespace

namespace {

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

AUTORTFM_DISABLE bool GodotVerse::HasIde()
{
    return GIde.IsValid();
}

AUTORTFM_DISABLE uLang::TSPtr<uLang::CProgramBuildManager> GodotVerse::IdeBuildManager()
{
    return GIde.IsValid() ? GIde->GetBuildManager() : nullptr;
}

AUTORTFM_DISABLE const FUtf8String& GodotVerse::PublishedScriptPackageName()
{
    return GScriptPackageName;
}

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

AUTORTFM_DISABLE void GodotVerse::ResetScriptState()
{
    // The caller joins the worker first (vh_shutdown's WaitForBackgroundCheck): this releases the
    // IDE the worker builds with, and a std::thread still joinable at static destruction is
    // std::terminate.
    check(!GBackgroundCheck.Thread.joinable());
    LeaveContentScope();
    // Before the IDE goes: a pending snapshot describes a program that is about to stop existing,
    // and a worker joined at shutdown leaves one nothing will ever publish.
    ForgetAnalysisSnapshots();
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

AUTORTFM_DISABLE const FUtf8String* GodotVerse::BindingForScriptClass(FUtf8StringView ScriptClass)
{
    return GBindingByScriptClass.Find(FUtf8String(ScriptClass));
}

AUTORTFM_DISABLE const FUtf8String* GodotVerse::BindingForGodotClass(FUtf8StringView GodotClass)
{
    return GBindingByGodotClass.Find(FUtf8String(GodotClass));
}

AUTORTFM_DISABLE bool GodotVerse::HasScriptClassBindings()
{
    return !GBindingByScriptClass.IsEmpty();
}

AUTORTFM_DISABLE const FUtf8String* GodotVerse::MintNameForBinding(FUtf8StringView VerseClass)
{
    return GMintNameByBinding.Find(FUtf8String(VerseClass));
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

    // Before either road rather than after a success: a build that fails in IR generation, assembly
    // or the link has already hung an IR package off every module, and the position walks assert
    // on one. A build that fails earlier leaves a program they could walk, and the next analysis --
    // which the consumer asks for on VH_ERR_NOT_ANALYSED -- replaces it anyway.
    GProgramIsAnalysisOnly = false;

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
        VH_UNREPORTED("RunCheck: no generation has been built, so there is no project to analyse");
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
    // BuildAll starts from a new program whenever the IDE has a build manager, so the program held
    // now is this analysis's whatever it concluded -- a failed one included, which is most of them
    // while an author is typing. Arming this only on a clean analysis left every hover after a Play
    // answering "not analysed yet" for as long as the buffer did not compile.
    if (GIde->GetBuildManager().IsValid())
    {
        GProgramIsAnalysisOnly = true;
    }

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

AUTORTFM_DISABLE GodotVerse::TResult<void> GodotVerse::BeginBackgroundCheck(const FUtf8String& Path,
                                                                          const FUtf8String& SourceText)
{
    if (GBackgroundCheck.bRunning.load(std::memory_order_acquire))
    {
        return EHostFailure::AnalysisInFlight;
    }
    // A finished worker still joinable, or one a wait joined: either way its diagnostics and its
    // snapshot are waiting for vh_check_project_poll, and starting another would overwrite both.
    if (GBackgroundCheck.Thread.joinable() || GBackgroundCheck.bResultPending)
    {
        return EHostFailure::AnalysisNotReaped;
    }
    if (!GProjectBuilt || !GIde.IsValid())
    {
        return EHostFailure::NotBuilt;
    }

    GBackgroundCheck.Path = Path;
    GBackgroundCheck.SourceText = SourceText;
    GBackgroundCheck.Diagnostics.Empty();
    GBackgroundCheck.bResult = false;
    GBackgroundCheck.bRunning.store(true, std::memory_order_release);
    GBackgroundCheck.Thread = std::thread(&BackgroundCheckMain);
    return TResult<void>::Ok();
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

AUTORTFM_DISABLE bool GodotVerse::ScriptPackageLoaded()
{
    return Verse::GlobalProgram && Verse::GlobalProgram->LookupPackage(GScriptPackageName) != nullptr;
}

namespace {

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

namespace {

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

AUTORTFM_DISABLE void GodotVerse::AdoptCookedGeneration(FUtf8StringView PackageName, int32 Generation)
{
    GScriptPackageName = FUtf8String(PackageName);
    GScriptSourcePackageName = GScriptPackageName;
    GScriptGeneration = Generation;
    GProjectBuilt = true;
}
