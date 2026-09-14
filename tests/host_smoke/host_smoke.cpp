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

static vh_bool SmokeIsValid(void*, vh_handle)
{
	return 0;
}

/* There is no Godot behind this harness, so every handle is dead -- which is what SmokeIsValid
 * reports, and these have to agree with it. Answering VH_CALL_OK (which is 0, and so is what a
 * stubbed-out `return 0` would now mean) would claim a successful call that wrote no value. */
static int32_t SmokeGetProperty(void*, vh_handle, const char*, int32_t, vh_arena*, vh_value*)
{
	return VH_CALL_DEAD_OBJECT;
}

static int32_t SmokeSetProperty(void*, vh_handle, const char*, int32_t, const vh_value*)
{
	return VH_CALL_DEAD_OBJECT;
}

static int32_t SmokeCallMethod(void*, vh_handle, const char*, int32_t, const vh_value*, int32_t, vh_arena*, vh_value*)
{
	return VH_CALL_DEAD_OBJECT;
}

static int32_t SmokeGetClassOf(void*, vh_handle, vh_arena*, vh_value*)
{
	return VH_CALL_DEAD_OBJECT;
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

static int DiagnosticCount = 0;
static int DiagnosticErrorCount = 0;
static int RuntimeErrorCount = 0;
static std::string LastRuntimeErrorPath;
static int LastRuntimeErrorLine = 0;
static std::string LastRuntimeErrorFunction;
static std::string LastRuntimeErrorStack;

/* A no-argument call that wants no result, which is what every lifecycle method is. */
static int32_t CallVoid(vh_instance_call_fn Call, vh_instance* Instance, const char* DecoratedName)
{
	return Call(Instance, DecoratedName, nullptr, 0, nullptr, nullptr);
}

static void SmokeOnRuntimeError(void*, const vh_runtime_error* Error)
{
	++RuntimeErrorCount;
	LastRuntimeErrorPath.clear();
	LastRuntimeErrorFunction.clear();
	LastRuntimeErrorStack.clear();
	LastRuntimeErrorLine = 0;
	for (int32_t Index = 0; Index < Error->FrameCount; ++Index)
	{
		const vh_stack_frame& Frame = Error->Frames[Index];
		LastRuntimeErrorStack.append(Frame.PathUtf8, static_cast<size_t>(Frame.PathLen));
		LastRuntimeErrorStack.append(" ");
		LastRuntimeErrorStack.append(Frame.FunctionUtf8, static_cast<size_t>(Frame.FunctionLen));
		LastRuntimeErrorStack.append("\n");

		/* The innermost frame carrying a location is where the error was raised, which for a call
		 * into the Godot mirror is the mirror's own line rather than the script's. The script's is
		 * further out, and the whole stack is what carries it. */
		if (LastRuntimeErrorLine == 0 && Frame.PathLen > 0 && Frame.Line > 0)
		{
			LastRuntimeErrorPath.assign(Frame.PathUtf8, static_cast<size_t>(Frame.PathLen));
			LastRuntimeErrorFunction.assign(Frame.FunctionUtf8, static_cast<size_t>(Frame.FunctionLen));
			LastRuntimeErrorLine = Frame.Line;
		}
	}
}

/// The file's bytes, for handing a buffer to the check entry points the way an editor would.
static std::string ReadFileUtf8(const fs::path& Path)
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

/// Writes a .verse fixture the test generates rather than ships. Generations exist to pick up an
/// edit, so a test of them has to be able to make one.
static bool WriteFileUtf8(const fs::path& Path, const std::string& Text)
{
	FILE* File = nullptr;
	if (fopen_s(&File, Path.string().c_str(), "wb") != 0 || !File)
	{
		return false;
	}
	const size_t Written = fwrite(Text.data(), 1, Text.size(), File);
	fclose(File);
	return Written == Text.size();
}

/// The generation fixture's text. Generation N reports N, so which generation an instance is
/// running is a question the instance itself can answer.
static std::string ReloadProbeSource(int Generation)
{
	return R"VERSE(using { /Godot.org/Godot }

# Declared at the project root, and read from inside a module below, which is what says root is
# implicit and needs no `using`.
ReloadProbeShared<public>():int = 10

reload_probe := class(object):
    Generation<public>():int = )VERSE"
		   + std::to_string(Generation) + "\n";
}

/// A second file, placed in a module rather than at the root. Its body names a root definition
/// with nothing imported, which is the other half of what the OQ-12 run has to confirm.
static std::string ModuleProbeSource()
{
	return R"VERSE(using { /Godot.org/Godot }

module_probe := class(object):
    Total<public>():int = ReloadProbeShared() + 5
)VERSE";
}

/// Prints the provenance record build_host.py leaves beside the DLL, so a test log says which
/// engine revision produced the host it just exercised. Absent when the DLL was copied by hand.
static void ReportProvenance(const fs::path& DllPath)
{
	fs::path Path = DllPath;
	Path.replace_filename("verse_host.build.txt");
	const std::string Text = ReadFileUtf8(Path);
	if (Text.empty())
	{
		printf("[smoke] provenance: none beside %ls\n", DllPath.filename().c_str());
		return;
	}
	size_t Start = 0;
	while (Start < Text.size())
	{
		size_t End = Text.find('\n', Start);
		if (End == std::string::npos)
		{
			End = Text.size();
		}
		size_t Length = End - Start;
		while (Length > 0 && Text[Start + Length - 1] == '\r')
		{
			Length--;
		}
		if (Length > 0)
		{
			printf("[smoke] provenance: %.*s\n", static_cast<int>(Length), Text.data() + Start);
		}
		Start = End + 1;
	}
}

static void SmokeOnDiagnostic(void*, const vh_diagnostic* Diagnostic)
{
	++DiagnosticCount;
	if (Diagnostic->Severity == VH_SEVERITY_ERROR)
	{
		++DiagnosticErrorCount;
	}
	fprintf(stderr, "%.*s:%d:%d: %s: %.*s\n",
		Diagnostic->FilePathLen, Diagnostic->FilePathUtf8,
		Diagnostic->Line, Diagnostic->Column,
		SeverityName(Diagnostic->Severity),
		Diagnostic->MessageLen, Diagnostic->MessageUtf8);
}

/// The zero-based row and byte-offset column of a byte offset, which is how the compiler counts
/// and so how vh_lookup_symbol is asked and answered.
static void RowColumnOf(const std::string& Text, size_t Offset, int32_t& OutRow, int32_t& OutColumn)
{
	OutRow = 0;
	size_t LineStart = 0;
	for (size_t i = 0; i < Offset && i < Text.size(); i++)
	{
		if (Text[i] == '\n')
		{
			OutRow++;
			LineStart = i + 1;
		}
	}
	OutColumn = static_cast<int32_t>(Offset - LineStart);
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
	fs::path ExportsPath = VerseBase / "tests" / "host_smoke" / "exports.verse";
	fs::path TasksPath = VerseBase / "tests" / "host_smoke" / "tasks.verse";

	ReportProvenance(DllPath);

	// LOAD_WITH_ALTERED_SEARCH_PATH: the host's own directory holds tbbmalloc.dll.
	HMODULE Module = LoadLibraryExW(DllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!Step("LoadLibraryW", Module != nullptr))
	{
		fprintf(stderr, "[smoke] could not load %ls (GetLastError=%lu)\n", DllPath.c_str(), GetLastError());
		return 1;
	}

	bool ResolveOk = true;
	auto AbiVersionFn = Resolve<vh_abi_version_fn>(Module, "vh_abi_version", &ResolveOk);
	auto InitFn = Resolve<vh_init_fn>(Module, "vh_init", &ResolveOk);
	auto ShutdownFn = Resolve<vh_shutdown_fn>(Module, "vh_shutdown", &ResolveOk);
	auto TickFn = Resolve<vh_tick_fn>(Module, "vh_tick", &ResolveOk);
	auto CompileProjectFn = Resolve<vh_compile_project_fn>(Module, "vh_compile_project", &ResolveOk);
	auto HasClassFn = Resolve<vh_has_class_fn>(Module, "vh_has_class", &ResolveOk);
	auto ClassExportListFn = Resolve<vh_class_export_list_fn>(Module, "vh_class_export_list", &ResolveOk);
	auto ClassDefaultFieldFn = Resolve<vh_class_default_field_fn>(Module, "vh_class_default_field", &ResolveOk);
	auto InstantiateFn = Resolve<vh_instantiate_fn>(Module, "vh_instantiate", &ResolveOk);
	auto ReleaseInstanceFn = Resolve<vh_release_instance_fn>(Module, "vh_release_instance", &ResolveOk);
	auto InstanceCallFn = Resolve<vh_instance_call_fn>(Module, "vh_instance_call", &ResolveOk);
	auto ClassMethodListFn = Resolve<vh_class_method_list_fn>(Module, "vh_class_method_list", &ResolveOk);
	auto GetFieldFn = Resolve<vh_instance_get_field_fn>(Module, "vh_instance_get_field", &ResolveOk);
	auto SetFieldFn = Resolve<vh_instance_set_field_fn>(Module, "vh_instance_set_field", &ResolveOk);
	auto SetFieldInstanceFn = Resolve<vh_instance_set_field_instance_fn>(Module, "vh_instance_set_field_instance", &ResolveOk);
	auto LookupSymbolFn = Resolve<vh_lookup_symbol_fn>(Module, "vh_lookup_symbol", &ResolveOk);
	auto CompleteSymbolFn = Resolve<vh_complete_symbol_fn>(Module, "vh_complete_symbol", &ResolveOk);
	auto ClassMembersFn = Resolve<vh_class_members_fn>(Module, "vh_class_members", &ResolveOk);
	auto SignatureAtFn = Resolve<vh_signature_at_fn>(Module, "vh_signature_at", &ResolveOk);
	auto CheckProjectFn = Resolve<vh_check_project_fn>(Module, "vh_check_project", &ResolveOk);
	auto ResolveUnknownNameFn = Resolve<vh_resolve_unknown_name_fn>(Module, "vh_resolve_unknown_name", &ResolveOk);
	auto CheckBeginFn = Resolve<vh_check_project_begin_fn>(Module, "vh_check_project_begin", &ResolveOk);
	auto CheckProjectPollFn = Resolve<vh_check_project_poll_fn>(Module, "vh_check_project_poll", &ResolveOk);
	auto CheckBusyFn = Resolve<vh_check_project_busy_fn>(Module, "vh_check_project_busy", &ResolveOk);
	auto RunMainFn = Resolve<vh_run_main_fn>(Module, "vh_run_main", &ResolveOk);
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
	Desc.Godot.IsValid = &SmokeIsValid;
	Desc.Godot.GetProperty = &SmokeGetProperty;
	Desc.Godot.SetProperty = &SmokeSetProperty;
	Desc.Godot.CallMethod = &SmokeCallMethod;
	Desc.Godot.GetClassOf = &SmokeGetClassOf;
	Desc.OnDiagnostic = &SmokeOnDiagnostic;
	Desc.DiagnosticCtx = nullptr;
	Desc.OnRuntimeError = &SmokeOnRuntimeError;
	Desc.RuntimeErrorCtx = nullptr;
	Desc.EnableDebugger = 0;

	if (!Step("vh_init", InitFn(&Desc) == VH_OK))
	{
		return 1;
	}

	// One call for every fixture: Verse builds a whole package at once, so every script that will
	// run has to be in the list.
	//
	// reload_probe is written rather than shipped, because the generation checks below need a file
	// whose text changes between builds. It is in generation 1 so an instance of it can outlive
	// generation 1.
	const fs::path ScratchDir = fs::temp_directory_path() / "godot_verse_smoke";
	std::error_code ScratchError;
	fs::create_directories(ScratchDir, ScratchError);
	const fs::path ReloadPath = ScratchDir / "reload_probe.verse";
	const fs::path ModuleProbePath = ScratchDir / "module_probe.verse";
	if (!Step("write the reload fixture", WriteFileUtf8(ReloadPath, ReloadProbeSource(1))))
	{
		ShutdownFn();
		return 1;
	}

	std::string VersePathUtf8 = VersePath.string();
	std::string ExportsPathUtf8 = ExportsPath.string();
	std::string ReloadPathUtf8 = ReloadPath.string();
	std::string ModuleProbePathUtf8 = ModuleProbePath.string();
	std::string TasksPathUtf8 = TasksPath.string();
	vh_source_file ProjectFiles[4] = {
		{ VersePathUtf8.c_str(), nullptr },
		{ ExportsPathUtf8.c_str(), nullptr },
		{ ReloadPathUtf8.c_str(), nullptr },
		{ TasksPathUtf8.c_str(), nullptr },
	};
	int32_t Generation = 0;
	if (!Step("vh_compile_project", CompileProjectFn(ProjectFiles, 4, &Generation) == VH_OK))
	{
		ShutdownFn();
		return 1;
	}
	Step("the first build is generation 1", Generation == 1);

	// Instantiated here rather than beside the generation checks below, because the point of it is
	// to be older than the second generation.
	vh_instance* GenerationOne = nullptr;
	Step("vh_instantiate against generation 1",
		 InstantiateFn("reload_probe", 3, &GenerationOne) == VH_OK && GenerationOne != nullptr);

	// Symbol lookup, asked before anything has edited a buffer. That is the state the editor is
	// in at startup, and it is the interesting one: the build just done generated code, which
	// hangs an IR package off every module and puts the AST the lookup walks out of reach. The
	// host is supposed to put the program back into an analysable shape on its own rather than
	// leave the first hover of a session unanswerable.
	bool LookupOk = true;
	{
		const std::string ExportsSource = ReadFileUtf8(ExportsPath);
		LookupOk = Step("read exports.verse", !ExportsSource.empty());

		// The Speed in `set Scale = Scale + Speed` is a reference; its definition is the
		// `Speed<public>:float` member declared far above it in the same file.
		//
		// A definition's source range starts at the first attribute applied to it rather than
		// at its name, so Speed's begins on its `@export` -- two lines above the name. That is
		// the first `@export` in the fixture, and it is the row a jump should land on.
		const size_t UseOffset = ExportsSource.find("+ Speed");
		const size_t DeclOffset = ExportsSource.find("@export");
		LookupOk = Step("located the fixture's Speed use and declaration",
					   UseOffset != std::string::npos && DeclOffset != std::string::npos)
				&& LookupOk;

		if (LookupOk)
		{
			int32_t UseRow = 0;
			int32_t UseColumn = 0;
			RowColumnOf(ExportsSource, UseOffset + 2, UseRow, UseColumn);
			int32_t DeclRow = 0;
			int32_t DeclColumn = 0;
			RowColumnOf(ExportsSource, DeclOffset, DeclRow, DeclColumn);

			auto Text = [](const char* Utf8, int32_t Len) { return std::string(Utf8 ? Utf8 : "", Len); };

			const vh_lookup_desc* Lookup = nullptr;
			LookupOk = Step("vh_lookup_symbol", LookupSymbolFn(ExportsPathUtf8.c_str(), UseRow, UseColumn, &Lookup) == VH_OK) && LookupOk;
			if (Lookup)
			{
				LookupOk = Step("the use resolves to Speed", Text(Lookup->NameUtf8, Lookup->NameLen) == "Speed") && LookupOk;
				LookupOk = Step("it points at the start of the declaration",
							   Lookup->Line == DeclRow && Lookup->Column == DeclColumn)
						&& LookupOk;
				LookupOk = Step("it points into exports.verse", Text(Lookup->PathUtf8, Lookup->PathLen) == ExportsPathUtf8) && LookupOk;
				LookupOk = Step("Speed is not a var", Lookup->IsVar == 0) && LookupOk;
				LookupOk = Step("Speed's type reads as float", Text(Lookup->TypeUtf8, Lookup->TypeLen) == "float") && LookupOk;
				LookupOk = Step("Speed is a data definition", Lookup->Kind == VH_LOOKUP_DATA) && LookupOk;
				LookupOk = Step("Speed's owner is the class it was declared in",
							   Text(Lookup->OwnerUtf8, Lookup->OwnerLen) == "exports")
						&& LookupOk;
			}

			// A mirrored member resolves to the class that *declares* it, which is what lets the
			// editor name the Godot original: the Verse name alone cannot be inverted, because
			// the transform to PascalCase drops the underscores that separated the words. A
			// property reports itself as the var it is, so the editor has to route on the owner
			// rather than on the kind.
			const size_t ProbeUse = ExportsSource.find("set Position");
			if (Step("the fixture still writes a mirrored Godot property", ProbeUse != std::string::npos))
			{
				int32_t ProbeRow = 0;
				int32_t ProbeColumn = 0;
				RowColumnOf(ExportsSource, ProbeUse + 4, ProbeRow, ProbeColumn);
				const vh_lookup_desc* ProbeLookup = nullptr;
				if (Step("vh_lookup_symbol on a mirrored Godot property",
						LookupSymbolFn(ExportsPathUtf8.c_str(), ProbeRow, ProbeColumn, &ProbeLookup) == VH_OK)
					&& ProbeLookup)
				{
					LookupOk = Step("it resolves to Position", Text(ProbeLookup->NameUtf8, ProbeLookup->NameLen) == "Position") && LookupOk;
					LookupOk = Step("it is a var data definition", ProbeLookup->Kind == VH_LOOKUP_DATA && ProbeLookup->IsVar != 0) && LookupOk;
					LookupOk = Step("it names the class that declares it", Text(ProbeLookup->OwnerUtf8, ProbeLookup->OwnerLen) == "node2d") && LookupOk;
				}
				else
				{
					LookupOk = false;
				}
			}

			// A var resolves the same way and says so, which is the whole of what the editor
			// needs to tell Godot's two local lookup results apart.
			const size_t ScaleUse = ExportsSource.find("set Scale");
			if (ScaleUse != std::string::npos)
			{
				int32_t ScaleRow = 0;
				int32_t ScaleColumn = 0;
				RowColumnOf(ExportsSource, ScaleUse + 4, ScaleRow, ScaleColumn);
				const vh_lookup_desc* ScaleLookup = nullptr;
				if (Step("vh_lookup_symbol on a var", LookupSymbolFn(ExportsPathUtf8.c_str(), ScaleRow, ScaleColumn, &ScaleLookup) == VH_OK) && ScaleLookup)
				{
					LookupOk = Step("Scale is a var", ScaleLookup->IsVar != 0) && LookupOk;
				}
				else
				{
					LookupOk = false;
				}
			}

			// An archetype instantiation names its class, and the editor turns that into a Godot
			// doc page. vector2 is a Godot builtin rather than a mirrored class, so it reaches
			// the class table by a different route -- but it has to arrive as a class either way,
			// or the editor falls back to calling it a local constant.
			const size_t VectorUse = ExportsSource.find("vector2{");
			if (Step("the fixture still instantiates a builtin value type", VectorUse != std::string::npos))
			{
				int32_t VectorRow = 0;
				int32_t VectorColumn = 0;
				RowColumnOf(ExportsSource, VectorUse, VectorRow, VectorColumn);
				const vh_lookup_desc* VectorLookup = nullptr;
				if (Step("vh_lookup_symbol on an archetype's class",
						LookupSymbolFn(ExportsPathUtf8.c_str(), VectorRow, VectorColumn, &VectorLookup) == VH_OK)
					&& VectorLookup)
				{
					LookupOk = Step("it resolves to vector2", Text(VectorLookup->NameUtf8, VectorLookup->NameLen) == "vector2") && LookupOk;
					LookupOk = Step("it is a class", VectorLookup->Kind == VH_LOOKUP_CLASS) && LookupOk;
				}
				else
				{
					LookupOk = false;
				}
			}

			// The base in a class header, which is the one place every script names `object`.
			// The editor documents it as Godot's Object -- the class gen_verse_api.py skips
			// because this hand-written one already stands where it does -- and that mapping is
			// only reachable if the base position answers as a class rather than as a use of
			// some value called object.
			const size_t BaseUse = ExportsSource.find("(object)");
			if (Step("the fixture still derives from object", BaseUse != std::string::npos))
			{
				int32_t BaseRow = 0;
				int32_t BaseColumn = 0;
				RowColumnOf(ExportsSource, BaseUse + 1, BaseRow, BaseColumn);
				const vh_lookup_desc* BaseLookup = nullptr;
				if (Step("vh_lookup_symbol on a class header's base",
						LookupSymbolFn(ExportsPathUtf8.c_str(), BaseRow, BaseColumn, &BaseLookup) == VH_OK)
					&& BaseLookup)
				{
					LookupOk = Step("it resolves to object", Text(BaseLookup->NameUtf8, BaseLookup->NameLen) == "object") && LookupOk;
					LookupOk = Step("object is a class", BaseLookup->Kind == VH_LOOKUP_CLASS) && LookupOk;
				}
				else
				{
					LookupOk = false;
				}
			}

			// A definition resolves at its own name, so hovering a method where it is declared
			// describes it. The narrowing this needs is what stops a blank column in the body
			// from resolving to the enclosing function too.
			const size_t BumpDecl = ExportsSource.find("Bump<public>()");
			if (Step("the fixture still declares Bump", BumpDecl != std::string::npos))
			{
				int32_t BumpRow = 0;
				int32_t BumpColumn = 0;
				RowColumnOf(ExportsSource, BumpDecl, BumpRow, BumpColumn);
				const vh_lookup_desc* BumpLookup = nullptr;
				if (Step("vh_lookup_symbol on a method's own declaration",
						LookupSymbolFn(ExportsPathUtf8.c_str(), BumpRow, BumpColumn, &BumpLookup) == VH_OK)
					&& BumpLookup)
				{
					LookupOk = Step("it resolves to Bump", Text(BumpLookup->NameUtf8, BumpLookup->NameLen) == "Bump") && LookupOk;
					LookupOk = Step("it is a function", BumpLookup->Kind == VH_LOOKUP_FUNCTION) && LookupOk;
				}
				else
				{
					LookupOk = false;
				}

				// The body is indented past the declaration, so a column inside it that holds
				// nothing must not fall back to the function whose locus spans it.
				const vh_lookup_desc* Inside = nullptr;
				LookupOk = Step("a blank column in the body still resolves to nothing",
							   LookupSymbolFn(ExportsPathUtf8.c_str(), BumpRow + 1, 0, &Inside) == VH_ERR_NOT_FOUND)
						&& LookupOk;
			}

			// An override carries its parent, which is what lets the editor describe a method
			// that says nothing about itself and send a click somewhere other than the line the
			// cursor is already on.
			const size_t ReadyDecl = ExportsSource.find("PhysicsProcess<override>(");
			if (Step("the fixture still overrides a lifecycle method", ReadyDecl != std::string::npos))
			{
				int32_t ReadyRow = 0;
				int32_t ReadyColumn = 0;
				RowColumnOf(ExportsSource, ReadyDecl, ReadyRow, ReadyColumn);
				const vh_lookup_desc* ReadyLookup = nullptr;
				if (Step("vh_lookup_symbol on an override",
						LookupSymbolFn(ExportsPathUtf8.c_str(), ReadyRow, ReadyColumn, &ReadyLookup) == VH_OK)
					&& ReadyLookup)
				{
					LookupOk = Step("it knows the cursor is on a definition", ReadyLookup->IsDefinition != 0) && LookupOk;
					/* `node` since Phase 4: every one of Godot's virtuals is generated onto the
					 * class that declares it, so _Ready belongs to node rather than to the
					 * hand-written native root. */
					LookupOk = Step("it names the class the override came from",
								   Text(ReadyLookup->OverriddenOwnerUtf8, ReadyLookup->OverriddenOwnerLen) == "node")
							&& LookupOk;
					LookupOk = Step("and where that parent was written",
								   ReadyLookup->OverriddenLine >= 0 && ReadyLookup->OverriddenPathLen > 0)
							&& LookupOk;
				}
				else
				{
					LookupOk = false;
				}
			}

			// A reference is not a declaration, so it must not be redirected to a parent: the
			// call that runs is the one the script wrote.
			const size_t ProbeCall = ExportsSource.find("Probe()");
			if (ProbeCall != std::string::npos)
			{
				int32_t CallRow = 0;
				int32_t CallColumn = 0;
				RowColumnOf(ExportsSource, ProbeCall, CallRow, CallColumn);
				const vh_lookup_desc* CallLookup = nullptr;
				if (Step("vh_lookup_symbol on a call", LookupSymbolFn(ExportsPathUtf8.c_str(), CallRow, CallColumn, &CallLookup) == VH_OK)
					&& CallLookup)
				{
					LookupOk = Step("a call site is not reported as a definition", CallLookup->IsDefinition == 0) && LookupOk;
					LookupOk = Step("and carries no override to redirect to", CallLookup->OverriddenOwnerLen == 0) && LookupOk;
				}
				else
				{
					LookupOk = false;
				}
			}
			// A global function -- Print, IsInstanceValid, a singleton accessor -- is declared at
			// the top level of the bridge's own package, where there is no class to be owned by.
			// A definition there reports the file it was written in as its owner instead, since a
			// snippet scope carries its path as its name, and the two agreeing is what lets the
			// editor tell a global from a member and document it with Godot's own global.
			{
				const std::string HelloSource = ReadFileUtf8(VersePath);
				const std::string HelloPathUtf8 = VersePath.string();
				struct Global { const char* What; const std::string* Source; const char* Path; const char* Needle; const char* File; };
				const Global Globals[] = {
					{ "Print is declared by the native package", &HelloSource, HelloPathUtf8.c_str(), "Print(", "Godot.native.verse" },
					{ "IsInstanceValid by the layer above it", &ExportsSource, ExportsPathUtf8.c_str(), "IsInstanceValid", "GodotApi.native.verse" },
					{ "and a singleton accessor by the generated mirror", &HelloSource, HelloPathUtf8.c_str(), "GetEngine[", "GodotClasses.native.verse" },
				};
				for (const Global& G : Globals)
				{
					const size_t At = G.Source->find(G.Needle);
					if (!Step("located the global's call site", At != std::string::npos))
					{
						LookupOk = false;
						continue;
					}
					int32_t Row = 0;
					int32_t Column = 0;
					RowColumnOf(*G.Source, At + 1, Row, Column);
					const vh_lookup_desc* Found = nullptr;
					const bool Resolved = LookupSymbolFn(G.Path, Row, Column, &Found) == VH_OK && Found != nullptr;
					const std::string Owner = Resolved ? Text(Found->OwnerUtf8, Found->OwnerLen) : std::string();
					const std::string DeclaredIn = Resolved ? Text(Found->PathUtf8, Found->PathLen) : std::string();
					LookupOk = Step(G.What,
								   Resolved && Found->Kind == VH_LOOKUP_FUNCTION
									   && fs::path(DeclaredIn).filename().string() == G.File)
							&& LookupOk;
					LookupOk = Step("owned by that file rather than by a class", !Owner.empty() && Owner == DeclaredIn) && LookupOk;
				}
			}

			// A declaration resolves at its name and nowhere else in its own declaration line.
			// The specifiers, the parameter list and the return type all sit inside the span the
			// compiler gives the definition, and none of them means "this definition".
			{
				struct Spot { const char* What; const char* Needle; size_t Offset; const char* Expect; const char* Owner; };
				const Spot Spots[] = {
					{ "an access specifier resolves to nothing", "Speed<public>:float", strlen("Speed<pub"), nullptr, nullptr },
					{ "nor does the bracket opening one", "Speed<public>:float", strlen("Speed"), nullptr, nullptr },
					{ "nor does an override specifier", "PhysicsProcess<override>(", strlen("PhysicsProcess<over"), nullptr, nullptr },
					{ "the member's own name still does", "Speed<public>:float", 2, "Speed", "exports" },
					{ "and so does the method's", "_PhysicsProcess<override>(", 2, "_PhysicsProcess", "exports_probe" },
					// A parameter is described by itself rather than by the method it belongs to,
					// which is what stops hovering an argument from documenting the whole call.
					// The editor declines to show anything for one, but that is its policy: the
					// host still has to resolve it, or the enclosing method would answer instead.
					{ "a parameter resolves to the parameter", "_PhysicsProcess<override>(Delta:float)", strlen("_PhysicsProcess<override>(De"), "Delta", "_PhysicsProcess" },
					{ "and its type still resolves to the type", "PhysicsProcess<override>(Delta:float)", strlen("PhysicsProcess<override>(Delta:fl"), "float", "Verse" },
				};
				for (const Spot& S : Spots)
				{
					const size_t At = ExportsSource.find(S.Needle);
					if (!Step("located the fixture spot", At != std::string::npos)) { LookupOk = false; continue; }
					int32_t Row = 0;
					int32_t Column = 0;
					RowColumnOf(ExportsSource, At + S.Offset, Row, Column);
					const vh_lookup_desc* Found = nullptr;
					const int32_t Status = LookupSymbolFn(ExportsPathUtf8.c_str(), Row, Column, &Found);
					if (S.Expect == nullptr)
					{
						LookupOk = Step(S.What, Status == VH_ERR_NOT_FOUND) && LookupOk;
					}
					else
					{
						LookupOk = Step(S.What,
									   Status == VH_OK && Found
										   && Text(Found->NameUtf8, Found->NameLen) == S.Expect
										   && Text(Found->OwnerUtf8, Found->OwnerLen) == S.Owner)
								&& LookupOk;
					}
				}

				// Only a parameter is flagged as one. A member and a method both live in a class
				// scope and neither is in anyone's signature, so the flag is what tells an editor
				// that a definition has no documentation of its own to read.
				struct Flagged { const char* What; const char* Needle; size_t Offset; bool Expect; };
				const Flagged Flags[] = {
					{ "a parameter is flagged as one", "PhysicsProcess<override>(Delta:float)", strlen("PhysicsProcess<override>(De"), true },
					{ "a method is not", "PhysicsProcess<override>(", 2, false },
					{ "and neither is a data member", "Speed<public>:float", 2, false },
				};
				for (const Flagged& F : Flags)
				{
					const size_t At = ExportsSource.find(F.Needle);
					int32_t Row = 0;
					int32_t Column = 0;
					RowColumnOf(ExportsSource, At + F.Offset, Row, Column);
					const vh_lookup_desc* Found = nullptr;
					LookupOk = Step(F.What,
								   LookupSymbolFn(ExportsPathUtf8.c_str(), Row, Column, &Found) == VH_OK
									   && Found && (Found->IsParameter != 0) == F.Expect)
							&& LookupOk;
				}
			}

			// A member reached through a `.` resolves to the type that declares it rather than to
			// whatever the receiver was. `X` belongs to vector2 even though `Position` is a
			// property of node2d, and it is the owner that lets the editor name Godot's Vector2.x.
			const size_t FieldUse = ExportsSource.find("Position.X");
			if (Step("the fixture still reads a field off a value type", FieldUse != std::string::npos))
			{
				int32_t FieldRow = 0;
				int32_t FieldColumn = 0;
				RowColumnOf(ExportsSource, FieldUse + strlen("Position."), FieldRow, FieldColumn);
				const vh_lookup_desc* FieldLookup = nullptr;
				if (Step("vh_lookup_symbol on a field of a value type",
						LookupSymbolFn(ExportsPathUtf8.c_str(), FieldRow, FieldColumn, &FieldLookup) == VH_OK)
					&& FieldLookup)
				{
					LookupOk = Step("it resolves to X", Text(FieldLookup->NameUtf8, FieldLookup->NameLen) == "X") && LookupOk;
					LookupOk = Step("declared by vector2, not by the receiver's class",
								   Text(FieldLookup->OwnerUtf8, FieldLookup->OwnerLen) == "vector2")
							&& LookupOk;
				}
				else
				{
					LookupOk = false;
				}
			}

			// Past the end of a line nothing encloses the position, so the answer is a refusal
			// rather than whichever definition happens to span the row.
			const vh_lookup_desc* Nothing = nullptr;
			LookupOk = Step("a column past the end of a line resolves to nothing",
						   LookupSymbolFn(ExportsPathUtf8.c_str(), DeclRow, 500, &Nothing) == VH_ERR_NOT_FOUND)
					&& LookupOk;
		}
	}

	// Completion. Every case asks about a buffer that does not analyse cleanly, because that is
	// the only state completion is ever asked in: the member being typed does not exist yet.
	{
		const std::string ExportsSource = ReadFileUtf8(ExportsPath);
		bool CompleteOk = Step("read exports.verse for completion", !ExportsSource.empty());

		auto Text = [](const char* Utf8, int32_t Len) { return std::string(Utf8 ? Utf8 : "", Len); };

		// Finds Name among the items, so a case can assert what must be offered without pinning
		// the whole list -- which would break every time a Godot class gains a method.
		auto Offers = [&Text](const vh_complete_item* Items, int32_t Count, const char* Name) -> const vh_complete_item* {
			for (int32_t Index = 0; Index < Count; ++Index)
			{
				if (Text(Items[Index].NameUtf8, Items[Index].NameLen) == Name)
				{
					return &Items[Index];
				}
			}
			return nullptr;
		};

		// The editor's buffer with the half-typed member standing in as the placeholder the
		// GDExtension substitutes, which is what makes the answer survive the rest of the prefix.
		// Every case below uses it, because it is the only buffer shape completion ever sees.
		const size_t FieldUse = ExportsSource.find("Position.X");
		CompleteOk = Step("located the fixture's field read", FieldUse != std::string::npos) && CompleteOk;

		if (CompleteOk)
		{
			std::string Typing = ExportsSource;
			Typing.replace(FieldUse, strlen("Position.X"), "Position.VhCompletionCursor");

			// The position is the receiver's last byte, not the cursor's: `Position` is the only
			// part of `Position.` that resolves to anything.
			int32_t RecvRow = 0;
			int32_t RecvColumn = 0;
			RowColumnOf(Typing, FieldUse + strlen("Position") - 1, RecvRow, RecvColumn);

			const vh_complete_item* Items = nullptr;
			int32_t Count = 0;
			if (Step("vh_complete_symbol on a half-typed member",
					CompleteSymbolFn(ExportsPathUtf8.c_str(), Typing.c_str(), RecvRow, RecvColumn,
									 VH_COMPLETE_MEMBERS, &Items, &Count) == VH_OK))
			{
				CompleteOk = Step("it offers vector2's X", Offers(Items, Count, "X") != nullptr) && CompleteOk;
				CompleteOk = Step("and its Y", Offers(Items, Count, "Y") != nullptr) && CompleteOk;
				if (const vh_complete_item* X = Offers(Items, Count, "X"))
				{
					CompleteOk = Step("X is named as vector2's", Text(X->OwnerUtf8, X->OwnerLen) == "vector2") && CompleteOk;
					CompleteOk = Step("and typed", Text(X->TypeUtf8, X->TypeLen) == "float") && CompleteOk;
				}
				// A value type's fields are the whole of it, so this is the one case where the
				// list can be pinned exactly -- and a leak of the enclosing scope would show here.
				CompleteOk = Step("and nothing else", Count == 2) && CompleteOk;
			}
			else
			{
				CompleteOk = false;
			}

			// A node reached through `Self` completes to the mirrored class' surface, inherited
			// members included: Position is node2d's and GetName is node's.
			std::string SelfTyping = ExportsSource;
			SelfTyping.replace(FieldUse, strlen("Position.X"), "Self.VhCompletionCursor");
			RowColumnOf(SelfTyping, FieldUse + strlen("Self") - 1, RecvRow, RecvColumn);
			if (Step("vh_complete_symbol on a node",
					CompleteSymbolFn(ExportsPathUtf8.c_str(), SelfTyping.c_str(), RecvRow, RecvColumn,
									 VH_COMPLETE_MEMBERS, &Items, &Count) == VH_OK))
			{
				CompleteOk = Step("it offers the class' own Probe", Offers(Items, Count, "Probe") != nullptr) && CompleteOk;
				CompleteOk = Step("node2d's Position", Offers(Items, Count, "Position") != nullptr) && CompleteOk;
				CompleteOk = Step("and node's GetName, two classes up", Offers(Items, Count, "GetName") != nullptr) && CompleteOk;
				if (const vh_complete_item* Position = Offers(Items, Count, "Position"))
				{
					CompleteOk = Step("Position is offered as the var it is", Position->IsVar != 0) && CompleteOk;
					CompleteOk = Step("and as a data definition", Position->Kind == VH_LOOKUP_DATA) && CompleteOk;
				}
				if (const vh_complete_item* Probe = Offers(Items, Count, "Probe"))
				{
					CompleteOk = Step("Probe is offered as a function", Probe->Kind == VH_LOOKUP_FUNCTION) && CompleteOk;
					// Where the caret lands after inserting a call turns on this: nothing to type
					// between the brackets means the caret belongs past them.
					CompleteOk = Step("and as one taking no arguments", Probe->ParamCount == 0) && CompleteOk;
				}
				if (const vh_complete_item* Process = Offers(Items, Count, "_PhysicsProcess"))
				{
					CompleteOk = Step("a method with an argument says so", Process->ParamCount == 1) && CompleteOk;
				}
				if (const vh_complete_item* Position = Offers(Items, Count, "Position"))
				{
					CompleteOk = Step("and a property is not a function at all", Position->ParamCount == -1) && CompleteOk;
				}
			}
			else
			{
				CompleteOk = false;
			}

			// A bare identifier completes against everything the cursor can see: the local above
			// it, the enclosing class, and the packages the file brought in with `using`.
			const size_t LocalUse = ExportsSource.find("X := Shifted");
			CompleteOk = Step("located the fixture's local", LocalUse != std::string::npos) && CompleteOk;
			if (LocalUse != std::string::npos)
			{
				int32_t ScopeRow = 0;
				int32_t ScopeColumn = 0;
				std::string ScopeTyping = ExportsSource;
				ScopeTyping.replace(LocalUse + strlen("X := "), strlen("Shifted"), "VhCompletionCursor");
				RowColumnOf(ScopeTyping, LocalUse + strlen("X := "), ScopeRow, ScopeColumn);
				if (Step("vh_complete_symbol in a function body",
						CompleteSymbolFn(ExportsPathUtf8.c_str(), ScopeTyping.c_str(), ScopeRow, ScopeColumn,
										 VH_COMPLETE_SCOPE, &Items, &Count) == VH_OK))
				{
					CompleteOk = Step("it offers the local declared above", Offers(Items, Count, "Shifted") != nullptr) && CompleteOk;
					CompleteOk = Step("the enclosing class' own method", Offers(Items, Count, "Probe") != nullptr) && CompleteOk;
					CompleteOk = Step("an inherited property", Offers(Items, Count, "Position") != nullptr) && CompleteOk;
					CompleteOk = Step("and Print, which arrived through a using",
									 Offers(Items, Count, "Print") != nullptr)
							  && CompleteOk;
					CompleteOk = Step("with no name offered twice", [&]() {
						for (int32_t Index = 1; Index < Count; ++Index)
						{
							if (Text(Items[Index].NameUtf8, Items[Index].NameLen)
								== Text(Items[Index - 1].NameUtf8, Items[Index - 1].NameLen))
							{
								return false;
							}
						}
						return true;
					}()) && CompleteOk;
				}
				else
				{
					CompleteOk = false;
				}
			}

			// A member being declared rather than a name being used. The editor completes an
			// inherited method there to the declaration that overrides it, which needs the base's
			// signature spelled with its parameters' own names -- and needs to know which methods
			// are still free to override.
			const size_t MemberDecl = ExportsSource.find("    Probe<public>()");
			CompleteOk = Step("located the fixture's member declarations", MemberDecl != std::string::npos) && CompleteOk;
			if (MemberDecl != std::string::npos)
			{
				std::string DeclTyping = ExportsSource;
				DeclTyping.insert(MemberDecl, "    VhCompletionCursor\n\n");
				int32_t DeclRow = 0;
				int32_t DeclColumn = 0;
				RowColumnOf(DeclTyping, MemberDecl + strlen("    "), DeclRow, DeclColumn);
				if (Step("vh_complete_symbol at a class member declaration",
						CompleteSymbolFn(ExportsPathUtf8.c_str(), DeclTyping.c_str(), DeclRow, DeclColumn,
										 VH_COMPLETE_SCOPE, &Items, &Count) == VH_OK))
				{
					if (const vh_complete_item* Ready = Offers(Items, Count, "_Ready"))
					{
						CompleteOk = Step("an inherited method is offered as overridable", Ready->IsOverridable != 0) && CompleteOk;
						CompleteOk = Step("owned by the class that declares it",
										 Text(Ready->OwnerUtf8, Ready->OwnerLen) == "node")
								  && CompleteOk;
						CompleteOk = Step("and spelled as a declaration",
										 Text(Ready->SignatureUtf8, Ready->SignatureLen) == "():void")
								  && CompleteOk;
					}
					else
					{
						CompleteOk = Step("an inherited method is offered at all", false);
					}
					if (const vh_complete_item* Process = Offers(Items, Count, "_Process"))
					{
						// The parameter's own name, which is the whole reason the signature is not
						// read off the function type: that spells this one "float->void".
						CompleteOk = Step("a parameter is named in the signature",
										 Text(Process->SignatureUtf8, Process->SignatureLen) == "(Delta:float):void")
								  && CompleteOk;
					}
					// Already overridden by the fixture, so it comes back owned by the fixture's
					// own class -- which is how the editor knows not to offer it a second time.
					if (const vh_complete_item* Physics = Offers(Items, Count, "_PhysicsProcess"))
					{
						CompleteOk = Step("an override already written is owned by the class that wrote it",
										 Text(Physics->OwnerUtf8, Physics->OwnerLen) == "exports_probe")
								  && CompleteOk;
					}
					// Nothing but a class' methods can be overridden, and nothing but a function
					// has a declaration to spell.
					if (const vh_complete_item* Position = Offers(Items, Count, "Position"))
					{
						CompleteOk = Step("a property is neither overridable nor spellable",
										 Position->IsOverridable == 0 && Position->SignatureLen == 0)
								  && CompleteOk;
					}
					if (const vh_complete_item* Print = Offers(Items, Count, "Print"))
					{
						CompleteOk = Step("and neither is a function no class declares", Print->IsOverridable == 0) && CompleteOk;
					}
					// A class var's <getter>/<setter> is a class member the analyzer nonetheless
					// refuses an override of by name, and the generated mirror is built out of
					// them -- node2d alone contributes a dozen.
					if (const vh_complete_item* Accessor = Offers(Items, Count, "GlobalPositionGetter"))
					{
						CompleteOk = Step("nor is a class var's accessor", Accessor->IsOverridable == 0) && CompleteOk;
					}
					else
					{
						CompleteOk = Step("the mirror's accessors reach the scope at all", false);
					}
				}
				else
				{
					CompleteOk = false;
				}
			}

			// An `@`, which admits neither the scope nor the members of anything: the attributes
			// alone, spelled without the `@` the editor has already got on screen.
			const size_t AttributeUse = ExportsSource.find("    @export");
			CompleteOk = Step("located the fixture's attribute", AttributeUse != std::string::npos) && CompleteOk;
			if (AttributeUse != std::string::npos)
			{
				std::string AttributeTyping = ExportsSource;
				AttributeTyping.replace(AttributeUse + strlen("    @"), strlen("export"), "VhCompletionCursor");
				int32_t AttributeRow = 0;
				int32_t AttributeColumn = 0;
				RowColumnOf(AttributeTyping, AttributeUse + strlen("    @"), AttributeRow, AttributeColumn);
				if (Step("vh_complete_symbol at an attribute",
						CompleteSymbolFn(ExportsPathUtf8.c_str(), AttributeTyping.c_str(), AttributeRow, AttributeColumn,
										 VH_COMPLETE_ATTRIBUTES, &Items, &Count) == VH_OK))
				{
					CompleteOk = Step("it offers export", Offers(Items, Count, "export") != nullptr) && CompleteOk;
					CompleteOk = Step("and the bridge's own global_class",
									 Offers(Items, Count, "global_class") != nullptr)
							  && CompleteOk;
					if (const vh_complete_item* Export = Offers(Items, Count, "export"))
					{
						CompleteOk = Step("an attribute is offered as the class it is", Export->Kind == VH_LOOKUP_CLASS) && CompleteOk;
					}
					// The name an author writes for an attribute that carries a payload is the
					// <constructor> beside the class, not the class -- `@export_group("Movement")`
					// against export_group_attribute -- and it has to be offered as the call it is.
					if (const vh_complete_item* Group = Offers(Items, Count, "export_group"))
					{
						CompleteOk = Step("a payload attribute is offered as a function", Group->Kind == VH_LOOKUP_FUNCTION) && CompleteOk;
						CompleteOk = Step("taking the one argument it spells", Group->ParamCount == 1) && CompleteOk;
					}
					else
					{
						CompleteOk = Step("a payload attribute is offered at all", false);
					}
					// The whole point of the mode: the scope this position sits in is the same one
					// VH_COMPLETE_SCOPE answers with three hundred names, none of which are legal
					// after an `@`.
					CompleteOk = Step("but no name from the enclosing scope", Offers(Items, Count, "Print") == nullptr) && CompleteOk;
					CompleteOk = Step("nor a member of the class being written", Offers(Items, Count, "Speed") == nullptr) && CompleteOk;
					// Applying the base every attribute derives from means nothing, and the
					// compiler-generated constructors are not spellable either.
					CompleteOk = Step("nor attribute itself", Offers(Items, Count, "attribute") == nullptr) && CompleteOk;
					CompleteOk = Step("nor a generated constructor", Offers(Items, Count, "Constructor") == nullptr) && CompleteOk;
				}
				else
				{
					CompleteOk = false;
				}
			}

			// The same question at the top level of a file, where the cursor is inside no
			// definition at all: an attribute above a class declaration, which is the one place
			// `@global_class` is ever written. The scope there is the file's own -- the snippet --
			// and it is the only scope that carries the file's `using`, so a position defaulted to
			// the package's root module instead would answer with neither the attribute the line
			// is reaching for nor anything else the Godot package brings into view.
			const size_t GlobalClassUse = ExportsSource.find("@global_class");
			CompleteOk = Step("located the fixture's class attribute", GlobalClassUse != std::string::npos) && CompleteOk;
			if (GlobalClassUse != std::string::npos)
			{
				std::string TopLevelTyping = ExportsSource;
				TopLevelTyping.replace(GlobalClassUse + 1, strlen("global_class"), "VhCompletionCursor");
				int32_t TopRow = 0;
				int32_t TopColumn = 0;
				RowColumnOf(TopLevelTyping, GlobalClassUse + 1, TopRow, TopColumn);
				if (Step("vh_complete_symbol at a top-level attribute",
						CompleteSymbolFn(ExportsPathUtf8.c_str(), TopLevelTyping.c_str(), TopRow, TopColumn,
										 VH_COMPLETE_ATTRIBUTES, &Items, &Count) == VH_OK))
				{
					CompleteOk = Step("it offers global_class above a class declaration",
									 Offers(Items, Count, "global_class") != nullptr)
							  && CompleteOk;
				}
				else
				{
					CompleteOk = false;
				}

				// And the scope mode at the same position, which is the other half of the same
				// fault: the file's `using` is what puts the mirrored API in view anywhere.
				if (Step("vh_complete_symbol in a top-level scope",
						CompleteSymbolFn(ExportsPathUtf8.c_str(), TopLevelTyping.c_str(), TopRow, TopColumn,
										 VH_COMPLETE_SCOPE, &Items, &Count) == VH_OK))
				{
					CompleteOk = Step("the Godot package is in view there", Offers(Items, Count, "Print") != nullptr) && CompleteOk;
					CompleteOk = Step("and so are its classes", Offers(Items, Count, "node2d") != nullptr) && CompleteOk;
				}
				else
				{
					CompleteOk = false;
				}
			}

			// Whitespace has no members, and answering anyway would put the enclosing scope behind
			// a dot the author never typed.
			const vh_complete_item* NoItems = nullptr;
			int32_t NoCount = 0;
			CompleteOk = Step("a position with no expression on it completes to nothing",
							 CompleteSymbolFn(ExportsPathUtf8.c_str(), ExportsSource.c_str(), 0, 0,
											  VH_COMPLETE_MEMBERS, &NoItems, &NoCount) == VH_ERR_NOT_FOUND)
					  && CompleteOk;

			// The argument hint. Asked at the callee's last byte, for the same reason members are
			// asked at the receiver's: the arguments being typed do not analyse.
			const size_t Call = ExportsSource.find("PhysicsProcess<override>(");
			if (Step("located the fixture's method", Call != std::string::npos))
			{
				int32_t CalleeRow = 0;
				int32_t CalleeColumn = 0;
				RowColumnOf(ExportsSource, Call + strlen("PhysicsUpdat"), CalleeRow, CalleeColumn);
				const vh_signature_desc* Signature = nullptr;
				if (Step("vh_signature_at on a method",
						SignatureAtFn(ExportsPathUtf8.c_str(), ExportsSource.c_str(), CalleeRow, CalleeColumn, &Signature) == VH_OK)
					&& Signature)
				{
					CompleteOk = Step("it names the method", Text(Signature->NameUtf8, Signature->NameLen) == "_PhysicsProcess") && CompleteOk;
					CompleteOk = Step("and its return type", Text(Signature->ResultUtf8, Signature->ResultLen) == "void") && CompleteOk;
					CompleteOk = Step("and its one parameter", Signature->ParamCount == 1) && CompleteOk;
					if (Signature->ParamCount == 1)
					{
						CompleteOk = Step("named Delta", Text(Signature->Params[0].NameUtf8, Signature->Params[0].NameLen) == "Delta") && CompleteOk;
						CompleteOk = Step("and typed float", Text(Signature->Params[0].TypeUtf8, Signature->Params[0].TypeLen) == "float") && CompleteOk;
					}
				}
				else
				{
					CompleteOk = false;
				}

				// A name that is not a function has no argument list to describe.
				const size_t NotCallable = ExportsSource.find("Speed<public>:float");
				const vh_signature_desc* NoSignature = nullptr;
				int32_t DataRow = 0;
				int32_t DataColumn = 0;
				RowColumnOf(ExportsSource, NotCallable + 2, DataRow, DataColumn);
				CompleteOk = Step("a data member has no signature",
								 SignatureAtFn(ExportsPathUtf8.c_str(), ExportsSource.c_str(), DataRow, DataColumn, &NoSignature) == VH_ERR_NOT_FOUND)
						  && CompleteOk;
			}

			// Completion left the host holding a scratch buffer; everything below reads the
			// semantic program and has to see the file as it is on disk.
			CompleteOk = Step("the project analyses clean again",
							 CheckProjectFn(ExportsPathUtf8.c_str(), ExportsSource.c_str()) == VH_OK)
					  && CompleteOk;
		}

		// What a class declares itself, which is what becomes its documentation. Inherited names
		// are deliberately absent: node2d's Position is Godot's to document, not this class'.
		{
			const vh_complete_item* Members = nullptr;
			int32_t MemberCount = 0;
			if (Step("vh_class_members on a script class",
					ClassMembersFn("exports", &Members, &MemberCount) == VH_OK))
			{
				auto Find = [&Text](const vh_complete_item* Items, int32_t Count, const char* Name) -> const vh_complete_item* {
					for (int32_t Index = 0; Index < Count; ++Index)
					{
						if (Text(Items[Index].NameUtf8, Items[Index].NameLen) == Name) { return &Items[Index]; }
					}
					return nullptr;
				};
				CompleteOk = Step("it lists an @export member", Find(Members, MemberCount, "Speed") != nullptr) && CompleteOk;
				CompleteOk = Step("and one carrying no attribute at all", Find(Members, MemberCount, "Hidden") != nullptr) && CompleteOk;
				CompleteOk = Step("and its methods", Find(Members, MemberCount, "Bump") != nullptr) && CompleteOk;
				CompleteOk = Step("but nothing it merely inherits", Find(Members, MemberCount, "Position") == nullptr) && CompleteOk;
				if (const vh_complete_item* Speed = Find(Members, MemberCount, "Speed"))
				{
					// The line is what lets the consumer find the comment block above a member.
					CompleteOk = Step("a member carries where it was declared",
									 Speed->Line >= 0 && Text(Speed->PathUtf8, Speed->PathLen) == ExportsPathUtf8)
							  && CompleteOk;
				}
			}
			else
			{
				CompleteOk = false;
			}

			const vh_complete_item* Absent = nullptr;
			int32_t AbsentCount = 0;
			CompleteOk = Step("a class the program does not have reports not found",
							 ClassMembersFn("no_such_class", &Absent, &AbsentCount) == VH_ERR_NOT_FOUND)
					  && CompleteOk;
		}

		LookupOk = CompleteOk && LookupOk;
	}

	const char* RunArgs[1] = { VersePathUtf8.c_str() };
	int64_t ExitCode = 0;
	bool RunOk = RunMainFn(RunArgs, 1, &ExitCode) == VH_OK;
	Step("vh_run_main", RunOk);
	printf("[smoke] exit code: %lld\n", static_cast<long long>(ExitCode));

	bool CallsOk = true;
	TickFn(0.0, nullptr);

	// --- Phase 3 / OQ-12: a second generation, at the same verse path -------------------------
	//
	// What this answers is whether a generation may change the package *name* while the verse path
	// stays /user@localhost. It has to: a module path is user-visible text the editor writes into
	// the author's own file (R-TOOL-12), and a path carrying a generation number would be
	// invalidated by the author's next save.
	//
	// The second half of the same run is cheaper to ask here than anywhere else: module_probe sits
	// in a `gameplay` module and its body names a definition declared at the root with no `using`
	// written, so the build succeeding is what says root is implicit from inside a submodule.
	//
	// Deliberately before everything below rather than after it, so that every check in the rest
	// of this file runs against the *second* generation. A generation that only half works would
	// otherwise look fine here and fail in an editor.
	{
		auto ReadGeneration = [&](vh_instance* Instance) -> int64_t {
			vh_value Result{};
			if (!Instance
				|| InstanceCallFn(Instance, "(/user@localhost/reload_probe:)Generation", nullptr, 0, nullptr, &Result) != VH_OK
				|| Result.Type != VH_TYPE_INT)
			{
				return -1;
			}
			return Result.Int;
		};

		CallsOk = Step("generation 1's instance reports generation 1", ReadGeneration(GenerationOne) == 1) && CallsOk;
		CallsOk = Step("write the edited fixture and a module beside it",
					   WriteFileUtf8(ReloadPath, ReloadProbeSource(2))
						   && WriteFileUtf8(ModuleProbePath, ModuleProbeSource())) && CallsOk;

		vh_source_file SecondFiles[5] = {
			{ VersePathUtf8.c_str(), nullptr },
			{ ExportsPathUtf8.c_str(), nullptr },
			{ ReloadPathUtf8.c_str(), nullptr },
			{ ModuleProbePathUtf8.c_str(), "gameplay" },
			{ TasksPathUtf8.c_str(), nullptr },
		};
		int32_t SecondGeneration = 0;
		DiagnosticErrorCount = 0;
		const bool SecondBuilt = CompileProjectFn(SecondFiles, 5, &SecondGeneration) == VH_OK;
		CallsOk = Step("a second vh_compile_project in the same process builds", SecondBuilt) && CallsOk;
		CallsOk = Step("and reports generation 2", SecondGeneration == 2) && CallsOk;
		CallsOk = Step("a file in a module reaches a root definition with nothing imported",
					   SecondBuilt && DiagnosticErrorCount == 0) && CallsOk;

		// A class in a module is addressed by a qualified name, and everything the ABI does with a
		// class name has to take one: the lookup, the method list and the per-call signature the
		// result type comes from. Instantiating it and reading an int back exercises all three,
		// because a result the host cannot type comes back as void.
		vh_instance* InModule = nullptr;
		CallsOk = Step("a class in a module instantiates under its qualified name",
					   InstantiateFn("gameplay/module_probe", 5, &InModule) == VH_OK && InModule != nullptr)
			   && CallsOk;
		vh_instance* Unqualified = nullptr;
		CallsOk = Step("and an unqualified name does not reach it",
					   InstantiateFn("module_probe", 6, &Unqualified) != VH_OK) && CallsOk;
		{
			vh_value Total{};
			CallsOk = Step("and its method returns a typed value",
						   InModule != nullptr
							   && InstanceCallFn(InModule, "(/user@localhost/gameplay/module_probe:)Total",
												 nullptr, 0, nullptr, &Total) == VH_OK
							   && Total.Type == VH_TYPE_INT && Total.Int == 15)
				   && CallsOk;
		}
		const vh_method_desc* ModuleMethods = nullptr;
		int32_t ModuleMethodCount = 0;
		CallsOk = Step("a module's class has a method list of its own",
					   ClassMethodListFn("gameplay/module_probe", &ModuleMethods, &ModuleMethodCount) == VH_OK
						   && ModuleMethodCount > 0) && CallsOk;
		// R-TOOL-12's half of the ABI: which module would an import have to name for this to
		// resolve. module_probe is in `gameplay` and nothing else is in any module at all.
		{
			const vh_module_ref* Found = nullptr;
			int32_t FoundCount = 0;
			CallsOk = Step("vh_resolve_unknown_name finds the module declaring a name",
						   ResolveUnknownNameFn("module_probe", &Found, &FoundCount) == VH_OK
							   && FoundCount == 1
							   && std::string(Found[0].PathUtf8, Found[0].PathLen) == "gameplay") && CallsOk;
			CallsOk = Step("a name in the root module needs no import and so is no answer",
						   ResolveUnknownNameFn("reload_probe", &Found, &FoundCount) == VH_OK
							   && FoundCount == 0) && CallsOk;
			CallsOk = Step("and a name the project does not declare has none either",
						   ResolveUnknownNameFn("no_such_name_anywhere", &Found, &FoundCount) == VH_OK
							   && FoundCount == 0) && CallsOk;
		}

		ReleaseInstanceFn(InModule);

		vh_instance* Fresh = nullptr;
		CallsOk = Step("the new generation's class is what resolves now",
					   InstantiateFn("reload_probe", 4, &Fresh) == VH_OK && Fresh != nullptr) && CallsOk;
		CallsOk = Step("and a fresh instance runs the edited code", ReadGeneration(Fresh) == 2) && CallsOk;

		// The whole no-adoption rule, in one line: nothing transferred state, so the instance made
		// against generation 1 is still running generation 1's class.
		CallsOk = Step("while the instance from generation 1 keeps its own class",
					   ReadGeneration(GenerationOne) == 1) && CallsOk;

		ReleaseInstanceFn(Fresh);
		ReleaseInstanceFn(GenerationOne);
	}

	// The check that the verse path the host builds for a script's class --
	// /user@localhost/<file stem> -- is the one the semantic program actually files it under;
	// everything about exports depends on that string being right.
	Step("vh_has_class exports", HasClassFn("exports") != 0);

	const vh_export_desc* Exports = nullptr;
	int32_t ExportCount = 0;
	bool ExportsOk = Step("vh_class_export_list", ClassExportListFn("exports", &Exports, &ExportCount) == VH_OK);
	for (int32_t Index = 0; Index < ExportCount; ++Index)
	{
		printf("[smoke]   export %.*s type=%d is_var=%d\n",
			   static_cast<int>(Exports[Index].NameLen), Exports[Index].NameUtf8,
			   Exports[Index].Type, Exports[Index].IsVar);
	}

	auto FindExport = [&](const char* Name) -> const vh_export_desc* {
		for (int32_t Index = 0; Index < ExportCount; ++Index)
		{
			if (std::string(Exports[Index].NameUtf8, Exports[Index].NameLen) == Name)
			{
				return &Exports[Index];
			}
		}
		return nullptr;
	};

	const vh_export_desc* SpeedExport = FindExport("Speed");
	const vh_export_desc* ScaleExport = FindExport("Scale");
	const vh_export_desc* EnabledExport = FindExport("Enabled");
	ExportsOk = Step("Speed is a float", SpeedExport && SpeedExport->Type == VH_TYPE_FLOAT) && ExportsOk;
	ExportsOk = Step("Label is a string", FindExport("Label") && FindExport("Label")->Type == VH_TYPE_STRING) && ExportsOk;
	ExportsOk = Step("Enabled is a var logic", EnabledExport && EnabledExport->Type == VH_TYPE_LOGIC && EnabledExport->IsVar != 0) && ExportsOk;
	ExportsOk = Step("Speed is not a var", SpeedExport && SpeedExport->IsVar == 0) && ExportsOk;
	ExportsOk = Step("Scale is a var float", ScaleExport && ScaleExport->Type == VH_TYPE_FLOAT && ScaleExport->IsVar != 0) && ExportsOk;
	// The whole point of the attribute: an unmarked member stays out of the inspector.
	Step("unmarked Hidden is not exported", FindExport("Hidden") == nullptr);

	auto TextOf = [](const char* Utf8, int32_t Len) { return std::string(Utf8 ? Utf8 : "", Len); };

	// A range off the declared type, which is the bounds the compiler is already enforcing at
	// every assignment rather than a second opinion written beside them.
	const vh_export_desc* RangedExport = FindExport("Ranged");
	ExportsOk = Step("a bounded float carries its bounds",
					RangedExport && RangedExport->Hint == VH_EXPORT_HINT_RANGE
						&& RangedExport->HasRangeMin && RangedExport->HasRangeMax
						&& RangedExport->RangeMin == 0.0 && RangedExport->RangeMax == 500.0)
			 && ExportsOk;
	const vh_export_desc* StepsExport = FindExport("Steps");
	ExportsOk = Step("and so does a bounded int",
					StepsExport && StepsExport->Hint == VH_EXPORT_HINT_RANGE
						&& StepsExport->RangeMin == 0.0 && StepsExport->RangeMax == 10.0)
			 && ExportsOk;
	// Plain `float` reports its max as a NaN rather than an infinity, so an unbounded member is
	// the case a bounds check has to get right to avoid hinting every float in the project.
	ExportsOk = Step("an unbounded one carries none", ScaleExport && ScaleExport->Hint == VH_EXPORT_HINT_NONE) && ExportsOk;

	// One-sided, which Godot's hint cannot spell: the bound it has stands at both ends, and the
	// slice says which way the value is free to run. The control goes with it, since a slider
	// across a range of zero width means nothing.
	const vh_export_desc* AtLeastExport = FindExport("AtLeast");
	ExportsOk = Step("a floor with no ceiling reports only the floor",
					AtLeastExport && AtLeastExport->Hint == VH_EXPORT_HINT_RANGE
						&& AtLeastExport->HasRangeMin && !AtLeastExport->HasRangeMax
						&& AtLeastExport->RangeMin == 0.0)
			 && ExportsOk;
	const vh_export_desc* AtMostExport = FindExport("AtMost");
	ExportsOk = Step("and a ceiling with no floor only the ceiling",
					AtMostExport && AtMostExport->Hint == VH_EXPORT_HINT_RANGE
						&& !AtMostExport->HasRangeMin && AtMostExport->HasRangeMax
						&& AtMostExport->RangeMax == 1.0)
			 && ExportsOk;

	// `<` rather than `<=`, which arrives already normalised: the largest double under 500, and
	// so under 500 by less than any inspector could draw. Nothing here spells it as strict, and
	// nothing needs to -- the consumer rounds every bound inward to its own step, which lands
	// below 500 from this and on 500 from `<=`.
	const vh_export_desc* ExclusiveExport = FindExport("Exclusive");
	ExportsOk = Step("a strict float bound arrives just under the number written",
					ExclusiveExport && ExclusiveExport->Hint == VH_EXPORT_HINT_RANGE
						&& ExclusiveExport->HasRangeMax
						&& ExclusiveExport->RangeMax < 500.0 && ExclusiveExport->RangeMax > 499.99)
			 && ExportsOk;
	// An integer is normalised all the way: `0 < _X` is `1 <= _X`, exactly.
	const vh_export_desc* ExclusiveIntExport = FindExport("ExclusiveInt");
	ExportsOk = Step("a strict int bound is already the next integer",
					ExclusiveIntExport && ExclusiveIntExport->RangeMin == 1.0)
			 && ExportsOk;

	// A section is opened by the member that names it, at one of Godot's three nesting depths.
	ExportsOk = Step("Speed opens a group",
					SpeedExport && SpeedExport->GroupKind == VH_EXPORT_GROUP_GROUP
						&& TextOf(SpeedExport->GroupNameUtf8, SpeedExport->GroupNameLen) == "Movement")
			 && ExportsOk;
	Step("a member that opens none says so", ScaleExport && ScaleExport->GroupKind == VH_EXPORT_GROUP_NONE);
	Step("Label carries no hint", FindExport("Label") && FindExport("Label")->Hint == VH_EXPORT_HINT_NONE);

	// A Godot reference is a slot the scene may leave empty, so the member has to be able to hold
	// that. Both spellings are harvested -- the point of reporting a rejection rather than
	// dropping the member is that something can say why, at the line it was declared on.
	const vh_export_desc* TargetExport = FindExport("Target");
	ExportsOk = Step("an optional node names the class its slot accepts",
					TargetExport && TargetExport->Hint == VH_EXPORT_HINT_CLASS
						&& TextOf(TargetExport->HintStringUtf8, TargetExport->HintStringLen) == "node2d"
						&& TextOf(TargetExport->NativeClassUtf8, TargetExport->NativeClassLen) == "node2d"
						&& TargetExport->VariantTag == VH_VARIANT_OBJECT)
			 && ExportsOk;
	const vh_export_desc* HeldExport = FindExport("Held");
	ExportsOk = Step("a node without an option around it is refused",
					HeldExport && HeldExport->Reject == VH_EXPORT_OBJECT_NOT_OPTIONAL)
			 && ExportsOk;
	ExportsOk = Step("an optional node exports", TargetExport && TargetExport->Reject == VH_EXPORT_OK) && ExportsOk;
	const vh_export_desc* MaybeExport = FindExport("Maybe");
	ExportsOk = Step("and an option around a number is refused the other way",
					MaybeExport && MaybeExport->Reject == VH_EXPORT_OPTION_NOT_OBJECT)
			 && ExportsOk;
	ExportsOk = Step("a member that exports says so", SpeedExport && SpeedExport->Reject == VH_EXPORT_OK) && ExportsOk;

	// A reference to one of the project's own classes is a different hint, because the two names
	// resolve through different tables: a mirrored name is in the generated API and this one is not.
	const vh_export_desc* FriendExport = FindExport("Friend");
	ExportsOk = Step("a reference to a registered script class carries its own hint",
					FriendExport && FriendExport->Hint == VH_EXPORT_HINT_SCRIPT_CLASS
						&& TextOf(FriendExport->HintStringUtf8, FriendExport->HintStringLen) == "exports_probe"
						&& FriendExport->VariantTag == VH_VARIANT_OBJECT
						&& FriendExport->Reject == VH_EXPORT_OK)
			 && ExportsOk;
	// Without this the slot cannot be drawn: ClassDB has never heard of the name exports_probe
	// registered, so only the mirrored class it derives from says it is a node and not a resource.
	ExportsOk = Step("and the mirrored class that says whether it is a node",
					FriendExport && TextOf(FriendExport->NativeClassUtf8, FriendExport->NativeClassLen) == "node2d")
			 && ExportsOk;
	// A struct crosses as the numbers it is made of, tagged with which Godot type to rebuild from
	// them -- the refusal Epic's `editable` gave these ("not concrete") is the reason the attribute
	// had to be ours.
	const vh_export_desc* OffsetExport = FindExport("Offset");
	ExportsOk = Step("a vector2 exports as a Vector2",
					OffsetExport && OffsetExport->Type == VH_TYPE_TUPLE
						&& OffsetExport->VariantTag == VH_VARIANT_VECTOR2
						&& OffsetExport->Reject == VH_EXPORT_OK)
			 && ExportsOk;
	const vh_export_desc* TintExport = FindExport("Tint");
	ExportsOk = Step("and a color as a Color",
					TintExport && TintExport->VariantTag == VH_VARIANT_COLOR
						&& TintExport->Reject == VH_EXPORT_OK)
			 && ExportsOk;

	// An array becomes the packed container Godot has for its element, and a plain Array where it has
	// none -- which among the elements that can cross is only `logic`, so that one alone has to say
	// what it holds. An int goes to the 64-bit packed array: a Verse int is 64 bits wide.
	const auto ArrayTag = [&](const char* Name) {
		const vh_export_desc* Export = FindExport(Name);
		return Export && Export->Reject == VH_EXPORT_OK && Export->Type == VH_TYPE_ARRAY ? Export->VariantTag : -1;
	};
	ExportsOk = Step("[]float exports as a PackedFloat64Array", ArrayTag("Speeds") == VH_VARIANT_PACKED_FLOAT64_ARRAY) && ExportsOk;
	ExportsOk = Step("[]int as a PackedInt64Array, not the 32-bit one", ArrayTag("Counts") == VH_VARIANT_PACKED_INT64_ARRAY) && ExportsOk;
	ExportsOk = Step("[]string as a PackedStringArray", ArrayTag("Names") == VH_VARIANT_PACKED_STRING_ARRAY) && ExportsOk;
	ExportsOk = Step("[]vector2 as a PackedVector2Array", ArrayTag("Path") == VH_VARIANT_PACKED_VECTOR2_ARRAY) && ExportsOk;
	const vh_export_desc* FlagsExport = FindExport("Flags");
	ExportsOk = Step("[]logic as an Array that names its element type",
					FlagsExport && FlagsExport->Reject == VH_EXPORT_OK
						&& FlagsExport->VariantTag == VH_VARIANT_ARRAY
						&& FlagsExport->ElementVariantTag == VH_VARIANT_BOOL)
			 && ExportsOk;
	ExportsOk = Step("a packed array says nothing about its element, having said it in its own tag",
					FindExport("Speeds") && FindExport("Speeds")->ElementVariantTag == VH_VARIANT_NIL)
			 && ExportsOk;

	// An enum crosses as the ordinal Godot stores, with the enumerators as the choices its dropdown
	// offers -- in declaration order, because the ordinal indexes into that order.
	const vh_export_desc* ModeExport = FindExport("Mode");
	ExportsOk = Step("an enum exports as an int and names its enumerators",
					ModeExport && ModeExport->Reject == VH_EXPORT_OK
						&& ModeExport->Type == VH_TYPE_INT && ModeExport->VariantTag == VH_VARIANT_INT
						&& ModeExport->Hint == VH_EXPORT_HINT_ENUM
						&& TextOf(ModeExport->HintStringUtf8, ModeExport->HintStringLen) == "Idle,Walking,Running")
			 && ExportsOk;

	const vh_export_desc* StrangerExport = FindExport("Stranger");
	ExportsOk = Step("a reference to an unregistered one is refused, and says which it was",
					StrangerExport && StrangerExport->Reject == VH_EXPORT_SCRIPT_CLASS_NOT_GLOBAL
						&& TextOf(StrangerExport->HintStringUtf8, StrangerExport->HintStringLen) == "exports_unregistered")
			 && ExportsOk;

	// The location is what a consumer needs to put a rejection where the author can see it.
	ExportsOk = Step("a harvested member carries where it was declared",
					HeldExport && HeldExport->Line >= 0 && HeldExport->Column >= 0)
			 && ExportsOk;

	// Defaults come off the CDO, whose Verse constructor has already run -- the semantic program
	// can only say that an initializer exists, not what it evaluates to.
	const vh_value* SpeedDefault = nullptr;
	const bool SpeedRead = ClassDefaultFieldFn("exports", "Speed", &SpeedDefault) == VH_OK && SpeedDefault != nullptr;
	Step("vh_class_default_field Speed", SpeedRead);
	Step("Speed defaults to 60.0", SpeedRead && SpeedDefault->Type == VH_TYPE_FLOAT && SpeedDefault->Float == 60.0);

	const vh_value* ScaleDefault = nullptr;
	const bool ScaleRead = ClassDefaultFieldFn("exports", "Scale", &ScaleDefault) == VH_OK && ScaleDefault != nullptr;
	Step("vh_class_default_field Scale (var float)", ScaleRead);
	Step("Scale defaults to 1.5", ScaleRead && ScaleDefault->Type == VH_TYPE_FLOAT && ScaleDefault->Float == 1.5);

	const vh_value* EnabledDefault = nullptr;
	const bool EnabledRead = ClassDefaultFieldFn("exports", "Enabled", &EnabledDefault) == VH_OK && EnabledDefault != nullptr;
	Step("vh_class_default_field Enabled (var logic)", EnabledRead);

	const vh_value* LabelDefault = nullptr;
	const bool LabelRead = ClassDefaultFieldFn("exports", "Label", &LabelDefault) == VH_OK && LabelDefault != nullptr;
	Step("vh_class_default_field Label", LabelRead);
	Step("Label defaults to \"hello\"",
		 LabelRead && LabelDefault->Type == VH_TYPE_STRING &&
			 std::string(LabelDefault->String.Utf8, LabelDefault->String.Len) == "hello");

	Step("absent field reports not found", ClassDefaultFieldFn("exports", "NoSuchMember", &SpeedDefault) == VH_ERR_NOT_FOUND);

	// Write path, against a live instance. Handle 1 is never dereferenced here -- the smoke
	// harness answers every Godot callback with a stub -- and reading a data member never
	// consults it.
	vh_instance* Instance = nullptr;
	if (Step("vh_instantiate exports", InstantiateFn("exports", 1, &Instance) == VH_OK && Instance != nullptr))
	{
		auto RoundTrip = [&](const char* Name, const vh_value& In, auto Check) {
			if (SetFieldFn(Instance, Name, &In) != VH_OK)
			{
				return false;
			}
			const vh_value* Out = nullptr;
			return GetFieldFn(Instance, Name, &Out) == VH_OK && Out != nullptr && Check(*Out);
		};

		vh_value NewFloat{};
		NewFloat.Type = VH_TYPE_FLOAT;
		NewFloat.Float = 10.0;
		Step("set/get float round-trips on a var member",
			 RoundTrip("Scale", NewFloat, [](const vh_value& V) { return V.Type == VH_TYPE_FLOAT && V.Float == 10.0; }));

		// Speed has no `var`, so this is the one window in which it may be given a value: the
		// instance is unsealed until the first call into it, and nothing has run that could have
		// read the declared default.
		vh_value NewSpeed{};
		NewSpeed.Type = VH_TYPE_FLOAT;
		NewSpeed.Float = 3.0;
		Step("a non-var member can be initialized before the instance seals",
			 RoundTrip("Speed", NewSpeed, [](const vh_value& V) { return V.Type == VH_TYPE_FLOAT && V.Float == 3.0; }));

		vh_value NewEnabled{};
		NewEnabled.Type = VH_TYPE_LOGIC;
		NewEnabled.Logic = 0;
		Step("set/get logic round-trips on a var member",
			 RoundTrip("Enabled", NewEnabled, [](const vh_value& V) { return V.Type == VH_TYPE_LOGIC && V.Logic == 0; }));

		vh_value NewLabel{};
		NewLabel.Type = VH_TYPE_STRING;
		NewLabel.String.Utf8 = "changed";
		NewLabel.String.Len = 7;
		Step("set/get string round-trips",
			 RoundTrip("Label", NewLabel, [](const vh_value& V) {
				 return V.Type == VH_TYPE_STRING && std::string(V.String.Utf8, V.String.Len) == "changed";
			 }));

		Step("setting an absent member reports not found", SetFieldFn(Instance, "NoSuchMember", &NewFloat) == VH_ERR_NOT_FOUND);

		// A reference. The handle is never dereferenced -- every Godot callback here is a stub that
		// reports a dead object -- and nothing below needs one to be alive: what is being checked is
		// that a handle becomes a Verse wrapper of the declared class and reads back as itself.
		vh_value NewTarget{};
		NewTarget.Type = VH_TYPE_INT;
		NewTarget.VariantTag = VH_VARIANT_OBJECT;
		NewTarget.Int = 4242;
		Step("set/get an optional node round-trips as its handle",
			 RoundTrip("Target", NewTarget, [](const vh_value& V) {
				 return V.Type == VH_TYPE_INT && V.VariantTag == VH_VARIANT_OBJECT && V.Int == 4242;
			 }));

		// The empty case is a different cell, not a handle of a different value -- and it is the
		// same cell Verse spells `logic` false with, so reading it back as an option rather than as
		// false is the whole of what says the declared type was consulted.
		vh_value NoTarget{};
		NoTarget.Type = VH_TYPE_INT;
		NoTarget.VariantTag = VH_VARIANT_OBJECT;
		NoTarget.Int = 0;
		Step("a null reference reads back as the empty option, not as false",
			 RoundTrip("Target", NoTarget, [](const vh_value& V) {
				 return V.Type == VH_TYPE_OPTION && V.VariantTag == VH_VARIANT_OBJECT && V.Option == nullptr;
			 }));

		// ... while a logic member holding false still reads as logic, which is the half of that
		// distinction a wrong answer here would break silently.
		vh_value FalseLogic{};
		FalseLogic.Type = VH_TYPE_LOGIC;
		FalseLogic.Logic = 0;
		Step("and a logic member holding false still reads as logic",
			 RoundTrip("Enabled", FalseLogic, [](const vh_value& V) { return V.Type == VH_TYPE_LOGIC && V.Logic == 0; }));

		// A struct reads and writes as a tuple of its components, in Godot's order.
		const vh_value* OffsetValue = nullptr;
		Step("a struct reads as its components",
			 GetFieldFn(Instance, "Offset", &OffsetValue) == VH_OK && OffsetValue != nullptr
				 && OffsetValue->Type == VH_TYPE_TUPLE && OffsetValue->VariantTag == VH_VARIANT_VECTOR2
				 && OffsetValue->Seq.Count == 2 && OffsetValue->Seq.Items[0].Float == 1.0
				 && OffsetValue->Seq.Items[1].Float == 2.0);

		vh_value NewOffsetItems[2]{};
		NewOffsetItems[0].Type = VH_TYPE_FLOAT;
		NewOffsetItems[0].Float = 7.5;
		NewOffsetItems[1].Type = VH_TYPE_FLOAT;
		NewOffsetItems[1].Float = -3.0;
		vh_value NewOffset{};
		NewOffset.Type = VH_TYPE_TUPLE;
		NewOffset.VariantTag = VH_VARIANT_VECTOR2;
		NewOffset.Seq.Items = NewOffsetItems;
		NewOffset.Seq.Count = 2;
		Step("set/get a struct round-trips",
			 RoundTrip("Offset", NewOffset, [](const vh_value& V) {
				 return V.Type == VH_TYPE_TUPLE && V.Seq.Count == 2 && V.Seq.Items[0].Float == 7.5
					 && V.Seq.Items[1].Float == -3.0;
			 }));
		Step("a struct written with the wrong number of components is refused",
			 [&] {
				 vh_value Short = NewOffset;
				 Short.Seq.Count = 1;
				 return SetFieldFn(Instance, "Offset", &Short) == VH_ERR_NOT_FOUND;
			 }());

		const vh_value* TintValue = nullptr;
		Step("a four-component struct reads in Godot's order",
			 GetFieldFn(Instance, "Tint", &TintValue) == VH_OK && TintValue != nullptr
				 && TintValue->Seq.Count == 4 && TintValue->Seq.Items[0].Float == 0.25
				 && TintValue->Seq.Items[2].Float == 0.75);

		// Arrays, one per container. The values written are read back through the ABI here and read
		// again from Verse further down, which is the half that can fail.
		const vh_value* SpeedsValue = nullptr;
		Step("an array reads as its elements",
			 GetFieldFn(Instance, "Speeds", &SpeedsValue) == VH_OK && SpeedsValue != nullptr
				 && SpeedsValue->Type == VH_TYPE_ARRAY
				 && SpeedsValue->VariantTag == VH_VARIANT_PACKED_FLOAT64_ARRAY
				 && SpeedsValue->Seq.Count == 2 && SpeedsValue->Seq.Items[1].Float == 2.0);

		const vh_value* NamesValue = nullptr;
		Step("a string array reads as strings",
			 GetFieldFn(Instance, "Names", &NamesValue) == VH_OK && NamesValue != nullptr
				 && NamesValue->Seq.Count == 2
				 && std::string(NamesValue->Seq.Items[1].String.Utf8, NamesValue->Seq.Items[1].String.Len) == "bc");

		const vh_value* PathValue = nullptr;
		Step("an array of structs reads as a tuple per element",
			 GetFieldFn(Instance, "Path", &PathValue) == VH_OK && PathValue != nullptr
				 && PathValue->VariantTag == VH_VARIANT_PACKED_VECTOR2_ARRAY && PathValue->Seq.Count == 1
				 && PathValue->Seq.Items[0].Type == VH_TYPE_TUPLE
				 && PathValue->Seq.Items[0].Seq.Count == 2
				 && PathValue->Seq.Items[0].Seq.Items[1].Float == 2.0);

		vh_value FloatItems[3]{};
		for (int32_t Index = 0; Index < 3; ++Index)
		{
			FloatItems[Index].Type = VH_TYPE_FLOAT;
			FloatItems[Index].Float = 10.0 + Index;
		}
		vh_value NewSpeeds{};
		NewSpeeds.Type = VH_TYPE_ARRAY;
		NewSpeeds.VariantTag = VH_VARIANT_PACKED_FLOAT64_ARRAY;
		NewSpeeds.Seq.Items = FloatItems;
		NewSpeeds.Seq.Count = 3;
		Step("set/get an array round-trips, length and all",
			 RoundTrip("Speeds", NewSpeeds, [](const vh_value& V) {
				 return V.Seq.Count == 3 && V.Seq.Items[0].Float == 10.0 && V.Seq.Items[2].Float == 12.0;
			 }));

		vh_value LogicItems[2]{};
		LogicItems[0].Type = VH_TYPE_LOGIC;
		LogicItems[0].Logic = 1;
		LogicItems[1].Type = VH_TYPE_LOGIC;
		LogicItems[1].Logic = 1;
		vh_value NewFlags{};
		NewFlags.Type = VH_TYPE_ARRAY;
		NewFlags.VariantTag = VH_VARIANT_ARRAY;
		NewFlags.Seq.Items = LogicItems;
		NewFlags.Seq.Count = 2;
		Step("a logic array round-trips as logic",
			 RoundTrip("Flags", NewFlags, [](const vh_value& V) {
				 return V.Seq.Count == 2 && V.Seq.Items[0].Type == VH_TYPE_LOGIC && V.Seq.Items[0].Logic != 0;
			 }));

		vh_value EmptyArray{};
		EmptyArray.Type = VH_TYPE_ARRAY;
		EmptyArray.VariantTag = VH_VARIANT_PACKED_STRING_ARRAY;
		EmptyArray.Seq.Items = nullptr;
		EmptyArray.Seq.Count = 0;
		Step("an emptied array reads back empty rather than as a string",
			 RoundTrip("Names", EmptyArray, [](const vh_value& V) {
				 return V.Type == VH_TYPE_ARRAY && V.Seq.Count == 0;
			 }));

		// Written back to what it started as, because ReadArrays below reads these elements.
		vh_value StringItems[2]{};
		StringItems[0].Type = VH_TYPE_STRING;
		StringItems[0].String.Utf8 = "a";
		StringItems[0].String.Len = 1;
		StringItems[1].Type = VH_TYPE_STRING;
		StringItems[1].String.Utf8 = "bc";
		StringItems[1].String.Len = 2;
		vh_value NewNames{};
		NewNames.Type = VH_TYPE_ARRAY;
		NewNames.VariantTag = VH_VARIANT_PACKED_STRING_ARRAY;
		NewNames.Seq.Items = StringItems;
		NewNames.Seq.Count = 2;
		Step("a string array round-trips",
			 RoundTrip("Names", NewNames, [](const vh_value& V) {
				 return V.Seq.Count == 2
					 && std::string(V.Seq.Items[1].String.Utf8, V.Seq.Items[1].String.Len) == "bc";
			 }));

		vh_value PathFields[2]{};
		PathFields[0].Type = VH_TYPE_FLOAT;
		PathFields[0].Float = 4.0;
		PathFields[1].Type = VH_TYPE_FLOAT;
		PathFields[1].Float = 5.0;
		vh_value PathItems[1]{};
		PathItems[0].Type = VH_TYPE_TUPLE;
		PathItems[0].VariantTag = VH_VARIANT_VECTOR2;
		PathItems[0].Seq.Items = PathFields;
		PathItems[0].Seq.Count = 2;
		vh_value NewPath{};
		NewPath.Type = VH_TYPE_ARRAY;
		NewPath.VariantTag = VH_VARIANT_PACKED_VECTOR2_ARRAY;
		NewPath.Seq.Items = PathItems;
		NewPath.Seq.Count = 1;
		Step("an array of structs round-trips",
			 RoundTrip("Path", NewPath, [](const vh_value& V) {
				 return V.Seq.Count == 1 && V.Seq.Items[0].Seq.Count == 2
					 && V.Seq.Items[0].Seq.Items[0].Float == 4.0
					 && V.Seq.Items[0].Seq.Items[1].Float == 5.0;
			 }));

		// An enum reads as the ordinal of the enumerator it holds, and is written with one.
		const vh_value* ModeValue = nullptr;
		Step("an enum reads as its ordinal",
			 GetFieldFn(Instance, "Mode", &ModeValue) == VH_OK && ModeValue != nullptr
				 && ModeValue->Type == VH_TYPE_INT && ModeValue->Int == 1);

		vh_value NewMode{};
		NewMode.Type = VH_TYPE_INT;
		NewMode.Int = 2;
		Step("set/get an enum round-trips",
			 RoundTrip("Mode", NewMode, [](const vh_value& V) { return V.Type == VH_TYPE_INT && V.Int == 2; }));

		// A scene saved against a longer version of the enum carries an ordinal the enumeration no
		// longer has, and GetEnumeratorChecked dies on one -- so it is refused here.
		vh_value PastTheEnd{};
		PastTheEnd.Type = VH_TYPE_INT;
		PastTheEnd.Int = 3;
		Step("an ordinal past the last enumerator is refused, not clamped",
			 SetFieldFn(Instance, "Mode", &PastTheEnd) == VH_ERR_NOT_FOUND);
		Step("and the refused write left the enumerator alone",
			 GetFieldFn(Instance, "Mode", &ModeValue) == VH_OK && ModeValue != nullptr && ModeValue->Int == 2);

		// A member typed as one of the project's own classes refuses a handle: the object it should
		// hold already exists, and vh_instance_set_field_instance is how it is handed over.
		Step("a script-class reference refuses a bare handle",
			 SetFieldFn(Instance, "Friend", &NewTarget) == VH_ERR_NOT_FOUND);

		vh_instance* Friend = nullptr;
		if (Step("vh_instantiate exports_probe", InstantiateFn("exports_probe", 2, &Friend) == VH_OK && Friend != nullptr))
		{
			Step("a script-class reference takes another instance",
				 SetFieldInstanceFn(Instance, "Friend", Friend) == VH_OK);
			const vh_value* FriendValue = nullptr;
			Step("and reads back as that node's handle",
				 GetFieldFn(Instance, "Friend", &FriendValue) == VH_OK && FriendValue != nullptr
					 && FriendValue->Type == VH_TYPE_INT && FriendValue->Int == 2);
			Step("a null clears it", SetFieldInstanceFn(Instance, "Friend", nullptr) == VH_OK);

			// exports_probe is a node2d, so it is a value a `?node2d` may hold -- and holding the
			// object that already exists beats building a second wrapper around its handle.
			Step("a mirrored reference takes an instance too",
				 SetFieldInstanceFn(Instance, "Target", Friend) == VH_OK);
			// Maybe is a ?float, which no object is.
			Step("a member that is not a reference refuses one",
				 SetFieldInstanceFn(Instance, "Maybe", Friend) == VH_ERR_NOT_FOUND);
			ReleaseInstanceFn(Friend);
		}


		// The round-trips above cannot see a value written in the wrong representation -- a bad
		// write and a matching bad read agree. Bump reads and assigns each member from Verse, so
		// the interpreter is the one checking, and a var whose reference was overwritten dies
		// here rather than reporting a plausible number.
		auto Read = [&](const char* Name) {
			const vh_value* Out = nullptr;
			return GetFieldFn(Instance, Name, &Out) == VH_OK && Out != nullptr ? Out : nullptr;
		};

		const bool BumpOk = CallVoid(InstanceCallFn, Instance, "(/user@localhost/exports:)Bump") == VH_OK;
		Step("Verse can read and assign the members it was handed", BumpOk);
		const vh_value* ScaleAfterBump = Read("Scale");
		Step("Verse read both a var and a non-var it was handed",
			 BumpOk && ScaleAfterBump && ScaleAfterBump->Type == VH_TYPE_FLOAT && ScaleAfterBump->Float == 13.0);
		const vh_value* EnabledAfterBump = Read("Enabled");
		Step("a Verse assignment to a logic var is visible across the ABI",
			 BumpOk && EnabledAfterBump && EnabledAfterBump->Type == VH_TYPE_LOGIC && EnabledAfterBump->Logic != 0);

		// Separate from Bump because a string var is stored as a mutable container rather than as
		// a box around an immutable one, so it is the case a scalar var cannot stand in for.
		const bool BumpLabelOk = CallVoid(InstanceCallFn, Instance, "(/user@localhost/exports:)BumpLabel") == VH_OK;
		Step("Verse can read and assign a string var", BumpLabelOk);
		const vh_value* LabelAfterBump = Read("Label");
		Step("a Verse assignment to a string var is visible across the ABI",
			 BumpLabelOk && LabelAfterBump && LabelAfterBump->Type == VH_TYPE_STRING &&
				 std::string(LabelAfterBump->String.Utf8, LabelAfterBump->String.Len) == "changed!");

		// And what the interpreter makes of a written reference, which is the only check that
		// counts for the same reason: a reference in the wrong representation round-trips through
		// the ABI perfectly and dies inside the VM. Target is a var, so this still works sealed.
		auto VerseSeesTarget = [&](const vh_value& Written) {
			if (SetFieldFn(Instance, "Target", &Written) != VH_OK
				|| CallVoid(InstanceCallFn, Instance, "(/user@localhost/exports:)ReadTarget") != VH_OK)
			{
				return -1;
			}
			const vh_value* Seen = nullptr;
			if (GetFieldFn(Instance, "TargetSeen", &Seen) != VH_OK || Seen == nullptr || Seen->Type != VH_TYPE_LOGIC)
			{
				return -1;
			}
			return Seen->Logic != 0 ? 1 : 0;
		};
		Step("Verse unwraps a reference it was handed and dispatches on it", VerseSeesTarget(NewTarget) == 1);
		Step("and sees the empty case as empty", VerseSeesTarget(NoTarget) == 0);

		// And the same for a struct: reading a field goes through the object's own shape, so one
		// built with the wrong emergent type fails here rather than reading back as what went in.
		const bool ReadOffsetOk = CallVoid(InstanceCallFn, Instance, "(/user@localhost/exports:)ReadOffset") == VH_OK;
		const vh_value* OffsetSeen = Read("OffsetSeen");
		Step("Verse reads a field off a struct it was handed",
			 ReadOffsetOk && OffsetSeen && OffsetSeen->Type == VH_TYPE_FLOAT
				 && OffsetSeen->Float == 7.5 + 0.75);

		// And for every array. Indexing reads an element through the container's own storage kind, so
		// an array built as the wrong kind fails here; the []vector2 goes on to read a field off an
		// element, which checks two layers of construction at once.
		//
		// Speeds[1] = 11.0, Counts[0] = 7 (never written), Names[1] = "bc" scores 100, Flags[0] true
		// scores 1000, Path[0].Y = 2.0.
		auto VerseReadsArray = [&](const char* Function) {
			if (CallVoid(InstanceCallFn, Instance, Function) != VH_OK)
			{
				return -2.0;
			}
			const vh_value* Seen = Read("ArraysSeen");
			return Seen && Seen->Type == VH_TYPE_FLOAT ? Seen->Float : -2.0;
		};
		Step("Verse indexes a float array it was handed",
			 VerseReadsArray("(/user@localhost/exports:)ReadSpeeds") == 11.0);
		Step("and one it was not, which must read as it always did",
			 VerseReadsArray("(/user@localhost/exports:)ReadCounts") == 7.0);
		Step("and a string array", VerseReadsArray("(/user@localhost/exports:)ReadNames") == 100.0);
		Step("and a logic array", VerseReadsArray("(/user@localhost/exports:)ReadFlags") == 1000.0);
		Step("and reads a field off an element of a struct array it was handed",
			 VerseReadsArray("(/user@localhost/exports:)ReadPath") == 5.0);

		// An enumerator is a value of its own rather than a number, so comparing one from Verse is
		// what says the write put the enumeration's own enumerator in the slot.
		Step("Verse compares the enumerator it was handed against its own",
			 VerseReadsArray("(/user@localhost/exports:)ReadMode") == 2.0);

		// Bump sealed the instance. From here Verse has observed the members, so the author's
		// `var` is the whole of what may still change.
		Step("writing a non-var member is refused once the instance is sealed",
			 SetFieldFn(Instance, "Speed", &NewFloat) == VH_ERR_NOT_FOUND);
		const vh_value* SpeedAfter = Read("Speed");
		Step("the refused write left the value alone",
			 SpeedAfter && SpeedAfter->Type == VH_TYPE_FLOAT && SpeedAfter->Float == 3.0);
		Step("a var member is still writable once the instance is sealed",
			 RoundTrip("Scale", NewFloat, [](const vh_value& V) { return V.Type == VH_TYPE_FLOAT && V.Float == 10.0; }));

		// --- ABI v2: general dispatch (R-NODE-6) ---------------------------------------------
		//
		// v1 had two call shapes and a three-name array. Everything below goes through the one
		// entry point, so a failure here is the dispatch core rather than a fixture.
		{
			auto Call = [&](const char* Decorated, const vh_value* Args, int32_t ArgCount, vh_value& Result) {
				return InstanceCallFn(Instance, Decorated, Args, ArgCount, nullptr, &Result);
			};

			vh_value IntArgs[2] = {};
			IntArgs[0].Type = VH_TYPE_INT;
			IntArgs[0].Int = 17;
			IntArgs[1].Type = VH_TYPE_INT;
			IntArgs[1].Int = 25;
			vh_value Result{};
			Step("a method taking two ints and returning one is callable",
				 Call("(/user@localhost/exports:)AddInts(:int,:int)", IntArgs, 2, Result) == VH_OK
					 && Result.Type == VH_TYPE_INT && Result.Int == 42);

			vh_value FloatArgs[2] = {};
			FloatArgs[0].Type = VH_TYPE_FLOAT;
			FloatArgs[0].Float = 1.5;
			FloatArgs[1].Type = VH_TYPE_FLOAT;
			FloatArgs[1].Float = 4.0;
			Step("and floats",
				 Call("(/user@localhost/exports:)ScaleFloat(:float,:float)", FloatArgs, 2, Result) == VH_OK
					 && Result.Type == VH_TYPE_FLOAT && Result.Float == 6.0);

			// An int where a float is declared. Godot spells 0 as an integer Variant whatever the
			// receiving type, so refusing this would make half of GDScript's literals uncallable.
			vh_value MixedArgs[2] = {};
			MixedArgs[0].Type = VH_TYPE_INT;
			MixedArgs[0].Int = 3;
			MixedArgs[1].Type = VH_TYPE_FLOAT;
			MixedArgs[1].Float = 2.0;
			Step("an int argument widens into a float parameter",
				 Call("(/user@localhost/exports:)ScaleFloat(:float,:float)", MixedArgs, 2, Result) == VH_OK
					 && Result.Type == VH_TYPE_FLOAT && Result.Float == 6.0);

			vh_value StringArgs[2] = {};
			StringArgs[0].Type = VH_TYPE_STRING;
			StringArgs[0].String.Utf8 = "<";
			StringArgs[0].String.Len = 1;
			StringArgs[1].Type = VH_TYPE_STRING;
			StringArgs[1].String.Utf8 = ">";
			StringArgs[1].String.Len = 1;
			Step("and strings, in and out",
				 Call("(/user@localhost/exports:)Decorate(:[]char,:[]char)", StringArgs, 2, Result) == VH_OK
					 && Result.Type == VH_TYPE_STRING
					 && std::string(Result.String.Utf8, Result.String.Len) == "<changed!>");

			vh_value LogicArg{};
			LogicArg.Type = VH_TYPE_LOGIC;
			LogicArg.Logic = 1;
			Step("and logic",
				 Call("(/user@localhost/exports:)Negate(:logic)", &LogicArg, 1, Result) == VH_OK
					 && Result.Type == VH_TYPE_LOGIC && Result.Logic == 0);

			// No arguments but a result, which v1 had no shape for at all.
			Step("a method with no arguments still returns its value",
				 Call("(/user@localhost/exports:)CurrentScale", nullptr, 0, Result) == VH_OK
					 && Result.Type == VH_TYPE_FLOAT && Result.Float == 10.0);

			// A <decides> method, both ways. The distinction from VH_ERR_NOT_FOUND is the point:
			// one ran and declined, the other was never there.
			vh_value DecideArgs[2] = {};
			DecideArgs[0].Type = VH_TYPE_INT;
			DecideArgs[0].Int = 5;
			DecideArgs[1].Type = VH_TYPE_INT;
			DecideArgs[1].Int = 3;
			Step("a <decides> method that succeeds returns its value",
				 Call("(/user@localhost/exports:)NotBelow(:int,:int)", DecideArgs, 2, Result) == VH_OK
					 && Result.Type == VH_TYPE_INT && Result.Int == 5);
			DecideArgs[0].Int = 1;
			Step("and one that declines answers VH_ERR_FAILED, not VH_ERR_NOT_FOUND",
				 Call("(/user@localhost/exports:)NotBelow(:int,:int)", DecideArgs, 2, Result) == VH_ERR_FAILED);

			Step("too few arguments is refused without running anything",
				 Call("(/user@localhost/exports:)AddInts(:int,:int)", IntArgs, 1, Result) == VH_ERR_ARGUMENT);
			Step("a method the class does not declare is VH_ERR_NOT_FOUND",
				 Call("(/user@localhost/exports:)NoSuchMethod", nullptr, 0, Result) == VH_ERR_NOT_FOUND);
		}

		// --- R-DIAG-2: a runtime error names a file and a line -------------------------------
		{
			const int ErrorsBefore = RuntimeErrorCount;
			// Target holds a handle the harness reports dead, so reaching through it raises.
			SetFieldFn(Instance, "Target", &NewTarget);
			const int32_t Status = CallVoid(InstanceCallFn, Instance, "(/user@localhost/exports:)TouchTarget");
			Step("a method that raises answers VH_ERR_RUNTIME", Status == VH_ERR_RUNTIME);
			Step("and the error reached the runtime error callback", RuntimeErrorCount > ErrorsBefore);
			// The raise happens inside the mirrored QueueFree, so that is the innermost located
			// frame -- which is correct, and is not where the author's mistake is.
			Step("naming the .verse file it was raised in",
				 LastRuntimeErrorPath.find(".verse") != std::string::npos);
			Step("and a line inside it", LastRuntimeErrorLine > 0);
			// The script's own frame is what the author needs, and it is further out. A stack that
			// stopped at the raise site would point every dead-object error at the same mirror line.
			Step("and a stack that reaches the script's own method",
				 LastRuntimeErrorStack.find("TouchTarget") != std::string::npos);
			Step("and the file that method is declared in",
				 LastRuntimeErrorStack.find("exports.verse") != std::string::npos);

			// --- R-DIAG-3 / R-ASYNC-4: the raise stops the call that raised, and nothing else ---
			//
			// A raised runtime error terminates the *active* content scope, and the VM then
			// declines to run anything in that scope at all -- which used to be invisible, because
			// a call reported success having not run and a read reported "no such member".
			//
			// Until Phase 5 that scope was the whole project's, so one script's first mistake
			// stopped every script until the next `vh_tick`. Now it is this instance's, and the
			// instance gets a *fresh* scope at its next call rather than an un-terminated one at
			// the next frame boundary -- so everything below runs in the same frame as the raise.
			vh_value IntArgs[2] = {};
			IntArgs[0].Type = VH_TYPE_INT;
			IntArgs[0].Int = 17;
			IntArgs[1].Type = VH_TYPE_INT;
			IntArgs[1].Int = 25;
			vh_value Result{};
			Step("the very next call, in the same frame, runs and returns its value",
				 InstanceCallFn(Instance, "(/user@localhost/exports:)AddInts(:int,:int)", IntArgs, 2, nullptr, &Result) == VH_OK
					 && Result.Type == VH_TYPE_INT && Result.Int == 42);

			// The half that matters most, and the half nobody checked when the project-wide rule
			// was first written up: a void method's *effect*, not just its result.
			const vh_value* BeforeBump = Read("Scale");
			const double ScaleBeforeBump = BeforeBump ? BeforeBump->Float : -1.0;
			Step("and a void method has its effect",
				 CallVoid(InstanceCallFn, Instance, "(/user@localhost/exports:)Bump") == VH_OK);
			const vh_value* AfterBump = Read("Scale");
			Step("which the member shows",
				 AfterBump != nullptr && AfterBump->Float != ScaleBeforeBump);

			vh_value Ninety{};
			Ninety.Type = VH_TYPE_FLOAT;
			Ninety.Float = 90.0;
			Step("a write lands rather than being refused", SetFieldFn(Instance, "Scale", &Ninety) == VH_OK);

			// A read is a question rather than script code, and always was answered.
			const vh_value* ScaleAfterRaise = Read("Scale");
			Step("and a read answers, because it runs no script code",
				 ScaleAfterRaise != nullptr && ScaleAfterRaise->Type == VH_TYPE_FLOAT);

			vh_instance* AfterRaise = nullptr;
			Step("and another class instantiates in the same frame",
				 InstantiateFn("exports_probe", 9, &AfterRaise) == VH_OK && AfterRaise != nullptr);
			ReleaseInstanceFn(AfterRaise);

			TickFn(0.004, nullptr);
			Step("ticking after all that changes nothing, because nothing was waiting for it",
				 InstanceCallFn(Instance, "(/user@localhost/exports:)AddInts(:int,:int)", IntArgs, 2, nullptr, &Result) == VH_OK
					 && Result.Int == 42);
		}

		ReleaseInstanceFn(Instance);
	}

	// --- R-ASYNC-1/4: tasks, and what a raise costs ------------------------------------------
	//
	// The tick-loop layer. No Godot at all, which is exactly what makes this the place to pin the
	// scope behaviour: a call and a tick happen in a chosen order with nothing else running, and a
	// task that hangs shows up here rather than as a mysterious timeout in a Godot project.
	{
		auto TaskCall = [&](vh_instance* Target, const char* Decorated) {
			vh_value Ignored{};
			return InstanceCallFn(Target, Decorated, nullptr, 0, nullptr, &Ignored);
		};
		auto TaskRead = [&](vh_instance* Target, const char* Decorated) {
			vh_value Result{};
			if (InstanceCallFn(Target, Decorated, nullptr, 0, nullptr, &Result) != VH_OK
				|| Result.Type != VH_TYPE_INT)
			{
				return (int64_t)-1;
			}
			return Result.Int;
		};

		vh_instance* One = nullptr;
		vh_instance* Two = nullptr;
		const bool Made = InstantiateFn("tasks", 101, &One) == VH_OK && One != nullptr
			&& InstantiateFn("tasks", 102, &Two) == VH_OK && Two != nullptr;
		Step("two instances of the task fixture", Made);

		if (Made)
		{
			// R-ASYNC-1: the call that spawns returns VH_OK having *not* run the task to
			// completion. The task ran up to its first await and stopped there.
			Step("a call that spawns a task returns normally",
				 TaskCall(One, "(/user@localhost/tasks:)StartWait") == VH_OK);
			Step("and the task ran up to its first await",
				 TaskRead(One, "(/user@localhost/tasks:)ReadReached") == 1);
			Step("without having got past it",
				 TaskRead(One, "(/user@localhost/tasks:)ReadSeen") == 0);

			// Across a tick, which is the half of R-ASYNC-1 that a single call cannot show.
			TickFn(0.004, nullptr);
			Step("the task is still suspended after a tick",
				 TaskRead(One, "(/user@localhost/tasks:)ReadSeen") == 0);

			vh_value FireArg{};
			FireArg.Type = VH_TYPE_INT;
			FireArg.Int = 7;
			vh_value Ignored{};
			Step("and resumes inside the call that signals it",
				 InstanceCallFn(One, "(/user@localhost/tasks:)Fire(:int)", &FireArg, 1, nullptr, &Ignored) == VH_OK
					 && TaskRead(One, "(/user@localhost/tasks:)ReadSeen") == 7);

			// D4: work with no event behind it is what `vh_tick` is for. `Sleep(0.0)` means
			// "resume at the next pump", so nothing has happened until one runs.
			Step("a sleeping task has not resumed before the tick",
				 TaskCall(One, "(/user@localhost/tasks:)StartNap") == VH_OK
					 && TaskRead(One, "(/user@localhost/tasks:)ReadNapped") == 0);
			TickFn(0.004, nullptr);
			Step("and has after it",
				 TaskRead(One, "(/user@localhost/tasks:)ReadNapped") == 1);

			// **The case R-ASYNC-4 exists for.** Two instances, each with a task suspended on its
			// own event. One raises. Before Phase 5 one content scope served the whole project, so
			// the raise cancelled both -- and stopped every script until the next tick besides.
			TaskCall(One, "(/user@localhost/tasks:)StartWait");
			TaskCall(Two, "(/user@localhost/tasks:)StartWait");
			Step("a raise in one instance answers VH_ERR_RUNTIME",
				 TaskCall(One, "(/user@localhost/tasks:)Raise") == VH_ERR_RUNTIME);

			FireArg.Int = 11;
			Step("the other instance's suspended task is still there, and resumes",
				 InstanceCallFn(Two, "(/user@localhost/tasks:)Fire(:int)", &FireArg, 1, nullptr, &Ignored) == VH_OK
					 && TaskRead(Two, "(/user@localhost/tasks:)ReadSeen") == 11);

			// And the raising instance is usable again at its very next call, with a fresh scope
			// rather than a revived one (D24) -- so the task it lost stays lost and a new one runs.
			FireArg.Int = 13;
			InstanceCallFn(One, "(/user@localhost/tasks:)Fire(:int)", &FireArg, 1, nullptr, &Ignored);
			Step("while the raising instance's own task was cancelled",
				 TaskRead(One, "(/user@localhost/tasks:)ReadSeen") == 7);
			Step("and it can start another immediately",
				 TaskCall(One, "(/user@localhost/tasks:)StartWait") == VH_OK);
			FireArg.Int = 19;
			InstanceCallFn(One, "(/user@localhost/tasks:)Fire(:int)", &FireArg, 1, nullptr, &Ignored);
			Step("which runs", TaskRead(One, "(/user@localhost/tasks:)ReadSeen") == 19);

			// A raise from inside a *task* rather than from a call. Nothing returns VH_ERR_RUNTIME
			// here -- the call that spawned it is long gone -- so what is asserted is that the
			// instance survives it.
			const int ErrorsBeforeTaskRaise = RuntimeErrorCount;
			TaskCall(Two, "(/user@localhost/tasks:)StartRaiser");
			TickFn(0.004, nullptr);
			Step("a raise inside a task is reported", RuntimeErrorCount > ErrorsBeforeTaskRaise);
			Step("and the instance is callable afterwards",
				 TaskRead(Two, "(/user@localhost/tasks:)ReadSeen") == 11);

			// R-ASYNC-5: releasing the instance terminates its scope, which cancels its tasks. What
			// is asserted is that it is not an error -- a leak or a use-after-free would show as a
			// crash on the next tick rather than as a failed step.
			TaskCall(Two, "(/user@localhost/tasks:)StartWait");
			ReleaseInstanceFn(Two);
			TickFn(0.004, nullptr);
			Step("releasing an instance with a suspended task is not an error", true);
			ReleaseInstanceFn(One);
		}
	}

	// --- ABI v2: the method list (R-NODE-9) --------------------------------------------------
	{
		auto Text = [](const char* Utf8, int32_t Len) { return std::string(Utf8 ? Utf8 : "", Len); };

		const vh_method_desc* Methods = nullptr;
		int32_t MethodCount = 0;
		const bool ListOk = ClassMethodListFn("exports", &Methods, &MethodCount) == VH_OK;
		Step("vh_class_method_list", ListOk && MethodCount > 0);

		auto Find = [&](const char* Name) -> const vh_method_desc* {
			for (int32_t Index = 0; Index < MethodCount; ++Index)
			{
				if (Text(Methods[Index].NameUtf8, Methods[Index].NameLen) == Name)
				{
					return &Methods[Index];
				}
			}
			return nullptr;
		};

		const vh_method_desc* AddInts = ListOk ? Find("AddInts") : nullptr;
		Step("it reports a method the class declares", AddInts != nullptr);
		Step("with its parameters, named and typed",
			 AddInts && AddInts->ParamCount == 2 && AddInts->RequiredParamCount == 2
				 && Text(AddInts->Params[0].NameUtf8, AddInts->Params[0].NameLen) == "A"
				 && AddInts->Params[0].Type == VH_TYPE_INT
				 && Text(AddInts->Params[1].NameUtf8, AddInts->Params[1].NameLen) == "B");
		Step("and its result type", AddInts && AddInts->ResultType == VH_TYPE_INT);
		Step("and the decorated name the call takes",
			 AddInts
				 && Text(AddInts->DecoratedUtf8, AddInts->DecoratedLen)
						== "(/user@localhost/exports:)AddInts(:int,:int)");

		const vh_method_desc* Bump = ListOk ? Find("Bump") : nullptr;
		Step("a void method reports no result", Bump && Bump->ResultType == VH_TYPE_VOID);

		const vh_method_desc* NotBelow = ListOk ? Find("NotBelow") : nullptr;
		Step("a <decides> method says so", NotBelow && NotBelow->CanFail != 0);
		Step("and one that cannot fail does not", AddInts && AddInts->CanFail == 0);

		// Nothing in `exports` overrides a Godot virtual, so every method here is the script's own.
		bool AnyVirtual = false;
		for (int32_t Index = 0; ListOk && Index < MethodCount; ++Index)
		{
			AnyVirtual = AnyVirtual || Methods[Index].GodotVirtualLen > 0;
		}
		Step("a method that overrides nothing of Godot's carries no virtual name", ListOk && !AnyVirtual);

		// exports_probe overrides PhysicsProcess, which is Godot's _physics_process. The mapping is
		// derived rather than tabulated, so this is the check that the derivation is right.
		const vh_method_desc* ProbeMethods = nullptr;
		int32_t ProbeCount = 0;
		const bool ProbeOk = ClassMethodListFn("exports_probe", &ProbeMethods, &ProbeCount) == VH_OK;
		const vh_method_desc* Physics = nullptr;
		for (int32_t Index = 0; ProbeOk && Index < ProbeCount; ++Index)
		{
			if (Text(ProbeMethods[Index].NameUtf8, ProbeMethods[Index].NameLen) == "_PhysicsProcess")
			{
				Physics = &ProbeMethods[Index];
			}
		}
		Step("an override of a Godot virtual reports Godot's own name for it",
			 Physics && Text(Physics->GodotVirtualUtf8, Physics->GodotVirtualLen) == "_physics_process");

		Step("a class the project does not declare has no method list",
			 ClassMethodListFn("no_such_class", &Methods, &MethodCount) == VH_ERR_NOT_FOUND);
	}

	Step("vh_tick", true);

	// Background analysis. The editor drives this from its frame loop, so the shape that matters
	// is begin / tick+poll each frame / reap -- and the tick is the part with teeth: VerseVM
	// blocks execution for the length of a build, and a tick that ran anyway used to trip
	// `ensure(!bBlockAllExecution)` and then kill the process.
	{
		bool AsyncOk = true;
		std::string CleanSource = ReadFileUtf8(ExportsPath);
		AsyncOk = Step("vh_check_project_begin", CheckBeginFn(ExportsPathUtf8.c_str(), CleanSource.c_str()) == VH_OK) && AsyncOk;
		AsyncOk = Step("vh_check_project_busy reports the analysis in flight", CheckBusyFn() != 0) && AsyncOk;
		AsyncOk = Step("a second begin while one is in flight is refused",
					   CheckBeginFn(ExportsPathUtf8.c_str(), CleanSource.c_str()) != VH_OK) && AsyncOk;

		vh_bool Finished = 0;
		const uint64_t Deadline = GetTickCount64() + 30000;
		while (!Finished && GetTickCount64() < Deadline)
		{
			CheckProjectPollFn(&Finished);
			TickFn(0.004, nullptr);
			Sleep(1); // a frame, roughly; the analysis takes ~100ms of them
		}
		AsyncOk = Step("the analysis finished while the frame loop kept ticking", Finished != 0) && AsyncOk;
		AsyncOk = Step("ticking throughout did not block execution", CheckBusyFn() == 0) && AsyncOk;

		// A poll with nothing in flight must be a harmless no-op, since that is every other frame.
		vh_bool Spurious = 1;
		CheckProjectPollFn(&Spurious);
		AsyncOk = Step("polling with nothing in flight reports nothing", Spurious == 0) && AsyncOk;

		// Diagnostics still have to come back, and only through the poll.
		std::string BrokenSource = CleanSource + "\nthis is not verse <<<\n";
		DiagnosticCount = 0;
		if (CheckBeginFn(ExportsPathUtf8.c_str(), BrokenSource.c_str()) == VH_OK)
		{
			Finished = 0;
			const uint64_t BrokenDeadline = GetTickCount64() + 30000;
			while (!Finished && GetTickCount64() < BrokenDeadline)
			{
				CheckProjectPollFn(&Finished);
				TickFn(0.004, nullptr);
				Sleep(1);
			}
		}
		AsyncOk = Step("a broken buffer's diagnostics arrive from the poll", DiagnosticCount > 0) && AsyncOk;

		// Put the good text back, so nothing after this sees the broken parse.
		CheckProjectFn(ExportsPathUtf8.c_str(), CleanSource.c_str());

		// The demo's own script, analysed by standing in for this fixture's buffer: every type it
		// names is in this package too, and its own top-level names do not collide with the ones it
		// replaces. `demo/` is the worked example for every feature here and nothing else compiles
		// it, so without this a change that breaks it surfaces only when the editor is opened.
		const std::string DemoSource = ReadFileUtf8(VerseBase / "demo" / "scripts" / "mover.verse");
		AsyncOk = Step("the demo's mover.verse analyses",
					   !DemoSource.empty()
						   && CheckProjectFn(ExportsPathUtf8.c_str(), DemoSource.c_str()) == VH_OK)
			   && AsyncOk;
		CheckProjectFn(ExportsPathUtf8.c_str(), CleanSource.c_str());

		CallsOk = AsyncOk && CallsOk;
	}

	ShutdownFn();
	Step("vh_shutdown", true);

	return RunOk && CallsOk && LookupOk ? 0 : 1;
}
