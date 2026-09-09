// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostRuntime.h"
#include "HAL/UnrealMemory.h"
#include "Math/UnrealMathUtility.h"

namespace GodotVerse {

FHostState& GetHost()
{
    static FHostState State;
    return State;
}

void ReportDiagnostic(vh_severity Severity,
                      FUtf8StringView Message,
                      FUtf8StringView FilePath,
                      int32 Line,
                      int32 Column,
                      int32 EndLine,
                      int32 EndColumn,
                      int32 ReferenceCode)
{
    FHostState& Host = GetHost();
    if (!Host.OnDiagnostic)
    {
        return;
    }

    vh_diagnostic Diagnostic{};
    Diagnostic.Severity = static_cast<int32_t>(Severity);
    Diagnostic.MessageUtf8 = reinterpret_cast<const char*>(Message.GetData());
    Diagnostic.MessageLen = Message.Len();
    Diagnostic.FilePathUtf8 = reinterpret_cast<const char*>(FilePath.GetData());
    Diagnostic.FilePathLen = FilePath.Len();
    Diagnostic.Line = Line;
    Diagnostic.Column = Column;
    Diagnostic.EndLine = EndLine;
    Diagnostic.EndColumn = EndColumn;
    Diagnostic.ReferenceCode = ReferenceCode;

    Host.OnDiagnostic(Host.DiagnosticCtx, &Diagnostic);
}

void ReportError(FUtf8StringView Message)
{
    ReportDiagnostic(VH_SEVERITY_ERROR, Message, FUtf8StringView(), 0, 0, 0, 0, 0);
}

void ReportInfo(FUtf8StringView Message)
{
    ReportDiagnostic(VH_SEVERITY_INFO, Message, FUtf8StringView(), 0, 0, 0, 0, 0);
}

namespace {
constexpr size_t DefaultBlockSize = 64 * 1024;
}

FCallArena::~FCallArena()
{
    for (uint8* Block : Blocks)
    {
        FMemory::Free(Block);
    }
}

void* FCallArena::AllocThunk(vh_arena* Self, size_t Size, size_t Align)
{
    return static_cast<FCallArena*>(Self)->Allocate(Size, Align);
}

void* FCallArena::Allocate(size_t Size, size_t Align)
{
    if (Align == 0)
    {
        Align = alignof(std::max_align_t);
    }

    if (!Blocks.IsEmpty())
    {
        const size_t Aligned = (BlockOffset + Align - 1) & ~(Align - 1);
        if (Aligned + Size <= BlockSize)
        {
            BlockOffset = Aligned + Size;
            return Blocks.Last() + Aligned;
        }
    }

    const size_t NewBlockSize = FMath::Max(DefaultBlockSize, Size + Align);
    uint8* Block = static_cast<uint8*>(FMemory::Malloc(NewBlockSize, Align));
    if (!Block)
    {
        return nullptr;
    }
    Blocks.Add(Block);
    BlockSize = NewBlockSize;
    BlockOffset = Size;
    return Block;
}

} // namespace GodotVerse
