// Asks a *runtime* host what an exported game would see: mounts a cooked directory, names a class
// and calls its zero-argument methods. No Godot, no export, no Godot editor -- so a wall in the
// cooked path is ten seconds away rather than a two-minute export and a game that dies with a
// stack and no output.
//
// It is the cooked-path twin of tests/verse_probe, and like it is deliberately not in
// run_tests.py: it asserts nothing and has no expected output. What it answers is "does this
// actually run", which is a reading rather than a test.
//
//   bin/cooked_probe.exe <engine>/Engine/Binaries/Win64/verse_host_runtime.dll
//       <cooked data dir> <cooked data dir>/Cooked [class]
//
// The second argument is the *data* directory an export ships -- it is the host's engine directory
// too (7a D7) -- and the third is the directory the containers are in, which is what
// vh_init_desc.CookedDirUtf8 names. The class is module-qualified: `gameplay/player`, not `player`.
//
// Run it from bin/, where build_host.py leaves tbbmalloc.dll.
//
// What it found: a script's own Verse runs, and the first call into the *mirror* takes the process
// down -- see docs/phase-7b-design.md §13.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "verse_host_abi.h"

static void ProbePrint(void*, const char* Utf8, int32_t Len)
{
	printf("[verse] %.*s\n", Len, Utf8);
}

static vh_bool ProbeIsValid(void*, vh_handle) { return 1; }
static int32_t ProbeGetProperty(void*, vh_handle, const char*, int32_t, vh_arena*, vh_value*) { return VH_CALL_DEAD_OBJECT; }
static int32_t ProbeSetProperty(void*, vh_handle, const char*, int32_t, const vh_value*) { return VH_CALL_DEAD_OBJECT; }
static int32_t ProbeCallMethod(void*, vh_handle, const char* N, int32_t L, const vh_value*, int32_t, vh_arena*, vh_value* Out)
{
	printf("[godot] call %.*s\n", L, N);
	Out->Type = VH_TYPE_VOID;
	return VH_CALL_OK;
}
static vh_handle ProbeGetSingleton(void*, const char*, int32_t) { return vh_handle{}; }
static int32_t ProbeGetClassOf(void*, vh_handle, vh_arena*, vh_value*) { return VH_CALL_DEAD_OBJECT; }
static vh_bool ProbeDebugShouldBreak(void*, const char*, int32_t, int32_t, int32_t) { return 0; }
static void ProbeDebugBreak(void*) {}

static void ProbeOnDiagnostic(void*, const vh_diagnostic* D)
{
	printf("[diag] %.*s\n", D->MessageLen, D->MessageUtf8);
}

static void ProbeOnRuntimeError(void*, const vh_runtime_error* E)
{
	printf("[error] %.*s\n", E->MessageLen, E->MessageUtf8);
}

// A bump allocator over one static block, which is all a probe needs.
static unsigned char GArenaBlock[1 << 20];
static size_t GArenaUsed = 0;

static void* ProbeAlloc(vh_arena*, size_t Size, size_t Align)
{
	GArenaUsed = (GArenaUsed + Align - 1) & ~(Align - 1);
	void* const Result = GArenaBlock + GArenaUsed;
	GArenaUsed += Size;
	return GArenaUsed <= sizeof(GArenaBlock) ? Result : nullptr;
}

static bool Step(const char* Name, bool Ok)
{
	printf("[probe] %s: %s\n", Name, Ok ? "ok" : "FAIL");
	return Ok;
}

template <typename Fn>
static Fn Resolve(HMODULE Module, const char* Name)
{
	FARPROC Proc = GetProcAddress(Module, Name);
	if (!Proc)
	{
		printf("[probe] resolve %s: FAIL\n", Name);
	}
	return reinterpret_cast<Fn>(Proc);
}

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc < 4)
	{
		printf("usage: cooked_probe <runtime host dll> <engine dir> <cooked dir> [class]\n");
		return 2;
	}

	const std::string DllPath = argv[1];
	const std::string EngineDir = argv[2];
	const std::string CookedDir = argv[3];
	const char* ClassName = argc > 4 ? argv[4] : "exports";

	const std::wstring DllPathW(DllPath.begin(), DllPath.end());
	HMODULE Module = LoadLibraryExW(DllPathW.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!Step("LoadLibraryW", Module != nullptr))
	{
		printf("[probe] GetLastError=%lu\n", GetLastError());
		return 1;
	}

	auto HostKindFn = Resolve<vh_host_kind_fn>(Module, "vh_host_kind");
	auto InitFn = Resolve<vh_init_fn>(Module, "vh_init");
	auto ShutdownFn = Resolve<vh_shutdown_fn>(Module, "vh_shutdown");
	auto HasClassFn = Resolve<vh_has_class_fn>(Module, "vh_has_class");
	auto InstantiateFn = Resolve<vh_instantiate_fn>(Module, "vh_instantiate");
	auto InstanceCallFn = Resolve<vh_instance_call_fn>(Module, "vh_instance_call");
	auto ClassMethodListFn = Resolve<vh_class_method_list_fn>(Module, "vh_class_method_list");
	auto ReleaseInstanceFn = Resolve<vh_release_instance_fn>(Module, "vh_release_instance");
	if (!InitFn || !ShutdownFn || !HasClassFn || !InstantiateFn || !InstanceCallFn || !ClassMethodListFn)
	{
		return 1;
	}

	printf("[probe] vh_host_kind = %d\n", HostKindFn ? HostKindFn() : -1);

	vh_init_desc Desc{};
	Desc.StructSize = sizeof(Desc);
	Desc.AbiVersion = VH_ABI_VERSION;
	Desc.EngineDirUtf8 = EngineDir.c_str();
	Desc.Godot.StructSize = sizeof(Desc.Godot);
	Desc.Godot.Print = &ProbePrint;
	Desc.Godot.IsValid = &ProbeIsValid;
	Desc.Godot.GetProperty = &ProbeGetProperty;
	Desc.Godot.SetProperty = &ProbeSetProperty;
	Desc.Godot.CallMethod = &ProbeCallMethod;
	Desc.Godot.GetSingleton = &ProbeGetSingleton;
	Desc.Godot.GetClassOf = &ProbeGetClassOf;
	Desc.Godot.DebugShouldBreak = &ProbeDebugShouldBreak;
	Desc.Godot.DebugBreak = &ProbeDebugBreak;
	Desc.OnDiagnostic = &ProbeOnDiagnostic;
	Desc.OnRuntimeError = &ProbeOnRuntimeError;
	Desc.CookedDirUtf8 = CookedDir.c_str();

	const int32_t InitStatus = InitFn(&Desc);
	if (!Step("vh_init", InitStatus == VH_OK))
	{
		printf("[probe] vh_init returned %d\n", InitStatus);
		return 1;
	}

	Step("vh_has_class", HasClassFn(ClassName) != 0);

	// Copied out, because the list points into storage the host owns only until its next call.
	std::vector<std::string> Callable;
	const vh_method_desc* Methods = nullptr;
	int32_t MethodCount = 0;
	if (ClassMethodListFn(ClassName, &Methods, &MethodCount) == VH_OK)
	{
		printf("[probe] %s has %d method(s)\n", ClassName, MethodCount);
		for (int32_t Index = 0; Index < MethodCount; ++Index)
		{
			if (Methods[Index].ParamCount == 0 && !Methods[Index].Suspends)
			{
				Callable.push_back(Methods[Index].DecoratedUtf8);
			}
		}
	}

	vh_instance* Instance = nullptr;
	const int32_t Status = InstantiateFn(ClassName, (vh_handle)4242, &Instance);
	if (Step("vh_instantiate", Status == VH_OK && Instance != nullptr))
	{
		vh_arena Arena{};
		Arena.Alloc = &ProbeAlloc;
		for (const std::string& Name : Callable)
		{
			GArenaUsed = 0;
			printf("[probe] calling %s\n", Name.c_str());
			vh_value Result{};
			const int32_t CallStatus =
				InstanceCallFn(Instance, Name.c_str(), nullptr, 0, &Arena, &Result);
			printf("[probe]   -> %d\n", CallStatus);
		}
		ReleaseInstanceFn(Instance);
	}
	else
	{
		printf("[probe] vh_instantiate returned %d\n", Status);
	}

	ShutdownFn();
	printf("[probe] done\n");
	return 0;
}
