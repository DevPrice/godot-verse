// Standalone console driver that loads verse_host.dll and exercises the whole C ABI with no
// Godot and no UE dependency, to catch ABI breaks before wiring up the GDExtension.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "verse_host_abi.h"

namespace fs = std::filesystem;

static void SmokePrint(void*, const char* Utf8, int32_t Len)
{
	fwrite(Utf8, 1, static_cast<size_t>(Len), stdout);
	fputc('\n', stdout);
}

static vh_handle SmokeGetNode(void*, const char*, int32_t)
{
	return 0;
}

static vh_bool SmokeIsValid(void*, vh_handle)
{
	return 0;
}

static vh_bool SmokeGetProperty(void*, vh_handle, const char*, int32_t, vh_arena*, vh_value*)
{
	return 0;
}

static vh_bool SmokeSetProperty(void*, vh_handle, const char*, int32_t, const vh_value*)
{
	return 0;
}

static vh_bool SmokeCallMethod(void*, vh_handle, const char*, int32_t, const vh_value*, int32_t, vh_arena*, vh_value*)
{
	return 0;
}

static int32_t SmokeGetChildCount(void*, vh_handle)
{
	return 0;
}

static vh_handle SmokeGetChild(void*, vh_handle, int32_t)
{
	return 0;
}

static const char* SeverityName(int32_t Severity)
{
	switch (Severity)
	{
	case VH_SEVERITY_INFO: return "info";
	case VH_SEVERITY_WARNING: return "warning";
	case VH_SEVERITY_ERROR: return "error";
	default: return "unknown";
	}
}

static void SmokeOnDiagnostic(void*, const vh_diagnostic* Diagnostic)
{
	fprintf(stderr, "%.*s:%d:%d: %s: %.*s\n",
		Diagnostic->FilePathLen, Diagnostic->FilePathUtf8,
		Diagnostic->Line, Diagnostic->Column,
		SeverityName(Diagnostic->Severity),
		Diagnostic->MessageLen, Diagnostic->MessageUtf8);
}

static bool Step(const char* Name, bool Result)
{
	printf("[smoke] %s: %s\n", Name, Result ? "ok" : "FAIL");
	return Result;
}

template <typename Fn>
static Fn Resolve(HMODULE Module, const char* Name, bool* Ok)
{
	if (!*Ok)
	{
		return nullptr;
	}
	FARPROC Proc = GetProcAddress(Module, Name);
	if (!Proc)
	{
		printf("[smoke] resolve %s: FAIL (symbol not found)\n", Name);
		*Ok = false;
		return nullptr;
	}
	return reinterpret_cast<Fn>(Proc);
}

int main(int argc, char** argv)
{
	wchar_t ExePathW[MAX_PATH];
	GetModuleFileNameW(nullptr, ExePathW, MAX_PATH);
	fs::path ExeDir = fs::path(ExePathW).parent_path();

	fs::path DllPath = argc > 1 ? fs::path(argv[1]) : ExeDir / "bin" / "verse_host.dll";
	const char* EngineDirUtf8 = argc > 2 ? argv[2] : nullptr;
	fs::path VerseBase = argc > 3 ? fs::path(argv[3]) : ExeDir.parent_path();
	fs::path VersePath = VerseBase / "tests" / "host_smoke" / "hello.verse";

	HMODULE Module = LoadLibraryW(DllPath.c_str());
	if (!Step("LoadLibraryW", Module != nullptr))
	{
		fprintf(stderr, "[smoke] could not load %ls\n", DllPath.c_str());
		return 1;
	}

	bool ResolveOk = true;
	auto AbiVersionFn = Resolve<vh_abi_version_fn>(Module, "vh_abi_version", &ResolveOk);
	auto InitFn = Resolve<vh_init_fn>(Module, "vh_init", &ResolveOk);
	auto ShutdownFn = Resolve<vh_shutdown_fn>(Module, "vh_shutdown", &ResolveOk);
	auto TickFn = Resolve<vh_tick_fn>(Module, "vh_tick", &ResolveOk);
	auto CompileFileFn = Resolve<vh_compile_file_fn>(Module, "vh_compile_file", &ResolveOk);
	auto ReleaseScriptFn = Resolve<vh_release_script_fn>(Module, "vh_release_script", &ResolveOk);
	auto ScriptHasFunctionFn = Resolve<vh_script_has_function_fn>(Module, "vh_script_has_function", &ResolveOk);
	auto RunMainFn = Resolve<vh_run_main_fn>(Module, "vh_run_main", &ResolveOk);
	auto CallVoidFn = Resolve<vh_call_void_fn>(Module, "vh_call_void", &ResolveOk);
	auto CallVoidFloatFn = Resolve<vh_call_void_float_fn>(Module, "vh_call_void_float", &ResolveOk);
	(void)TickFn;
	(void)ScriptHasFunctionFn;
	(void)CallVoidFn;
	(void)CallVoidFloatFn;
	if (!Step("resolve exports", ResolveOk))
	{
		return 1;
	}

	if (!Step("vh_abi_version", AbiVersionFn() == VH_ABI_VERSION))
	{
		fprintf(stderr, "[smoke] abi version mismatch: got %d, expected %d\n", AbiVersionFn(), VH_ABI_VERSION);
		return 1;
	}

	vh_init_desc Desc{};
	Desc.StructSize = sizeof(Desc);
	Desc.AbiVersion = VH_ABI_VERSION;
	Desc.EngineDirUtf8 = EngineDirUtf8;
	Desc.Godot.StructSize = sizeof(Desc.Godot);
	Desc.Godot.Ctx = nullptr;
	Desc.Godot.Print = &SmokePrint;
	Desc.Godot.GetNode = &SmokeGetNode;
	Desc.Godot.IsValid = &SmokeIsValid;
	Desc.Godot.GetProperty = &SmokeGetProperty;
	Desc.Godot.SetProperty = &SmokeSetProperty;
	Desc.Godot.CallMethod = &SmokeCallMethod;
	Desc.Godot.GetChildCount = &SmokeGetChildCount;
	Desc.Godot.GetChild = &SmokeGetChild;
	Desc.OnDiagnostic = &SmokeOnDiagnostic;
	Desc.DiagnosticCtx = nullptr;
	Desc.EnableDebugger = 0;

	if (!Step("vh_init", InitFn(&Desc) == VH_OK))
	{
		return 1;
	}

	std::string VersePathUtf8 = VersePath.string();
	vh_script* Script = nullptr;
	if (!Step("vh_compile_file", CompileFileFn(VersePathUtf8.c_str(), &Script) == VH_OK))
	{
		ShutdownFn();
		return 1;
	}

	const char* RunArgs[1] = { VersePathUtf8.c_str() };
	int64_t ExitCode = 0;
	bool RunOk = RunMainFn(Script, RunArgs, 1, &ExitCode) == VH_OK;
	Step("vh_run_main", RunOk);
	printf("[smoke] exit code: %lld\n", static_cast<long long>(ExitCode));

	ReleaseScriptFn(Script);
	Step("vh_release_script", true);
	ShutdownFn();
	Step("vh_shutdown", true);

	return RunOk ? 0 : 1;
}
