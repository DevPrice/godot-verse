// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Misc/AssertionMacros.h"
#include "Misc/Optional.h"
#include "Templates/UnrealTemplate.h"

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

} // namespace GodotVerse
