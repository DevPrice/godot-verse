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
	auto HasClassFn = Resolve<vh_has_class_fn>(Module, "vh_has_class", &ResolveOk);
	auto ClassExportListFn = Resolve<vh_class_export_list_fn>(Module, "vh_class_export_list", &ResolveOk);
	auto ClassDefaultFieldFn = Resolve<vh_class_default_field_fn>(Module, "vh_class_default_field", &ResolveOk);
	auto InstantiateFn = Resolve<vh_instantiate_fn>(Module, "vh_instantiate", &ResolveOk);
	auto ReleaseInstanceFn = Resolve<vh_release_instance_fn>(Module, "vh_release_instance", &ResolveOk);
	auto CallInstanceVoidFn = Resolve<vh_instance_call_void_fn>(Module, "vh_instance_call_void", &ResolveOk);
	auto GetFieldFn = Resolve<vh_instance_get_field_fn>(Module, "vh_instance_get_field", &ResolveOk);
	auto SetFieldFn = Resolve<vh_instance_set_field_fn>(Module, "vh_instance_set_field", &ResolveOk);
	auto LookupSymbolFn = Resolve<vh_lookup_symbol_fn>(Module, "vh_lookup_symbol", &ResolveOk);
	auto CompleteSymbolFn = Resolve<vh_complete_symbol_fn>(Module, "vh_complete_symbol", &ResolveOk);
	auto ClassMembersFn = Resolve<vh_class_members_fn>(Module, "vh_class_members", &ResolveOk);
	auto SignatureAtFn = Resolve<vh_signature_at_fn>(Module, "vh_signature_at", &ResolveOk);
	auto CheckProjectFn = Resolve<vh_check_project_fn>(Module, "vh_check_project", &ResolveOk);
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
	Desc.OnDiagnostic = &SmokeOnDiagnostic;
	Desc.DiagnosticCtx = nullptr;
	Desc.EnableDebugger = 0;

	if (!Step("vh_init", InitFn(&Desc) == VH_OK))
	{
		return 1;
	}

	// One call for both fixtures: Verse builds a whole package at once, and the host may only
	// generate once per process.
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
					LookupOk = Step("it names the class the override came from",
								   Text(ReadyLookup->OverriddenOwnerUtf8, ReadyLookup->OverriddenOwnerLen) == "object")
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
					{ "and so does the method's", "PhysicsProcess<override>(", 2, "PhysicsProcess", "exports_probe" },
					// A parameter is described by itself rather than by the method it belongs to,
					// which is what stops hovering an argument from documenting the whole call.
					// The editor declines to show anything for one, but that is its policy: the
					// host still has to resolve it, or the enclosing method would answer instead.
					{ "a parameter resolves to the parameter", "PhysicsProcess<override>(Delta:float)", strlen("PhysicsProcess<override>(De"), "Delta", "PhysicsProcess" },
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
				if (const vh_complete_item* Process = Offers(Items, Count, "PhysicsProcess"))
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
					if (const vh_complete_item* Ready = Offers(Items, Count, "Ready"))
					{
						CompleteOk = Step("an inherited method is offered as overridable", Ready->IsOverridable != 0) && CompleteOk;
						CompleteOk = Step("owned by the class that declares it",
										 Text(Ready->OwnerUtf8, Ready->OwnerLen) == "object")
								  && CompleteOk;
						CompleteOk = Step("and spelled as a declaration",
										 Text(Ready->SignatureUtf8, Ready->SignatureLen) == "():void")
								  && CompleteOk;
					}
					else
					{
						CompleteOk = Step("an inherited method is offered at all", false);
					}
					if (const vh_complete_item* Process = Offers(Items, Count, "Process"))
					{
						// The parameter's own name, which is the whole reason the signature is not
						// read off the function type: that spells this one "float->void".
						CompleteOk = Step("a parameter is named in the signature",
										 Text(Process->SignatureUtf8, Process->SignatureLen) == "(Delta:float):void")
								  && CompleteOk;
					}
					// Already overridden by the fixture, so it comes back owned by the fixture's
					// own class -- which is how the editor knows not to offer it a second time.
					if (const vh_complete_item* Physics = Offers(Items, Count, "PhysicsProcess"))
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
					CompleteOk = Step("it names the method", Text(Signature->NameUtf8, Signature->NameLen) == "PhysicsProcess") && CompleteOk;
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
				CompleteOk = Step("it lists an @editable member", Find(Members, MemberCount, "Speed") != nullptr) && CompleteOk;
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
	TickFn(0.0);
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

	ShutdownFn();
	Step("vh_shutdown", true);

	return RunOk && CallsOk && LookupOk ? 0 : 1;
}
