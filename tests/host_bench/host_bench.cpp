// Times the three host operations whose cost scales with the size of the Godot mirror, with no
// Godot and no editor in the way: loading the engine and its native packages, the one generating
// build, and the analysis the editor runs per keystroke.
//
// Phase 2 needs these numbers before it can choose between the curated class list and the full
// 1022-class mirror (docs/phase-2-design.md 3). Reported rather than asserted: R-PERF-2 asks for a
// recorded number, not a threshold, because there is no baseline to set one against.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "verse_host_abi.h"

namespace fs = std::filesystem;

namespace
{

using Clock = std::chrono::steady_clock;

double MillisSince(Clock::time_point Start)
{
	return std::chrono::duration<double, std::milli>(Clock::now() - Start).count();
}

void BenchPrint(void*, const char*, int32_t)
{
}

vh_bool BenchIsValid(void*, vh_handle)
{
	return 0;
}

int32_t BenchGetProperty(void*, vh_handle, const char*, int32_t, vh_arena*, vh_value*)
{
	return VH_CALL_DEAD_OBJECT;
}

int32_t BenchSetProperty(void*, vh_handle, const char*, int32_t, const vh_value*)
{
	return VH_CALL_DEAD_OBJECT;
}

int32_t BenchCallMethod(void*, vh_handle, const char*, int32_t, const vh_value*, int32_t, vh_arena*, vh_value*)
{
	return VH_CALL_DEAD_OBJECT;
}

int ErrorCount = 0;

void BenchOnDiagnostic(void*, const vh_diagnostic* Diagnostic)
{
	if (Diagnostic->Severity == VH_SEVERITY_ERROR)
	{
		++ErrorCount;
		printf("[bench] error: %.*s(%d,%d): %.*s\n",
			   Diagnostic->FilePathLen,
			   Diagnostic->FilePathUtf8,
			   Diagnostic->Line,
			   Diagnostic->Column,
			   Diagnostic->MessageLen,
			   Diagnostic->MessageUtf8);
	}
}

void BenchOnRuntimeError(void*, const vh_runtime_error*)
{
}

std::string ReadFileUtf8(const fs::path& Path)
{
	std::string Text;
	FILE* File = nullptr;
	if (fopen_s(&File, Path.string().c_str(), "rb") != 0 || !File)
	{
		return Text;
	}
	char Buffer[4096];
	size_t Read = 0;
	while ((Read = fread(Buffer, 1, sizeof(Buffer), File)) > 0)
	{
		Text.append(Buffer, Read);
	}
	fclose(File);
	return Text;
}

template <typename Fn>
Fn Resolve(HMODULE Module, const char* Name, bool* Ok)
{
	FARPROC Proc = GetProcAddress(Module, Name);
	if (!Proc)
	{
		printf("[bench] resolve %s: FAIL\n", Name);
		*Ok = false;
		return nullptr;
	}
	return reinterpret_cast<Fn>(Proc);
}

/// The bytes of the mirror the host will read at runtime, which is the size the numbers below are
/// a function of. Reported so a log says which mirror it measured without being told.
void ReportMirrorSize(const fs::path& EngineDir)
{
	const fs::path Mirror =
		EngineDir / "Engine" / "Source" / "Programs" / "VerseHost" / "Verse" / "GodotClasses.native.verse";
	std::error_code Error;
	const auto Size = fs::file_size(Mirror, Error);
	if (Error)
	{
		printf("[bench] mirror: %ls not found\n", Mirror.c_str());
		return;
	}
	printf("[bench] mirror: %.0f KB (%ls)\n", static_cast<double>(Size) / 1024.0, Mirror.filename().c_str());
}

void ReportSeries(const char* Name, std::vector<double> Samples)
{
	if (Samples.empty())
	{
		return;
	}
	std::sort(Samples.begin(), Samples.end());
	double Total = 0.0;
	for (double Sample : Samples)
	{
		Total += Sample;
	}
	printf("[bench] %-28s n=%zu  min %8.1f ms  median %8.1f ms  max %8.1f ms  mean %8.1f ms\n",
		   Name,
		   Samples.size(),
		   Samples.front(),
		   Samples[Samples.size() / 2],
		   Samples.back(),
		   Total / static_cast<double>(Samples.size()));
}

} // namespace

int main(int argc, char** argv)
{
	wchar_t ExePathW[MAX_PATH];
	GetModuleFileNameW(nullptr, ExePathW, MAX_PATH);
	const fs::path ExeDir = fs::path(ExePathW).parent_path();

	const fs::path DllPath = argc > 1 ? fs::path(argv[1]) : ExeDir / "bin" / "verse_host.dll";
	const char* EngineDirUtf8 = argc > 2 ? argv[2] : nullptr;
	const fs::path VerseBase = argc > 3 ? fs::path(argv[3]) : ExeDir.parent_path();
	const int Iterations = argc > 4 ? atoi(argv[4]) : 10;

	// The smoke test's own fixtures: a script with a class and one with exported members, which is
	// the shape of project the editor analyses.
	const fs::path VersePath = VerseBase / "tests" / "host_smoke" / "hello.verse";
	const fs::path ExportsPath = VerseBase / "tests" / "host_smoke" / "exports.verse";

	if (EngineDirUtf8)
	{
		ReportMirrorSize(fs::path(EngineDirUtf8));
	}

	const Clock::time_point LoadStart = Clock::now();
	HMODULE Module = LoadLibraryExW(DllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	const double LoadMs = MillisSince(LoadStart);
	if (!Module)
	{
		fprintf(stderr, "[bench] could not load %ls (GetLastError=%lu)\n", DllPath.c_str(), GetLastError());
		return 1;
	}

	bool Ok = true;
	auto InitFn = Resolve<vh_init_fn>(Module, "vh_init", &Ok);
	auto ShutdownFn = Resolve<vh_shutdown_fn>(Module, "vh_shutdown", &Ok);
	auto CompileProjectFn = Resolve<vh_compile_project_fn>(Module, "vh_compile_project", &Ok);
	auto CheckProjectFn = Resolve<vh_check_project_fn>(Module, "vh_check_project", &Ok);
	if (!Ok)
	{
		return 1;
	}

	vh_init_desc Desc{};
	Desc.StructSize = sizeof(Desc);
	Desc.AbiVersion = VH_ABI_VERSION;
	Desc.EngineDirUtf8 = EngineDirUtf8;
	Desc.Godot.StructSize = sizeof(Desc.Godot);
	Desc.Godot.Print = &BenchPrint;
	Desc.Godot.IsValid = &BenchIsValid;
	Desc.Godot.GetProperty = &BenchGetProperty;
	Desc.Godot.SetProperty = &BenchSetProperty;
	Desc.Godot.CallMethod = &BenchCallMethod;
	Desc.OnDiagnostic = &BenchOnDiagnostic;
	Desc.OnRuntimeError = &BenchOnRuntimeError;

	const Clock::time_point InitStart = Clock::now();
	const int32_t InitResult = InitFn(&Desc);
	const double InitMs = MillisSince(InitStart);
	if (InitResult != VH_OK)
	{
		fprintf(stderr, "[bench] vh_init failed: %d\n", InitResult);
		return 1;
	}

	const std::string VersePathUtf8 = VersePath.string();
	const std::string ExportsPathUtf8 = ExportsPath.string();
	const char* ProjectPaths[2] = { VersePathUtf8.c_str(), ExportsPathUtf8.c_str() };

	const Clock::time_point CompileStart = Clock::now();
	const int32_t CompileResult = CompileProjectFn(ProjectPaths, 2);
	const double CompileMs = MillisSince(CompileStart);
	if (CompileResult != VH_OK)
	{
		fprintf(stderr, "[bench] vh_compile_project failed: %d\n", CompileResult);
		ShutdownFn();
		return 1;
	}

	// Analysis, as the editor asks for it: the file's own text handed back with one character
	// changed, because an unchanged buffer is the one case the host is allowed to skip.
	const std::string Source = ReadFileUtf8(ExportsPath);
	if (Source.empty())
	{
		fprintf(stderr, "[bench] could not read %ls\n", ExportsPath.c_str());
		ShutdownFn();
		return 1;
	}

	std::vector<double> CheckSamples;
	for (int Iteration = 0; Iteration < Iterations; ++Iteration)
	{
		// A trailing comment line, grown by one character each time. Cheap to append, changes the
		// buffer the host compares against, and cannot change what the analysis has to resolve --
		// so the series measures the mirror rather than the edit.
		std::string Edited = Source;
		Edited.append("\n# ");
		Edited.append(static_cast<size_t>(Iteration) + 1, 'x');
		Edited.append("\n");

		const Clock::time_point CheckStart = Clock::now();
		CheckProjectFn(ExportsPathUtf8.c_str(), Edited.c_str());
		CheckSamples.push_back(MillisSince(CheckStart));
	}

	printf("\n");
	printf("[bench] %-28s %8.1f ms\n", "LoadLibrary", LoadMs);
	printf("[bench] %-28s %8.1f ms\n", "vh_init", InitMs);
	printf("[bench] %-28s %8.1f ms\n", "vh_compile_project", CompileMs);
	ReportSeries("vh_check_project", CheckSamples);
	printf("[bench] diagnostics reported as errors: %d\n", ErrorCount);

	ShutdownFn();
	return 0;
}
