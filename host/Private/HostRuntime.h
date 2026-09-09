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

void ReportError(FUtf8StringView Message);
void ReportInfo(FUtf8StringView Message);

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
