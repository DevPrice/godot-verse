// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "HostScript.h"
#include "HostTypeModel.h"
#include "Templates/SharedPointer.h"

/// The analysis snapshot: what the last analysis to land said about every class the project
/// declares, taken once per analysis off the program that analysis built, and every read that
/// answers from it. The public reads are declared in HostScript.h; this is the snapshot's own
/// shape and what the build lifecycle calls to take one and make it current.
namespace GodotVerse {

/// One method's declared parameter and result types, which is what a call needs and what
/// FMethodDesc -- which carries only what crosses the ABI -- does not have.
struct FMethodSignatureTypes
{
    TArray<FMemberType> Params;
    FMemberType Result;
};

/// The three tables one class's analysis recorded. The sidecar moves them through
/// WriteDeclaredTypes and ReadDeclaredTypes (HostSidecar.h), which drop the uLang pointers an
/// FMemberType carries.
struct FDeclaredTypes
{
    /// Member name -> declared type, derived class first and up the script chain.
    TMap<FUtf8String, FMemberType> Members;
    /// Decorated method name -> what it takes and answers.
    TMap<FUtf8String, FMethodSignatureTypes> Methods;
    /// Signal member name -> what its payload decomposes into.
    TMap<FUtf8String, FPayloadShape> Signals;
};

/// Everything the class-describing entry points answer, extracted once per analysis.
///
/// The stall this removes is the editor's. Around twenty ABI entry points used to call
/// WaitForBackgroundCheck before touching the semantic program, and the editor asks several of them
/// on the game thread while an analysis it started is still running -- `_validate` wants the method
/// outline, the inspector wants the property list, a hover wants the documentation. Waiting was
/// correct (the worker is rebuilding the very program they read, and VerseVM blocks execution for
/// the length of a build) and it cost the rest of the analysis, which is where the ~1.7 s hang came
/// from.
///
/// Two halves, because only one of them may be filled off the game thread. The *semantic* half is
/// read out of the program the analysis just built, by whichever thread built it. The *VM* half --
/// whether the published generation carries the class, a statics module's constant values, an
/// `@export`'s declared default -- has to enter the VM, which is the game thread's alone, so it is
/// filled at the moment the snapshot is made current. Neither half is read while it is being
/// written: the pending snapshot is private to the thread building it until the swap.
struct FAnalysisSnapshot
{
    struct FClass
    {
        bool bAbstract = false;
        TArray<GodotVerse::FMethodDesc> Methods;
        TArray<GodotVerse::FSignalDesc> Signals;
        TArray<GodotVerse::FRpcDesc> Rpcs;
        TArray<GodotVerse::FCompleteItem> Members;

        /// What the class inherits and could still override. Held beside Members rather than
        /// derived from it because deriving it needs the superclass chain, which is the program
        /// the analysis just built -- and the question is asked on the keystroke that opens
        /// completion, where nothing may touch that program.
        TArray<GodotVerse::FCompleteItem> OverrideCandidates;

        /// The decorated name of the `ToString` extension method the project wrote for this class,
        /// or empty when it wrote none -- in which case Godot keeps its own `<Node2D#27>`.
        ///
        /// Recorded here rather than looked up on demand for the reason everything else in this
        /// struct is: a runtime host has no semantic program to ask, and the editor may not wait
        /// on one. It is a *module-level* name, not a member -- see FindToStringExtensionLive.
        FUtf8String ToStringDecorated;

        /// GetClassExports answers false for a program with no export attribute in it as well as
        /// for a class that is not there, and the two mean different things to `_validate`.
        bool bExportsHarvested = false;
        TArray<GodotVerse::FExportDesc> Exports;

        /// Whether the *published* generation carries the class, which is a different question from
        /// whether this analysis declares it -- a class renamed in an unsaved buffer is in one and
        /// not the other.
        bool bInPublishedProgram = false;

        /// Shared rather than held by value: a vh_value in either points into the FFieldStorage
        /// beside it, so what the ABI hands out has to be kept alive by the caller rather than
        /// copied. Defaults are keyed by member name and hold one entry per `@export`.
        TSharedPtr<const GodotVerse::FClassStatics> Statics;
        TMap<FUtf8String, TSharedPtr<const GodotVerse::FFieldValue>> Defaults;

        /// Every declared type the class's members, methods and signals carry.
        ///
        /// **This is what makes an exported game able to run Verse at all.** Reading a field,
        /// calling a method and emitting a signal each need the *declared* type -- the bytecode has
        /// erased it by the time a VValue exists, so the semantic program was the only view that
        /// had one. A runtime host has no semantic program and can never build one
        /// (`MakeDevEnvironment` is one of the four ISolarisModule members WITH_VERSE_COMPILER=0
        /// takes away), so the analysis records them here and the sidecar carries them across.
        ///
        TSharedPtr<FDeclaredTypes> Types;
    };

    /// Module-qualified, exactly as every ClassNameUtf8 in the ABI is: `player`, `gameplay/player`.
    TMap<FUtf8String, FClass> Classes;

    /// A generated binding's own members, keyed by the binding's class name.
    ///
    /// Beside Classes rather than in it, and that is the point: everything reading Classes is
    /// asking about a class the *project* declares -- whether it is abstract, what it exports,
    /// whether the published generation carries it -- and a binding answers none of those. In one
    /// map a binding would also take a script class's place, which a project with a
    /// `class_name Mover` GDScript beside a `mover.verse` would do on its first analysis.
    ///
    /// Members and nothing else, because a completion behind a `.` is the one question a script
    /// asks about a binding that cannot wait for an analysis. The sidecar does not carry these:
    /// a runtime host completes nothing.
    TMap<FUtf8String, TArray<GodotVerse::FCompleteItem>> BindingMembers;

    /// ResolveUnknownName's whole answer, inverted: which modules declare each top-level name. Built
    /// here because the walk reads the AST project, which the worker rebuilds under it -- that read
    /// never waited and so was a race as well as a cost.
    TMap<FUtf8String, TArray<FUtf8String>> ModulesDeclaring;
    bool bAstAvailable = false;
};

/// The snapshot every class-describing entry point answers from. Null before the first analysis.
///
/// Exposed for the sidecar (HostSidecar.cpp), which is the *only* other thing that touches it: the
/// cooker writes this out and a runtime host, which can never take one of its own, reads one back.
AUTORTFM_DISABLE const TSharedPtr<const FAnalysisSnapshot>& GetAnalysisSnapshot();

/// Makes one current, for a host with no compiler to publish what it loaded from disk. Game thread
/// only, like the swap TakeAnalysisSnapshot's publish does.
AUTORTFM_DISABLE void SetAnalysisSnapshot(TSharedRef<const FAnalysisSnapshot> Snapshot);

/// What every mirrored engine-signal accessor's `signal(t)` carries, keyed `timer.Timeout`.
///
/// The same problem as FDeclaredTypes and the same answer, for the other half of the signal
/// surface: `Await`ing one of Godot's own signals has to rebuild `t` from the arguments the
/// emission delivered, and only the accessor's declared return type says what `t` is. An editor
/// host reads it off the semantic program per accessor and caches it; a runtime host has no program
/// to read, so the cook records all of them once and the sidecar carries them. Without it every
/// `Timer.Timeout().Await()` in an exported game connects and then never resumes.
AUTORTFM_DISABLE TSharedPtr<struct FEngineSignalTypes> CollectEngineSignalTypes();

/// Fills the semantic half of the pending snapshot off the program the IDE holds. Runs on whichever
/// thread built that program.
AUTORTFM_DISABLE void TakeAnalysisSnapshot();

/// Fills the VM half of the pending snapshot and makes it current. Game thread only.
AUTORTFM_DISABLE void PublishAnalysisSnapshot();

/// Drops the current and the pending snapshot, before the IDE whose program they describe goes.
AUTORTFM_DISABLE void ForgetAnalysisSnapshots();

/// The two things `@statics` was chosen over a naming convention to make checkable (R-NODE-4),
/// said about the program the analysis just left.
AUTORTFM_DISABLE void ReportStaticsDiagnostics();

} // namespace GodotVerse
