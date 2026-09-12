// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostRuntime.h"
#include "Containers/Utf8String.h"
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

void ReportRuntimeError(FUtf8StringView Message, const FString& Callstack)
{
    FHostState& Host = GetHost();
    if (!Host.OnRuntimeError)
    {
        // No runtime error callback: fold it into the ordinary diagnostics so the message is not
        // simply lost, which is what v1 did for every runtime error.
        ReportDiagnostic(VH_SEVERITY_ERROR, Message, FUtf8StringView(), 0, 0, 0, 0, 0);
        return;
    }

    TArray<FString> Lines;
    Callstack.ParseIntoArrayLines(Lines, /*bCullEmpty*/ true);

    // The frames own their bytes, and the descriptors point into them, so both have to outlive the
    // callback. Reserved to final size before any descriptor is built: growing Paths or Names
    // afterwards would leave every pointer already written dangling.
    TArray<FUtf8String> Paths;
    TArray<FUtf8String> Names;
    TArray<vh_stack_frame> Frames;
    Paths.Reserve(Lines.Num());
    Names.Reserve(Lines.Num());
    Frames.Reserve(Lines.Num());

    for (const FString& Raw : Lines)
    {
        FString Line = Raw;
        Line.TrimStartAndEndInline();
        if (Line.IsEmpty())
        {
            continue;
        }

        // `<path> <name>:<line>`, with the path and the line each optional. The line is taken from
        // the last colon only when what follows it is entirely digits -- a Windows path carries a
        // colon of its own in the drive letter.
        int32 Line1 = 0;
        int32 ColonIndex = INDEX_NONE;
        if (Line.FindLastChar(TEXT(':'), ColonIndex))
        {
            const FString Suffix = Line.Mid(ColonIndex + 1);
            if (!Suffix.IsEmpty() && Suffix.IsNumeric())
            {
                Line1 = FCString::Atoi(*Suffix);
                Line = Line.Left(ColonIndex);
            }
        }

        FString PathPart;
        FString NamePart = Line;
        int32 SpaceIndex = INDEX_NONE;
        if (Line.FindLastChar(TEXT(' '), SpaceIndex))
        {
            PathPart = Line.Left(SpaceIndex);
            NamePart = Line.Mid(SpaceIndex + 1);
        }

        Paths.Add(FUtf8String(PathPart));
        Names.Add(FUtf8String(NamePart));

        vh_stack_frame& Frame = Frames.AddDefaulted_GetRef();
        Frame.PathUtf8 = reinterpret_cast<const char*>(*Paths.Last());
        Frame.PathLen = Paths.Last().Len();
        Frame.FunctionUtf8 = reinterpret_cast<const char*>(*Names.Last());
        Frame.FunctionLen = Names.Last().Len();
        Frame.Line = Line1;
        Frame.Column = 0;
    }

    vh_runtime_error Error{};
    Error.MessageUtf8 = reinterpret_cast<const char*>(Message.GetData());
    Error.MessageLen = Message.Len();
    Error.Frames = Frames.GetData();
    Error.FrameCount = Frames.Num();

    Host.OnRuntimeError(Host.RuntimeErrorCtx, &Error);
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
