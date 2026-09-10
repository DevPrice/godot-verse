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

static int DiagnosticCount = 0;

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

static void SmokeOnDiagnostic(void*, const vh_diagnostic* Diagnostic)
{
	++DiagnosticCount;
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
	auto OpenScriptFn = Resolve<vh_open_script_fn>(Module, "vh_open_script", &ResolveOk);
	auto HasClassFn = Resolve<vh_has_class_fn>(Module, "vh_has_class", &ResolveOk);
	auto ClassExportListFn = Resolve<vh_class_export_list_fn>(Module, "vh_class_export_list", &ResolveOk);
	auto ClassDefaultFieldFn = Resolve<vh_class_default_field_fn>(Module, "vh_class_default_field", &ResolveOk);
	auto InstantiateFn = Resolve<vh_instantiate_fn>(Module, "vh_instantiate", &ResolveOk);
	auto ReleaseInstanceFn = Resolve<vh_release_instance_fn>(Module, "vh_release_instance", &ResolveOk);
	auto CallInstanceVoidFn = Resolve<vh_instance_call_void_fn>(Module, "vh_instance_call_void", &ResolveOk);
	auto GetFieldFn = Resolve<vh_instance_get_field_fn>(Module, "vh_instance_get_field", &ResolveOk);
	auto SetFieldFn = Resolve<vh_instance_set_field_fn>(Module, "vh_instance_set_field", &ResolveOk);
	auto ReleaseScriptFn = Resolve<vh_release_script_fn>(Module, "vh_release_script", &ResolveOk);
	auto LookupSymbolFn = Resolve<vh_lookup_symbol_fn>(Module, "vh_lookup_symbol", &ResolveOk);
	auto CheckProjectFn = Resolve<vh_check_project_fn>(Module, "vh_check_project", &ResolveOk);
	auto CheckBeginFn = Resolve<vh_check_project_begin_fn>(Module, "vh_check_project_begin", &ResolveOk);
	auto CheckProjectPollFn = Resolve<vh_check_project_poll_fn>(Module, "vh_check_project_poll", &ResolveOk);
	auto CheckBusyFn = Resolve<vh_check_project_busy_fn>(Module, "vh_check_project_busy", &ResolveOk);
	auto ScriptHasFunctionFn = Resolve<vh_script_has_function_fn>(Module, "vh_script_has_function", &ResolveOk);
	auto RunMainFn = Resolve<vh_run_main_fn>(Module, "vh_run_main", &ResolveOk);
	auto CallVoidFn = Resolve<vh_call_void_fn>(Module, "vh_call_void", &ResolveOk);
	auto CallVoidFloatFn = Resolve<vh_call_void_float_fn>(Module, "vh_call_void_float", &ResolveOk);
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

	// vh_compile_project rather than vh_compile_file: Verse builds a whole package at once and
	// the host may only generate once per process, so both fixtures have to go in together.
	std::string VersePathUtf8 = VersePath.string();
	std::string ExportsPathUtf8 = ExportsPath.string();
	const char* ProjectPaths[2] = { VersePathUtf8.c_str(), ExportsPathUtf8.c_str() };
	if (!Step("vh_compile_project", CompileProjectFn(ProjectPaths, 2) == VH_OK))
	{
		ShutdownFn();
		return 1;
	}

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
		// at its name, so Speed's begins on its `@editable` -- four lines above the name. That
		// is the first `@editable` in the fixture, and it is the row a jump should land on.
		const size_t UseOffset = ExportsSource.find("+ Speed");
		const size_t DeclOffset = ExportsSource.find("@editable");
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

			// Past the end of a line nothing encloses the position, so the answer is a refusal
			// rather than whichever definition happens to span the row.
			const vh_lookup_desc* Nothing = nullptr;
			LookupOk = Step("a column past the end of a line resolves to nothing",
						   LookupSymbolFn(ExportsPathUtf8.c_str(), DeclRow, 500, &Nothing) == VH_ERR_NOT_FOUND)
					&& LookupOk;
		}
	}

	vh_script* Script = nullptr;
	if (!Step("vh_open_script", OpenScriptFn(VersePathUtf8.c_str(), &Script) == VH_OK))
	{
		ShutdownFn();
		return 1;
	}

	const char* RunArgs[1] = { VersePathUtf8.c_str() };
	int64_t ExitCode = 0;
	bool RunOk = RunMainFn(Script, RunArgs, 1, &ExitCode) == VH_OK;
	Step("vh_run_main", RunOk);
	printf("[smoke] exit code: %lld\n", static_cast<long long>(ExitCode));

	bool CallsOk = Step("vh_script_has_function Ready", ScriptHasFunctionFn(Script, "Ready") != 0);
	CallsOk = Step("vh_script_has_function Update(:float)", ScriptHasFunctionFn(Script, "Update(:float)") != 0) && CallsOk;
	CallsOk = Step("vh_call_void Ready", CallVoidFn(Script, "Ready") == VH_OK) && CallsOk;
	CallsOk = Step("vh_call_void_float Update", CallVoidFloatFn(Script, "Update(:float)", 0.016) == VH_OK) && CallsOk;
	TickFn(0.0);
	// The class-shaped half. This is the check that the verse path the host builds for a script's
	// class -- /user@localhost/<file stem> -- is the one the semantic program actually files it
	// under; everything about exports depends on that string being right.
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

	ExportsOk = Step("four members are exported", ExportCount == 4) && ExportsOk;
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

	// Hint metadata. These attributes carry a single string, which is the only attribute payload
	// SOL-972 leaves readable -- so "0.0" arrives as text and Godot parses it, not the compiler.
	auto TextOf = [](const char* Utf8, int32_t Len) { return std::string(Utf8 ? Utf8 : "", Len); };
	Step("Speed carries clamp_min", SpeedExport && TextOf(SpeedExport->ClampMinUtf8, SpeedExport->ClampMinLen) == "0.0");
	Step("Speed carries clamp_max", SpeedExport && TextOf(SpeedExport->ClampMaxUtf8, SpeedExport->ClampMaxLen) == "500.0");
	Step("Speed carries category", SpeedExport && TextOf(SpeedExport->CategoryUtf8, SpeedExport->CategoryLen) == "Movement");
	Step("Label carries no hints", FindExport("Label") && FindExport("Label")->ClampMinLen == 0 && FindExport("Label")->CategoryLen == 0);

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

		// The round-trips above cannot see a value written in the wrong representation -- a bad
		// write and a matching bad read agree. Bump reads and assigns each member from Verse, so
		// the interpreter is the one checking, and a var whose reference was overwritten dies
		// here rather than reporting a plausible number.
		auto Read = [&](const char* Name) {
			const vh_value* Out = nullptr;
			return GetFieldFn(Instance, Name, &Out) == VH_OK && Out != nullptr ? Out : nullptr;
		};

		const bool BumpOk = CallInstanceVoidFn(Instance, "(/user@localhost/exports:)Bump") == VH_OK;
		Step("Verse can read and assign the members it was handed", BumpOk);
		const vh_value* ScaleAfterBump = Read("Scale");
		Step("Verse read both a var and a non-var it was handed",
			 BumpOk && ScaleAfterBump && ScaleAfterBump->Type == VH_TYPE_FLOAT && ScaleAfterBump->Float == 13.0);
		const vh_value* EnabledAfterBump = Read("Enabled");
		Step("a Verse assignment to a logic var is visible across the ABI",
			 BumpOk && EnabledAfterBump && EnabledAfterBump->Type == VH_TYPE_LOGIC && EnabledAfterBump->Logic != 0);

		// Separate from Bump because a string var is stored as a mutable container rather than as
		// a box around an immutable one, so it is the case a scalar var cannot stand in for.
		const bool BumpLabelOk = CallInstanceVoidFn(Instance, "(/user@localhost/exports:)BumpLabel") == VH_OK;
		Step("Verse can read and assign a string var", BumpLabelOk);
		const vh_value* LabelAfterBump = Read("Label");
		Step("a Verse assignment to a string var is visible across the ABI",
			 BumpLabelOk && LabelAfterBump && LabelAfterBump->Type == VH_TYPE_STRING &&
				 std::string(LabelAfterBump->String.Utf8, LabelAfterBump->String.Len) == "changed!");

		// Bump sealed the instance. From here Verse has observed the members, so the author's
		// `var` is the whole of what may still change.
		Step("writing a non-var member is refused once the instance is sealed",
			 SetFieldFn(Instance, "Speed", &NewFloat) == VH_ERR_NOT_FOUND);
		const vh_value* SpeedAfter = Read("Speed");
		Step("the refused write left the value alone",
			 SpeedAfter && SpeedAfter->Type == VH_TYPE_FLOAT && SpeedAfter->Float == 3.0);
		Step("a var member is still writable once the instance is sealed",
			 RoundTrip("Scale", NewFloat, [](const vh_value& V) { return V.Type == VH_TYPE_FLOAT && V.Float == 10.0; }));

		ReleaseInstanceFn(Instance);
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
			TickFn(0.004);
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
				TickFn(0.004);
				Sleep(1);
			}
		}
		AsyncOk = Step("a broken buffer's diagnostics arrive from the poll", DiagnosticCount > 0) && AsyncOk;

		// Put the good text back, so nothing after this sees the broken parse.
		CheckProjectFn(ExportsPathUtf8.c_str(), CleanSource.c_str());
		CallsOk = AsyncOk && CallsOk;
	}

	ReleaseScriptFn(Script);
	Step("vh_release_script", true);
	ShutdownFn();
	Step("vh_shutdown", true);

	return RunOk && CallsOk && LookupOk ? 0 : 1;
}
