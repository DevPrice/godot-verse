// Asks the Verse compiler a question directly: compiles whatever .verse files are named on the
// command line as one project, prints every diagnostic, and then instantiates a class and calls its
// zero-argument methods so a side effect is observable rather than inferred.
//
// It exists because a language question ("can a struct have methods?", "does a two-parameter
// function satisfy a tuple-parameter callback?", "is a leading-underscore method name legal?") is
// cheaper to put to the compiler than to read out of SemanticAnalyzer.cpp, and every answer read
// rather than run has to be taken on trust. Six of Phase 4's design decisions were settled this way
// in one sitting; docs/phase-4-design.md 1.3 and 2 are what came out of it.
//
// Not a test, and deliberately not in run_tests.py: it has no expected output and asserts nothing.
// It is the same kind of thing as host_bench -- a way of taking a reading.
//
//   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
//       path/to/probe.verse [more.verse ...] [--class name] [--module path]
//
// Run it from bin/, where build_host.py leaves tbbmalloc.dll: verse_host.dll needs it, and
// LoadLibrary answers a bare 126 when it is missing rather than naming what it could not find.
//
// A class is a script only if it derives from `object` and is named after its file, so a probe file
// wanting --class must be named for the class it declares.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "verse_host_abi.h"

static void ProbePrint(void*, const char* Utf8, int32_t Len)
{
	fwrite(Utf8, 1, static_cast<size_t>(Len), stdout);
	fputc('\n', stdout);
}

/* There is no Godot behind this driver, so every handle is dead and every call on one must say so.
 * A probe that wants to reach Godot wants the integration project instead. */
static vh_bool ProbeIsValid(void*, vh_handle)
{
	return 0;
}

static int32_t ProbeGetProperty(void*, vh_handle, const char*, int32_t, vh_arena*, vh_value*)
{
	return VH_CALL_DEAD_OBJECT;
}

static int32_t ProbeSetProperty(void*, vh_handle, const char*, int32_t, const vh_value*)
{
	return VH_CALL_DEAD_OBJECT;
}

static int32_t ProbeCallMethod(void*, vh_handle, const char*, int32_t, const vh_value*, int32_t, vh_arena*, vh_value*)
{
	return VH_CALL_DEAD_OBJECT;
}

/* R-NODE-3: `helper{}` asks Godot for an object, and there is no Godot here. A fresh fake id per
 * ask, and the class printed, is enough to make the question "what class did the mirror walk up
 * to?" one the probe can answer -- and without these a probe fixture that constructs anything at
 * all raises instead of running. */
static int64_t NextProbeHandle = 5000;

static vh_handle ProbeInstantiateClass(void*, const char* ClassUtf8, int32_t ClassLen)
{
	printf("[probe]   instantiate %.*s -> %lld\n", ClassLen, ClassUtf8, (long long)NextProbeHandle);
	return NextProbeHandle++;
}

static void ProbeReleaseObject(void*, vh_handle Handle, vh_bool Discard)
{
	printf("[probe]   release %lld%s\n", (long long)Handle, Discard != 0 ? " (discarded)" : "");
}

static int ErrorCount = 0;

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

static void ProbeOnDiagnostic(void*, const vh_diagnostic* Diagnostic)
{
	if (Diagnostic->Severity == VH_SEVERITY_ERROR)
	{
		++ErrorCount;
	}
	/* The reference code as well as the message, because that is the half a recogniser in
	 * verse_script_language.cpp should match on: the message is English and is not ours, and the
	 * code is uLang's own glitch id from Glitch.h. Phase 4.5 needed it and had to add it. */
	printf("[probe] %.*s:%d:%d: %s %d: %.*s\n",
		   Diagnostic->FilePathLen, Diagnostic->FilePathUtf8,
		   Diagnostic->Line, Diagnostic->Column,
		   SeverityName(Diagnostic->Severity), Diagnostic->ReferenceCode,
		   Diagnostic->MessageLen, Diagnostic->MessageUtf8);
}

static void ProbeOnRuntimeError(void*, const vh_runtime_error* Error)
{
	printf("[probe] runtime error: %.*s\n", Error->MessageLen, Error->MessageUtf8);
	for (int32_t Index = 0; Index < Error->FrameCount; ++Index)
	{
		const vh_stack_frame& Frame = Error->Frames[Index];
		printf("[probe]   %.*s  %.*s:%d\n",
			   Frame.FunctionLen, Frame.FunctionUtf8,
			   Frame.PathLen, Frame.PathUtf8, Frame.Line);
	}
}

/* Never freed: the process is about to exit, and a bump allocator that outlives every call is one
 * fewer thing between a question and its answer. */
static void* ProbeAlloc(vh_arena*, size_t Size, size_t Align)
{
	return _aligned_malloc(Size ? Size : 1, Align ? Align : 8);
}

template <typename Fn>
static Fn Resolve(HMODULE Module, const char* Name)
{
	FARPROC Proc = GetProcAddress(Module, Name);
	if (!Proc)
	{
		printf("[probe] verse_host.dll exports no %s -- is it older than include/verse_host_abi.h?\n", Name);
		ExitProcess(2);
	}
	return reinterpret_cast<Fn>(Proc);
}

static void PrintValue(const vh_value& Value)
{
	switch (Value.Type)
	{
	case VH_TYPE_VOID: printf("void"); break;
	case VH_TYPE_LOGIC: printf("logic %s", Value.Logic ? "true" : "false"); break;
	case VH_TYPE_INT: printf("int %lld", static_cast<long long>(Value.Int)); break;
	case VH_TYPE_FLOAT: printf("float %f", Value.Float); break;
	case VH_TYPE_CHAR: printf("char %u", Value.Char); break;
	case VH_TYPE_STRING: printf("string \"%.*s\"", Value.String.Len, Value.String.Utf8); break;
	case VH_TYPE_REF: printf("ref %lld", static_cast<long long>(Value.Ref)); break;
	case VH_TYPE_TUPLE:
	case VH_TYPE_ARRAY:
		printf(Value.Type == VH_TYPE_TUPLE ? "tuple(" : "array(");
		for (int32_t Index = 0; Index < Value.Seq.Count; ++Index)
		{
			if (Index > 0)
			{
				printf(", ");
			}
			PrintValue(Value.Seq.Items[Index]);
		}
		printf(")");
		break;
	default: printf("vh_type %d", Value.Type); break;
	}
}

int main(int argc, char** argv)
{
	if (argc < 4)
	{
		printf("usage: verse_probe.exe <verse_host.dll> <EngineDir> <file.verse>... "
			   "[--class name] [--module path]\n");
		return 2;
	}

	const char* DllPath = argv[1];
	const char* EngineDir = argv[2];

	// --module applies to the files named after it, so a probe can put two files in two modules and
	// ask what that does to a name -- which is the question submodules exist to answer.
	std::vector<std::string> Files;
	std::vector<std::string> Modules;
	std::string ClassName;
	std::string Module;
	for (int Index = 3; Index < argc; ++Index)
	{
		if (strcmp(argv[Index], "--class") == 0 && Index + 1 < argc)
		{
			ClassName = argv[++Index];
			continue;
		}
		if (strcmp(argv[Index], "--module") == 0 && Index + 1 < argc)
		{
			Module = argv[++Index];
			continue;
		}
		Files.push_back(argv[Index]);
		Modules.push_back(Module);
	}

	HMODULE Module_ = LoadLibraryA(DllPath);
	if (!Module_)
	{
		printf("[probe] LoadLibrary(%s) failed: %lu. Error 126 means a dependency is missing -- run "
			   "from bin/, where tbbmalloc.dll is.\n",
			   DllPath, GetLastError());
		return 2;
	}

	auto InitFn = Resolve<vh_init_fn>(Module_, "vh_init");
	auto ShutdownFn = Resolve<vh_shutdown_fn>(Module_, "vh_shutdown");
	auto CompileFn = Resolve<vh_compile_project_fn>(Module_, "vh_compile_project");
	auto HasClassFn = Resolve<vh_has_class_fn>(Module_, "vh_has_class");
	auto MethodListFn = Resolve<vh_class_method_list_fn>(Module_, "vh_class_method_list");
	auto SignalListFn = Resolve<vh_class_signal_list_fn>(Module_, "vh_class_signal_list");
	auto InstantiateFn = Resolve<vh_instantiate_fn>(Module_, "vh_instantiate");
	auto ReleaseFn = Resolve<vh_release_instance_fn>(Module_, "vh_release_instance");
	auto CallFn = Resolve<vh_instance_call_fn>(Module_, "vh_instance_call");
	auto TickFn = Resolve<vh_tick_fn>(Module_, "vh_tick");

	vh_init_desc Desc{};
	Desc.StructSize = sizeof(Desc);
	Desc.AbiVersion = VH_ABI_VERSION;
	Desc.EngineDirUtf8 = EngineDir;
	Desc.Godot.StructSize = sizeof(Desc.Godot);
	Desc.Godot.Print = &ProbePrint;
	Desc.Godot.IsValid = &ProbeIsValid;
	Desc.Godot.GetProperty = &ProbeGetProperty;
	Desc.Godot.SetProperty = &ProbeSetProperty;
	Desc.Godot.CallMethod = &ProbeCallMethod;
	Desc.Godot.InstantiateClass = &ProbeInstantiateClass;
	Desc.Godot.ReleaseObject = &ProbeReleaseObject;
	Desc.OnDiagnostic = &ProbeOnDiagnostic;
	Desc.OnRuntimeError = &ProbeOnRuntimeError;

	if (InitFn(&Desc) != VH_OK)
	{
		printf("[probe] vh_init failed -- ABI %d against a host built from a different header?\n", VH_ABI_VERSION);
		return 2;
	}

	std::vector<vh_source_file> Sources;
	Sources.reserve(Files.size());
	for (size_t Index = 0; Index < Files.size(); ++Index)
	{
		vh_source_file Source{};
		Source.PathUtf8 = Files[Index].c_str();
		Source.ModulePathUtf8 = Modules[Index].empty() ? nullptr : Modules[Index].c_str();
		Sources.push_back(Source);
	}

	int32_t Generation = 0;
	const int32_t Status = CompileFn(Sources.data(), static_cast<int32_t>(Sources.size()), &Generation);
	printf("[probe] compile: status %d, generation %d, %d error(s)\n", Status, Generation, ErrorCount);

	if (Status == VH_OK && !ClassName.empty())
	{
		printf("[probe] vh_has_class(%s): %d\n", ClassName.c_str(), static_cast<int>(HasClassFn(ClassName.c_str())));

		// The signal list before the methods, because the question it answers is usually about a
		// *declaration* rather than about a call: a payload that decomposes into no arguments is
		// what an unread type-variable substitution looks like, and it is indistinguishable from
		// `event(tuple())` unless the fixture says which it meant.
		const vh_signal_desc* Signals = nullptr;
		int32_t SignalCount = 0;
		if (SignalListFn(ClassName.c_str(), &Signals, &SignalCount) == VH_OK)
		{
			printf("[probe] %d signal(s)\n", SignalCount);
			for (int32_t Index = 0; Index < SignalCount; ++Index)
			{
				const vh_signal_desc& Signal = Signals[Index];
				printf("[probe]   %.*s  args=%d  reject=%d%s%.*s\n",
					   Signal.NameLen, Signal.NameUtf8,
					   Signal.ArgCount, Signal.Reject,
					   Signal.RejectDetailLen > 0 ? " detail=" : "",
					   Signal.RejectDetailLen, Signal.RejectDetailUtf8);
				for (int32_t ArgIndex = 0; ArgIndex < Signal.ArgCount; ++ArgIndex)
				{
					const vh_param_desc& Arg = Signal.Args[ArgIndex];
					printf("[probe]     arg %d: %.*s type=%d tag=%d\n",
						   ArgIndex, Arg.NameLen, Arg.NameUtf8, Arg.Type, Arg.VariantTag);
				}
			}
		}

		const vh_method_desc* Methods = nullptr;
		int32_t MethodCount = 0;
		if (MethodListFn(ClassName.c_str(), &Methods, &MethodCount) != VH_OK)
		{
			printf("[probe] no method list for %s\n", ClassName.c_str());
		}
		else
		{
			printf("[probe] %d method(s)\n", MethodCount);
			for (int32_t Index = 0; Index < MethodCount; ++Index)
			{
				const vh_method_desc& Method = Methods[Index];
				printf("[probe]   %.*s  params=%d result=%d%s%s  virtual=%.*s\n",
					   Method.NameLen, Method.NameUtf8,
					   Method.ParamCount, Method.ResultType,
					   Method.CanFail ? " <decides>" : "",
					   Method.Suspends ? " <suspends>" : "",
					   Method.GodotVirtualLen, Method.GodotVirtualUtf8);
			}

			// Handle 7 is a lie the whole way down -- IsValid says every handle is dead -- so a
			// method that reaches Godot raises rather than answering. That is the boundary of what
			// this driver can ask.
			vh_instance* Instance = nullptr;
			if (InstantiateFn(ClassName.c_str(), 7, &Instance) != VH_OK || Instance == nullptr)
			{
				printf("[probe] vh_instantiate(%s) failed\n", ClassName.c_str());
			}
			else
			{
				for (int32_t Index = 0; Index < MethodCount; ++Index)
				{
					const vh_method_desc& Method = Methods[Index];
					if (Method.ParamCount != 0 || Method.GodotVirtualLen > 0 || Method.Suspends)
					{
						continue;
					}
					const std::string Decorated(Method.DecoratedUtf8, Method.DecoratedLen);
					vh_arena Arena{};
					Arena.Alloc = &ProbeAlloc;
					vh_value Result{};
					const int32_t CallStatus = CallFn(Instance, Decorated.c_str(), nullptr, 0, &Arena, &Result);
					printf("[probe] call %.*s: status %d, ", Method.NameLen, Method.NameUtf8, CallStatus);
					PrintValue(Result);
					printf("\n");
					// A raise stops every script until the next tick, so a probe with several
					// methods needs the frame boundary between them or the rest report VH_ERR_HALTED.
					TickFn(0.01, nullptr);
				}
				ReleaseFn(Instance);
			}
		}
	}

	ShutdownFn();
	return Status == VH_OK ? 0 : 1;
}
