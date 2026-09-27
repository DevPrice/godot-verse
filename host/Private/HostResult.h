// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Misc/AssertionMacros.h"
#include "Misc/Optional.h"
#include "Templates/UnrealTemplate.h"

#include <atomic>

namespace GodotVerse {

/// Why the host has no answer, as the host's internal surface reports it. Closed: VerseHost.cpp
/// turns one into a vh_status in a single exhaustive switch, so a reason added here fails the host
/// build until it has decided which status the consumer should see.
///
/// The members are the reasons the functions answering a TResult actually decline for, and grow
/// as each unit item 5 splits out of HostScript.cpp takes the type.
enum class EHostFailure : uint8
{
    /// No analysis has left a snapshot to answer from.
    NotAnalysed,
    /// An analysis is in flight, and the program it is rebuilding is the one the question reads.
    AnalysisRunning,
    /// A build has generated code since the last analysis, which leaves no AST to resolve against.
    BuiltSinceAnalysis,
    /// The last analysis read different text for this file than the question is about.
    BufferNotAnalysed,
    /// A row or column below zero.
    InvalidPosition,
    /// Nothing at the position resolves to what was asked for.
    NothingAtPosition,
    /// The position resolves, to a definition that is not a function.
    NotAFunction,
    /// The analysed program declares no class of that name.
    NoSuchClass,
    /// The published program has no class or enumeration of the name a declaration or a layout
    /// names -- a retired generation's, a nested class, or a build that has not published yet.
    NotPublished,
    /// The value is not of the kind its declaration, or the wire, says it is.
    TypeMismatch,
    /// The declared type has no representation on the other side of the wire.
    Unconvertible,
    /// Godot's null, for a declaration that is not an option.
    NullNotOptional,
    /// An ordinal with no enumerator in the enum the author declared.
    EnumOrdinalOutOfRange,
    /// A struct value without a field its layout names.
    MissingField,
    /// The VM refused to create or set a field of a value being built.
    ConstructionFailed,
    /// The consumer supplied no callback for what this needs.
    CallbackMissing,
    /// The consumer's callback answered an error.
    CallbackFailed,
    /// No live row answers to the id: a callback, a signal binding or a wait that was never made or
    /// has already been released.
    UnknownId,
    /// The object named as a signal, or held by a wait or a binding, carries no event to signal.
    NotASignal,
    /// The Verse code run for the answer failed or raised, and its transaction was aborted.
    Aborted,
    /// The VM declined to run the body at all -- the scope it would run under was terminated.
    Halted,
    /// A <decides> method ran and declined, which leaves no value and is not a missing method.
    Declined,
    /// The script instance, or the object it held, has been released.
    InstanceReleased,
    /// The object's shape carries no data member of that name.
    NoSuchMember,
    /// The member exists and holds no value yet.
    Unset,
    /// The member cannot be written now: a non-var on a sealed instance, or a shape constant.
    NotAssignable,
    /// The class declares no method of that name, or has no ToString extension method.
    NoSuchMethod,
    /// The analysis recorded no declared types for the method, or recorded a different arity.
    SignatureNotRecorded,
    /// More or fewer arguments than the method takes.
    WrongArgumentCount,
    /// Godot's own half of an answer is missing: no callback to make a container with, or one that
    /// made nothing.
    GodotUnavailable,
};

/// A value, or the reason there is none.
template <typename T, typename EFailure = EHostFailure>
class TResult
{
public:
    TResult(const T& InValue)
        : Value(InValue)
    {
    }
    TResult(T&& InValue)
        : Value(MoveTemp(InValue))
    {
    }
    TResult(EFailure InFailure)
        : Failure(InFailure)
    {
    }

    bool IsOk() const { return Value.IsSet(); }
    explicit operator bool() const { return IsOk(); }

    T& GetValue()
    {
        check(IsOk());
        return Value.GetValue();
    }
    const T& GetValue() const
    {
        check(IsOk());
        return Value.GetValue();
    }

    EFailure GetFailure() const
    {
        check(!IsOk());
        return Failure;
    }

private:
    TOptional<T> Value;
    EFailure Failure{};
};

/// Success with nothing to carry, or the reason it failed: what a function that used to answer
/// bool and fill out-parameters answers instead.
template <typename EFailure>
class TResult<void, EFailure>
{
public:
    static TResult Ok() { return TResult(); }

    TResult(EFailure InFailure)
        : Failure(InFailure)
    {
    }

    bool IsOk() const { return !Failure.IsSet(); }
    explicit operator bool() const { return IsOk(); }

    EFailure GetFailure() const
    {
        check(!IsOk());
        return Failure.GetValue();
    }

private:
    TResult() = default;

    TOptional<EFailure> Failure;
};

/// The vh_status a consumer sees for a failure: the one place a reason becomes a status. Defined in
/// VerseHost.cpp, beside the entry points; a unit whose answer reaches the ABI as a status of its
/// own -- InvokeCallback's -- calls it rather than choosing one.
int32 StatusFor(EHostFailure Failure);

AUTORTFM_DISABLE void ReportUnreported(const char* Reason, const char* File, int32 Line);

} // namespace GodotVerse

/// Marks a failure path that answers without saying why -- a `return false` or `return {}` that
/// carries no EHostFailure -- so the silent paths are one grep away and can be counted down. Says so
/// once per site on stderr in a development host, and is nothing in Shipping.
#if UE_BUILD_SHIPPING
#define VH_UNREPORTED(Reason) ((void)0)
#else
#define VH_UNREPORTED(Reason)                                                           \
    do                                                                                  \
    {                                                                                   \
        static std::atomic<bool> bVhUnreportedSaid{false};                              \
        if (!bVhUnreportedSaid.exchange(true))                                          \
        {                                                                               \
            ::GodotVerse::ReportUnreported((Reason), __FILE__, __LINE__);               \
        }                                                                               \
    } while (0)
#endif
