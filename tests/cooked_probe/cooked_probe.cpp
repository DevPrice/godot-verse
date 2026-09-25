// Asks a *runtime* host what an exported game would see: mounts a cooked directory, names a class
// and calls its zero-argument methods. No Godot, no export, no Godot editor -- so a wall in the
// cooked path is ten seconds away rather than a two-minute export and a game that dies with a
// stack and no output.
//
// It is the cooked-path twin of tests/verse_probe, and like it asserts nothing and has no expected
// output of its own: what it answers is "does this actually run". The callers that assert are
// tools/run_vm_conformance.py, which diffs its transcripts, and tools/run_tests.py's cook case,
// which reads its output for the lines a runtime host must print.
//
//   bin/cooked_probe.exe <engine>/Engine/Binaries/Win64/verse_host_runtime.dll
//       <cooked data dir> <cooked data dir>/Cooked [class [--frames]]
//
// --frames prints each runtime error's frames, one `[frame] path | function | line` per frame.
//
// --bench <repeats> times instead (tools/run_vm_bench.py): each method is called once to warm up,
// then <repeats> times, and each call and the vh_tick after it are timed apart, because the
// interpreter collects only in vh_tick and a call that allocates pays for it there. Nothing is
// printed per call, and `get_position` answers a vector2 so a property read converts a real value.
//
// The second argument is the *data* directory an export ships -- it is the host's engine directory
// too (7a D7) -- and the third is the directory the containers are in, which is what
// vh_init_desc.CookedDirUtf8 names. The class is module-qualified: `gameplay/player`, not `player`.
//
// Run it from bin/, where build_host.py leaves tbbmalloc.dll.
//
// What it found: a script's own Verse runs, and the first call into the *mirror* takes the process
// down -- see docs/phase-7b-design.md §13.
//
// Built with COOKED_PROBE_STATIC, it links vm/ in instead of loading a DLL, which is how
// tools/run_vm_bench.py --wasm runs the interpreter as WebAssembly under Node; the first argument is
// then ignored.
#if !defined(COOKED_PROBE_STATIC)
#include <windows.h>

#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")
#define COOKED_PROBE_SAMPLER 1
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "verse_host_abi.h"

static bool GBench = false;

static void ProbePrint(void*, const char* Utf8, int32_t Len)
{
	if (!GBench)
	{
		printf("[verse] %.*s\n", Len, Utf8);
	}
}

static vh_value GBenchVector2Items[2] = {};

static bool NameIs(const char* Name, int32_t Len, const char* Expected)
{
	return int32_t(std::strlen(Expected)) == Len && std::memcmp(Name, Expected, Len) == 0;
}

static vh_bool ProbeIsValid(void*, vh_handle) { return 1; }
static int32_t ProbeGetProperty(void*, vh_handle, const char*, int32_t, vh_arena*, vh_value*) { return VH_CALL_DEAD_OBJECT; }
static int32_t ProbeSetProperty(void*, vh_handle, const char*, int32_t, const vh_value*) { return VH_CALL_DEAD_OBJECT; }
static int32_t ProbeCallMethod(void*, vh_handle, const char* N, int32_t L, const vh_value*, int32_t, vh_arena*, vh_value* Out)
{
	if (!GBench)
	{
		printf("[godot] call %.*s\n", L, N);
	}
	if (Out == nullptr)
	{
		return VH_CALL_OK;
	}
	if (GBench && NameIs(N, L, "get_position"))
	{
		Out->Type = VH_TYPE_TUPLE;
		Out->VariantTag = VH_VARIANT_VECTOR2;
		Out->Seq.Items = GBenchVector2Items;
		Out->Seq.Count = 2;
		return VH_CALL_OK;
	}
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

// Off by default: tools/run_vm_conformance.py diffs a transcript line for line against an
// interpreter that reports frames its own way, so only a caller that asks gets them.
static bool GPrintFrames = false;

static void ProbeOnRuntimeError(void*, const vh_runtime_error* E)
{
	printf("[error] %.*s\n", E->MessageLen, E->MessageUtf8);
	if (!GPrintFrames)
	{
		return;
	}
	printf("[error] %d frame(s)\n", E->FrameCount);
	for (int32_t Index = 0; Index < E->FrameCount; ++Index)
	{
		const vh_stack_frame& F = E->Frames[Index];
		printf("[frame] %.*s | %.*s | %d\n", F.PathLen, F.PathUtf8, F.FunctionLen, F.FunctionUtf8, F.Line);
	}
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

// Named alongside the code so a transcript reads without include/verse_host_abi.h open beside it;
// the differential harness (tools/run_vm_conformance.py) diffs this line as-is between hosts.
static const char* StatusName(int32_t Status)
{
	switch (Status)
	{
		case VH_OK: return "VH_OK";
		case VH_ERR_ABI: return "VH_ERR_ABI";
		case VH_ERR_STATE: return "VH_ERR_STATE";
		case VH_ERR_INIT: return "VH_ERR_INIT";
		case VH_ERR_COMPILE: return "VH_ERR_COMPILE";
		case VH_ERR_NOT_FOUND: return "VH_ERR_NOT_FOUND";
		case VH_ERR_RUNTIME: return "VH_ERR_RUNTIME";
		case VH_ERR_ARGUMENT: return "VH_ERR_ARGUMENT";
		case VH_ERR_FAILED: return "VH_ERR_FAILED";
		case VH_ERR_HALTED: return "VH_ERR_HALTED";
		case VH_ERR_THREAD: return "VH_ERR_THREAD";
		case VH_ERR_STOPPED: return "VH_ERR_STOPPED";
		case VH_ERR_UNSUPPORTED: return "VH_ERR_UNSUPPORTED";
		default: return "?";
	}
}

#if !defined(COOKED_PROBE_STATIC)
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
#endif

// --sample, after --bench: a second thread suspends the benchmarking one about every millisecond,
// walks its stack with DbgHelp and counts each function once as the leaf (self) and once per sample
// it appears in (inclusive). A sampling profiler that needs no elevation and no Visual Studio UI,
// which is what docs/vm-performance.md §1.5 asks of the native build; the DLL's PDB names the frames.
#if defined(COOKED_PROBE_SAMPLER)
class Sampler
{
public:
	void Start(HANDLE Thread)
	{
		Target = Thread;
		Self.clear();
		Inclusive.clear();
		Samples = 0;
		Running = true;
		Worker = CreateThread(nullptr, 0, &Sampler::Run, this, 0, nullptr);
	}

	void Stop(const std::string& Label)
	{
		Running = false;
		WaitForSingleObject(Worker, INFINITE);
		CloseHandle(Worker);
		Report(Label, "self", Self);
		Report(Label, "incl", Inclusive);
	}

private:
	static DWORD WINAPI Run(void* Param)
	{
		static_cast<Sampler*>(Param)->Loop();
		return 0;
	}

	// Nothing between SuspendThread and ResumeThread may allocate or take a lock the target could
	// hold -- the heap's, above all -- so the suspended half only unwinds into a fixed array, with
	// RtlVirtualUnwind over the images' own unwind tables, and names are looked up after.
	void Loop()
	{
		HANDLE Process = GetCurrentProcess();
		constexpr int MaxDepth = 48;
		DWORD64 Addresses[MaxDepth];
		while (Running)
		{
			Sleep(1);
			if (SuspendThread(Target) == DWORD(-1))
			{
				continue;
			}
			int Depth = 0;
			CONTEXT Context{};
			Context.ContextFlags = CONTEXT_FULL;
			if (GetThreadContext(Target, &Context))
			{
				while (Depth < MaxDepth && Context.Rip != 0)
				{
					Addresses[Depth++] = Context.Rip;
					DWORD64 ImageBase = 0;
					PRUNTIME_FUNCTION Function = RtlLookupFunctionEntry(Context.Rip, &ImageBase, nullptr);
					if (Function == nullptr)
					{
						Context.Rip = *reinterpret_cast<DWORD64*>(Context.Rsp);
						Context.Rsp += 8;
						continue;
					}
					void* HandlerData = nullptr;
					DWORD64 EstablisherFrame = 0;
					RtlVirtualUnwind(UNW_FLAG_NHANDLER, ImageBase, Context.Rip, Function, &Context, &HandlerData,
						&EstablisherFrame, nullptr);
				}
			}
			ResumeThread(Target);
			if (Depth == 0)
			{
				continue;
			}
			std::vector<const std::string*> Seen;
			for (int Index = 0; Index < Depth; ++Index)
			{
				const std::string& Name = SymbolName(Process, Addresses[Index]);
				if (Index == 0)
				{
					++Self[Name];
				}
				if (std::find_if(Seen.begin(), Seen.end(), [&](const std::string* Other) { return *Other == Name; }) == Seen.end())
				{
					Seen.push_back(&Name);
					++Inclusive[Name];
				}
			}
			++Samples;
		}
	}

	const std::string& SymbolName(HANDLE Process, DWORD64 Address)
	{
		auto Cached = Names.find(Address);
		if (Cached != Names.end())
		{
			return Cached->second;
		}
		return Names.emplace(Address, LookUp(Process, Address)).first->second;
	}

	static std::string LookUp(HANDLE Process, DWORD64 Address)
	{
		alignas(SYMBOL_INFO) char Buffer[sizeof(SYMBOL_INFO) + 256];
		SYMBOL_INFO* Symbol = reinterpret_cast<SYMBOL_INFO*>(Buffer);
		Symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
		Symbol->MaxNameLen = 255;
		IMAGEHLP_MODULE64 Module{};
		Module.SizeOfStruct = sizeof(Module);
		const std::string ModuleName = SymGetModuleInfo64(Process, Address, &Module) ? Module.ModuleName : "?";
		if (SymFromAddr(Process, Address, nullptr, Symbol))
		{
			return ModuleName + "!" + Symbol->Name;
		}
		return ModuleName + "!?";
	}

	void Report(const std::string& Label, const char* Kind, const std::map<std::string, int>& Counts) const
	{
		std::vector<std::pair<int, std::string>> Sorted;
		for (const auto& [Name, Count] : Counts)
		{
			Sorted.emplace_back(Count, Name);
		}
		std::sort(Sorted.rbegin(), Sorted.rend());
		for (size_t Index = 0; Index < Sorted.size() && Index < 15; ++Index)
		{
			printf("[sample] %s %s %5.1f%% %s\n", Label.c_str(), Kind,
				Samples ? 100.0 * Sorted[Index].first / Samples : 0.0, Sorted[Index].second.c_str());
		}
	}

	HANDLE Target = nullptr;
	HANDLE Worker = nullptr;
	std::atomic<bool> Running = false;
	std::map<std::string, int> Self;
	std::map<std::string, int> Inclusive;
	std::map<DWORD64, std::string> Names;
	int Samples = 0;
};

static Sampler* GSampler = nullptr;
#endif

static double Median(std::vector<double> Samples)
{
	std::sort(Samples.begin(), Samples.end());
	return Samples[Samples.size() / 2];
}

// One line per method: `[bench] <method> status=<s> call_min_us=.. call_median_us=.. tick_median_us=..`.
static void Bench(const std::string& Name, int32_t Repeats, vh_instance* Instance,
	vh_instance_call_fn InstanceCallFn, vh_tick_fn TickFn, vh_arena& Arena)
{
	using Clock = std::chrono::steady_clock;
	int32_t Status = VH_OK;
	auto CallOnce = [&]
	{
		GArenaUsed = 0;
		vh_value Result{};
		Status = InstanceCallFn(Instance, Name.c_str(), nullptr, 0, &Arena, &Result);
	};
	CallOnce();
	TickFn(0.0, nullptr);

	std::vector<double> Calls;
	std::vector<double> Ticks;
	Calls.reserve(Repeats);
	Ticks.reserve(Repeats);
#if defined(COOKED_PROBE_SAMPLER)
	if (GSampler != nullptr)
	{
		HANDLE Self = nullptr;
		DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &Self, 0, FALSE,
			DUPLICATE_SAME_ACCESS);
		GSampler->Start(Self);
	}
#endif
	for (int32_t Index = 0; Index < Repeats; ++Index)
	{
		const Clock::time_point Start = Clock::now();
		CallOnce();
		const Clock::time_point Called = Clock::now();
		TickFn(0.0, nullptr);
		const Clock::time_point Ticked = Clock::now();
		Calls.push_back(std::chrono::duration<double, std::micro>(Called - Start).count());
		Ticks.push_back(std::chrono::duration<double, std::micro>(Ticked - Called).count());
	}
#if defined(COOKED_PROBE_SAMPLER)
	if (GSampler != nullptr)
	{
		const size_t Paren = Name.rfind(')');
		GSampler->Stop(Paren == std::string::npos ? Name : Name.substr(Paren + 1));
	}
#endif
	printf("[bench] %s status=%s call_min_us=%.2f call_median_us=%.2f tick_median_us=%.2f tick_max_us=%.2f\n",
		Name.c_str(), StatusName(Status), *std::min_element(Calls.begin(), Calls.end()), Median(Calls),
		Median(Ticks), *std::max_element(Ticks.begin(), Ticks.end()));
}

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc < 4)
	{
		printf("usage: cooked_probe <runtime host dll> <engine dir> <cooked dir> [class [--frames | --bench <repeats> [--sample]]]\n");
		return 2;
	}

	const std::string DllPath = argv[1];
	const std::string EngineDir = argv[2];
	const std::string CookedDir = argv[3];
	const char* ClassName = argc > 4 ? argv[4] : "exports";
	GPrintFrames = argc > 5 && std::strcmp(argv[5], "--frames") == 0;
	int32_t BenchRepeats = 0;
	if (argc > 6 && std::strcmp(argv[5], "--bench") == 0)
	{
		BenchRepeats = std::atoi(argv[6]);
		GBench = BenchRepeats > 0;
	}
	const bool Sample = GBench && argc > 7 && std::strcmp(argv[7], "--sample") == 0;
	for (int32_t Index = 0; Index < 2; ++Index)
	{
		GBenchVector2Items[Index].Type = VH_TYPE_FLOAT;
		GBenchVector2Items[Index].VariantTag = VH_VARIANT_FLOAT;
		GBenchVector2Items[Index].Float = 1.5 + Index;
	}

#if defined(COOKED_PROBE_STATIC)
	vh_host_kind_fn HostKindFn = &vh_host_kind;
	vh_init_fn InitFn = &vh_init;
	vh_shutdown_fn ShutdownFn = &vh_shutdown;
	vh_has_class_fn HasClassFn = &vh_has_class;
	vh_instantiate_fn InstantiateFn = &vh_instantiate;
	vh_instance_call_fn InstanceCallFn = &vh_instance_call;
	vh_class_method_list_fn ClassMethodListFn = &vh_class_method_list;
	vh_release_instance_fn ReleaseInstanceFn = &vh_release_instance;
	vh_tick_fn TickFn = &vh_tick;
#else
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
	auto TickFn = Resolve<vh_tick_fn>(Module, "vh_tick");
	if (!InitFn || !ShutdownFn || !HasClassFn || !InstantiateFn || !InstanceCallFn || !ClassMethodListFn)
	{
		return 1;
	}
#endif

	printf("[probe] vh_host_kind = %d\n", HostKindFn ? HostKindFn() : -1);

#if defined(COOKED_PROBE_SAMPLER)
	static Sampler BenchSampler;
	if (Sample)
	{
		timeBeginPeriod(1);
		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
		const std::string DllDir = DllPath.substr(0, DllPath.find_last_of("/\\"));
		SymInitialize(GetCurrentProcess(), DllDir.c_str(), TRUE);
		GSampler = &BenchSampler;
	}
#else
	(void)Sample;
#endif

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
		if (GBench)
		{
			if (TickFn == nullptr)
			{
				return 1;
			}
			for (const std::string& Name : Callable)
			{
				Bench(Name, BenchRepeats, Instance, InstanceCallFn, TickFn, Arena);
			}
			Callable.clear();
		}
		for (const std::string& Name : Callable)
		{
			GArenaUsed = 0;
			printf("[probe] calling %s\n", Name.c_str());
			vh_value Result{};
			const int32_t CallStatus =
				InstanceCallFn(Instance, Name.c_str(), nullptr, 0, &Arena, &Result);
			printf("[probe]   -> %d (%s)\n", CallStatus, StatusName(CallStatus));
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
