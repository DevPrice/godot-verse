// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Array.h"
#include "Containers/StringView.h"
#include "CoreMinimal.h"
#include "verse_host_abi.h"

namespace GodotVerse {

struct FHostState
{
    bool bInitialized{false};
    vh_godot_api Godot{};
    vh_diagnostic_fn OnDiagnostic{nullptr};
    void* DiagnosticCtx{nullptr};
    vh_runtime_error_fn OnRuntimeError{nullptr};
    void* RuntimeErrorCtx{nullptr};
};

FHostState& GetHost();

void ReportDiagnostic(vh_severity Severity,
                      FUtf8StringView Message,
                      FUtf8StringView FilePath,
                      int32 Line,
                      int32 Column,
                      int32 EndLine,
                      int32 EndColumn,
                      int32 ReferenceCode);

/// Drops the host's claim on a GDExtension reference id. Called from godot_ref::BeginDestroy, so
/// it runs on the collector's thread of control and must not touch the VM.
void ReleaseGodotRef(int64 Ref);

void ReportError(FUtf8StringView Message);
void ReportInfo(FUtf8StringView Message);

/// Forwards a Verse runtime error with its call stack (R-DIAG-2).
///
/// Callstack is the VM's own rendering, one frame per line as `	<path> <name>:<line>`, which is
/// parsed back into frames here. Parsing a rendered string is not the shape anyone would choose,
/// but the VM offers no structured form: FVerseRuntimeErrorDelegates hands subscribers the text
/// and nothing else, and the frames it was rendered from are gone by the time the delegate runs --
/// RaiseVerseRuntimeError unwinds the stack immediately afterwards.
void ReportRuntimeError(FUtf8StringView Message, const FString& Callstack);

inline FUtf8StringView MakeView(const char* Utf8, int32 Len)
{
    return FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Utf8), Len);
}

/// Bump allocator handed to Godot callbacks so they can return strings and containers.
/// Everything it hands out dies with the arena, which lives no longer than one native call.
class FCallArena : public vh_arena
{
public:
    FCallArena() { Alloc = &AllocThunk; }
    ~FCallArena();

    FCallArena(const FCallArena&) = delete;
    FCallArena& operator=(const FCallArena&) = delete;

private:
    static void* AllocThunk(vh_arena* Self, size_t Size, size_t Align);
    void* Allocate(size_t Size, size_t Align);

    TArray<uint8*> Blocks;
    size_t BlockOffset{0};
    size_t BlockSize{0};
};

} // namespace GodotVerse
