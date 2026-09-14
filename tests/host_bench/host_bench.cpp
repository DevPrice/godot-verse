// Times the three host operations whose cost scales with the size of the Godot mirror, with no
// Godot and no editor in the way: loading the engine and its native packages, the one generating
// build, and the analysis the editor runs per keystroke.
//
// Phase 2 needs these numbers before it can choose between the curated class list and the full
// 1022-class mirror (docs/phase-2-design.md 3). Reported rather than asserted: R-PERF-2 asks for a
// recorded number, not a threshold, because there is no baseline to set one against.
#include <windows.h>

#include <psapi.h>

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
	// EngineDirUtf8 is what vh_init is given, which is the *Engine* directory rather than the
	// checkout root -- but the smoke test and a hand-run can pass either, so both are tried.
	const fs::path Relative = fs::path("Source") / "Programs" / "VerseHost" / "Verse" / "GodotClasses.native.verse";
	fs::path Mirror = EngineDir / Relative;
	std::error_code Error;
	auto Size = fs::file_size(Mirror, Error);
	if (Error)
	{
		Mirror = EngineDir / "Engine" / Relative;
		Size = fs::file_size(Mirror, Error);
	}
	if (Error)
	{
		printf("[bench] mirror: %ls not found\n", Mirror.c_str());
		return;
	}
	printf("[bench] mirror: %.0f KB (%ls)\n", static_cast<double>(Size) / 1024.0, Mirror.filename().c_str());
}

/// Row and column of a byte offset, as every position-taking entry point wants them: zero-based,
/// counted in bytes. The same helper host_smoke drives completion with.
void RowColumnOf(const std::string& Text, size_t Offset, int32_t& OutRow, int32_t& OutColumn)
{
	OutRow = 0;
	size_t LineStart = 0;
	for (size_t Index = 0; Index < Offset && Index < Text.size(); ++Index)
	{
		if (Text[Index] == '\n')
		{
			++OutRow;
			LineStart = Index + 1;
		}
	}
	OutColumn = static_cast<int32_t>(Offset - LineStart);
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

/// The same shape for something that is not a duration. Kept separate rather than given a unit
/// parameter, because every other series here is milliseconds and a column headed "ms" that is not
/// is worse than a second function.
void ReportCounts(const char* Name, std::vector<double> Samples)
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
	printf("[bench] %-28s n=%zu  min %8.0f     median %8.0f     max %8.0f     mean %8.1f\n",
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
	auto InstantiateFn = Resolve<vh_instantiate_fn>(Module, "vh_instantiate", &Ok);
	auto ReleaseInstanceFn = Resolve<vh_release_instance_fn>(Module, "vh_release_instance", &Ok);
	auto InstanceCallFn = Resolve<vh_instance_call_fn>(Module, "vh_instance_call", &Ok);
	auto TickFn = Resolve<vh_tick_fn>(Module, "vh_tick", &Ok);
	auto CheckProjectBeginFn = Resolve<vh_check_project_begin_fn>(Module, "vh_check_project_begin", &Ok);
	auto CheckProjectPollFn = Resolve<vh_check_project_poll_fn>(Module, "vh_check_project_poll", &Ok);
	auto CompleteSymbolFn = Resolve<vh_complete_symbol_fn>(Module, "vh_complete_symbol", &Ok);
	auto SignatureAtFn = Resolve<vh_signature_at_fn>(Module, "vh_signature_at", &Ok);
	auto LookupSymbolFn = Resolve<vh_lookup_symbol_fn>(Module, "vh_lookup_symbol", &Ok);
	auto ClassMembersFn = Resolve<vh_class_members_fn>(Module, "vh_class_members", &Ok);
	auto ClassExportListFn = Resolve<vh_class_export_list_fn>(Module, "vh_class_export_list", &Ok);
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
	const vh_source_file ProjectFiles[2] = {
		{ VersePathUtf8.c_str(), nullptr },
		{ ExportsPathUtf8.c_str(), nullptr },
	};

	int32_t Generation = 0;
	const Clock::time_point CompileStart = Clock::now();
	const int32_t CompileResult = CompileProjectFn(ProjectFiles, 2, &Generation);
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

	// The same analysis on the path the editor actually takes: _validate begins it on the host's own
	// thread and _frame reaps it, so what an author waits is wall time spread over frames rather than
	// a blocked call. The poll count is the frame count, and is the half of the cost the millisecond
	// figure cannot show.
	std::vector<double> AsyncSamples;
	std::vector<double> AsyncPolls;
	for (int Iteration = 0; Iteration < Iterations; ++Iteration)
	{
		std::string Edited = Source;
		Edited.append("\n# async ");
		Edited.append(static_cast<size_t>(Iteration) + 1, 'y');
		Edited.append("\n");

		const Clock::time_point AsyncStart = Clock::now();
		if (CheckProjectBeginFn(ExportsPathUtf8.c_str(), Edited.c_str()) != VH_OK)
		{
			printf("[bench] async check: vh_check_project_begin refused on iteration %d\n", Iteration + 1);
			break;
		}

		double Polls = 0.0;
		vh_bool Finished = 0;
		while (Finished == 0)
		{
			// The budget the editor's _frame passes, and for the same reason: a frame that lands
			// mid-analysis must not spend itself waiting for one.
			vh_tick_stats Stats{};
			Stats.StructSize = sizeof(Stats);
			TickFn(0.001, &Stats);
			CheckProjectPollFn(&Finished);
			Polls += 1.0;
			Sleep(1); // A frame, roughly. Spinning here would take a core off the analysis.
		}
		AsyncSamples.push_back(MillisSince(AsyncStart));
		AsyncPolls.push_back(Polls);
	}

	// What an editor pays when it asks a question while an analysis it started is still running.
	// Two of the reads `_validate` and the inspector make on the game thread, taken inside that
	// window: both used to wait the analysis out first, which is the whole of the editor hang.
	// Measured once each rather than as a series -- it is the same analysis as above, seen from
	// the caller's side, and the window only exists once.
	double BlockedMembersMs = 0.0;
	double BlockedExportsMs = 0.0;
	int32_t BlockedMemberCount = 0;
	int32_t BlockedExportCount = 0;
	vh_tick_stats WaitStats{};
	{
		std::string Edited = Source;
		Edited.append("\n# blocking\n");
		if (CheckProjectBeginFn(ExportsPathUtf8.c_str(), Edited.c_str()) == VH_OK)
		{
			const vh_complete_item* Items = nullptr;
			const Clock::time_point MembersStart = Clock::now();
			ClassMembersFn("exports", &Items, &BlockedMemberCount);
			BlockedMembersMs = MillisSince(MembersStart);

			// The second read is what says the first was answered rather than merely fast: a
			// snapshot answers both out of the same window, where a wait only ever pays once.
			const vh_export_desc* Exports = nullptr;
			const Clock::time_point ExportsStart = Clock::now();
			ClassExportListFn("exports", &Exports, &BlockedExportCount);
			BlockedExportsMs = MillisSince(ExportsStart);

			// Reap whatever the wait left queued, then tick: the wait accounting is reported by the
			// first vh_tick after it, which is where a running editor would see it too.
			//
			// Polled to completion rather than once. Neither read above waits any more, so this
			// analysis is still running when they answer -- and since ABI v7 nothing downstream
			// joins one either, so a single poll would leave the worker unreaped and every later
			// vh_check_project_begin refused. A running editor polls every frame; so does this.
			vh_bool Finished = 0;
			WaitStats.StructSize = sizeof(WaitStats);
			while (Finished == 0)
			{
				TickFn(0.001, &WaitStats);
				CheckProjectPollFn(&Finished);
				Sleep(1);
			}
		}
	}

	// Completion, the editor's other per-keystroke cost, measured as the three things it now pays
	// for one `.` (ABI v7). The old shape -- one call that analysed and one that did not -- stopped
	// describing anything the moment vh_complete_symbol stopped analysing: both calls would now
	// refuse in microseconds and the analysis would be missing from the table entirely.
	//
	//   refused      what the editor gets back on the keystroke itself, before it has queued
	//                anything. It is the whole of the latency the author sees, and it must be
	//                VH_ERR_STATE rather than an answer.
	//   analysis     the completion buffer through vh_check_project_begin/_poll, which is the
	//                background wait before the full list can replace the partial one.
	//   warm         the answer once that analysis has landed. This is the row the walk and the
	//                describe-once deduplication move, and the only one that was ever the host's
	//                own work rather than the compiler's.
	std::vector<double> MemberRefusedSamples;
	std::vector<double> MemberAnalysisSamples;
	std::vector<double> MemberWarmSamples;
	std::vector<double> ScopeRefusedSamples;
	std::vector<double> ScopeAnalysisSamples;
	std::vector<double> ScopeWarmSamples;
	std::vector<double> SignatureRefusedSamples;
	std::vector<double> SignatureSamples;
	std::vector<double> LookupSamples;
	int32_t RefusalsSeen = 0;
	int32_t RefusalsExpected = 0;
	{
		// The editor's own loop: hand the host the completion buffer and pump until the poll reaps
		// it, exactly as _frame does. Returns the wall time, which is what the author waits.
		auto AnalyseBuffer = [&](const std::string& Buffer) -> double {
			const Clock::time_point Started = Clock::now();
			if (CheckProjectBeginFn(ExportsPathUtf8.c_str(), Buffer.c_str()) != VH_OK)
			{
				return 0.0;
			}
			vh_bool Finished = 0;
			while (Finished == 0)
			{
				vh_tick_stats Stats{};
				Stats.StructSize = sizeof(Stats);
				TickFn(0.001, &Stats);
				CheckProjectPollFn(&Finished);
				Sleep(1);
			}
			return MillisSince(Started);
		};

		// The placeholder the GDExtension substitutes for the member half-typed at the cursor, on a
		// receiver of a mirrored type: `Position` is node2d's, so the answer comes out of the Godot
		// mirror rather than out of the project.
		const size_t FieldUse = Source.find("Position.X");
		const size_t Call = Source.find("PhysicsProcess<override>(");
		const size_t LocalUse = Source.find("X := Shifted");
		if (FieldUse == std::string::npos || Call == std::string::npos || LocalUse == std::string::npos)
		{
			printf("[bench] completion: skipped -- the fixture no longer has the sites it is measured on\n");
		}
		else
		{
			for (int Iteration = 0; Iteration < Iterations; ++Iteration)
			{
				// Appended past every site below, so the positions hold while the buffer is new
				// text the host has not analysed.
				std::string Typing = Source;
				Typing.replace(FieldUse, strlen("Position.X"), "Position.VhCompletionCursor");
				Typing.append("\n# typing ");
				Typing.append(static_cast<size_t>(Iteration) + 1, 'z');
				Typing.append("\n");

				// The receiver's last byte, not the cursor: the member being typed does not exist
				// yet and asking about it would resolve nothing.
				int32_t RecvRow = 0;
				int32_t RecvColumn = 0;
				RowColumnOf(Typing, FieldUse + strlen("Position") - 1, RecvRow, RecvColumn);

				const vh_complete_item* Items = nullptr;
				int32_t Count = 0;

				// The keystroke itself: the host has never seen this buffer, so it must refuse
				// immediately rather than analyse.
				const Clock::time_point RefusedStart = Clock::now();
				const int32_t MemberRefusal =
					CompleteSymbolFn(ExportsPathUtf8.c_str(), Typing.c_str(), RecvRow, RecvColumn,
									 VH_COMPLETE_MEMBERS, &Items, &Count);
				MemberRefusedSamples.push_back(MillisSince(RefusedStart));
				RefusalsExpected++;
				RefusalsSeen += MemberRefusal == VH_ERR_STATE ? 1 : 0;

				MemberAnalysisSamples.push_back(AnalyseBuffer(Typing));

				const Clock::time_point WarmStart = Clock::now();
				CompleteSymbolFn(ExportsPathUtf8.c_str(), Typing.c_str(), RecvRow, RecvColumn,
								 VH_COMPLETE_MEMBERS, &Items, &Count);
				MemberWarmSamples.push_back(MillisSince(WarmStart));

				// Scope mode walks everything the cursor can see, the whole of
				// `using {/Godot.org/Godot}` included, so it answers with thousands of names where
				// the member case answers with two.
				std::string ScopeTyping = Source;
				ScopeTyping.replace(LocalUse + strlen("X := "), strlen("Shifted"), "VhCompletionCursor");
				ScopeTyping.append("\n# scope ");
				ScopeTyping.append(static_cast<size_t>(Iteration) + 1, 'z');
				ScopeTyping.append("\n");

				int32_t ScopeRow = 0;
				int32_t ScopeColumn = 0;
				RowColumnOf(ScopeTyping, LocalUse + strlen("X := "), ScopeRow, ScopeColumn);

				const Clock::time_point ScopeRefusedStart = Clock::now();
				const int32_t ScopeRefusal =
					CompleteSymbolFn(ExportsPathUtf8.c_str(), ScopeTyping.c_str(), ScopeRow, ScopeColumn,
									 VH_COMPLETE_SCOPE, &Items, &Count);
				ScopeRefusedSamples.push_back(MillisSince(ScopeRefusedStart));
				RefusalsExpected++;
				RefusalsSeen += ScopeRefusal == VH_ERR_STATE ? 1 : 0;

				ScopeAnalysisSamples.push_back(AnalyseBuffer(ScopeTyping));

				const Clock::time_point ScopeStart = Clock::now();
				CompleteSymbolFn(ExportsPathUtf8.c_str(), ScopeTyping.c_str(), ScopeRow, ScopeColumn,
								 VH_COMPLETE_SCOPE, &Items, &Count);
				ScopeWarmSamples.push_back(MillisSince(ScopeStart));

				// Hover and goto-definition, off the same landed analysis. Asked on `Position`,
				// whose definition is in the mirror rather than in the project: the walk that finds
				// the cursor is over the project's own package, and what it resolves to is not.
				const size_t Hovered = ScopeTyping.find("Position.X");
				if (Hovered != std::string::npos)
				{
					int32_t HoverRow = 0;
					int32_t HoverColumn = 0;
					RowColumnOf(ScopeTyping, Hovered + 1, HoverRow, HoverColumn);
					const vh_lookup_desc* Looked = nullptr;
					const Clock::time_point LookupStart = Clock::now();
					LookupSymbolFn(ExportsPathUtf8.c_str(), HoverRow, HoverColumn, &Looked);
					LookupSamples.push_back(MillisSince(LookupStart));
				}

				// The argument hint, asked at the callee's last byte for the same reason. Asked
				// about the buffer the scope analysis just landed for, which is the editor's own
				// shape: one analysis answers the options and the hint together.
				int32_t CalleeRow = 0;
				int32_t CalleeColumn = 0;
				RowColumnOf(ScopeTyping, Call + strlen("PhysicsUpdat"), CalleeRow, CalleeColumn);
				const vh_signature_desc* Signature = nullptr;
				const Clock::time_point SignatureStart = Clock::now();
				SignatureAtFn(ExportsPathUtf8.c_str(), ScopeTyping.c_str(), CalleeRow, CalleeColumn, &Signature);
				SignatureSamples.push_back(MillisSince(SignatureStart));

				// And the same question about a buffer nothing has analysed, which is what the
				// editor gets on the keystroke that opens the call.
				std::string Unseen = ScopeTyping;
				Unseen.append("# unseen\n");
				const Clock::time_point SignatureRefusedStart = Clock::now();
				const int32_t SignatureRefusal =
					SignatureAtFn(ExportsPathUtf8.c_str(), Unseen.c_str(), CalleeRow, CalleeColumn, &Signature);
				SignatureRefusedSamples.push_back(MillisSince(SignatureRefusedStart));
				RefusalsExpected++;
				RefusalsSeen += SignatureRefusal == VH_ERR_STATE ? 1 : 0;
			}
		}
	}

	// Completion left the host holding a scratch buffer. The two readers below describe the program
	// the last analysis left behind, so the analysis has to be of the file as it is on disk -- and
	// has to have landed, or what they would measure is the wait rather than the read.
	CheckProjectFn(ExportsPathUtf8.c_str(), Source.c_str());

	std::vector<double> ClassMembersSamples;
	std::vector<double> ExportListSamples;
	for (int Iteration = 0; Iteration < Iterations; ++Iteration)
	{
		const vh_complete_item* Members = nullptr;
		int32_t MemberCount = 0;
		const Clock::time_point MembersStart = Clock::now();
		ClassMembersFn("exports", &Members, &MemberCount);
		ClassMembersSamples.push_back(MillisSince(MembersStart));

		const vh_export_desc* Exports = nullptr;
		int32_t ExportCount = 0;
		const Clock::time_point ExportsStart = Clock::now();
		ClassExportListFn("exports", &Exports, &ExportCount);
		ExportListSamples.push_back(MillisSince(ExportsStart));
	}

	// Phase 5's S-2 risk, measured rather than argued: a content scope per instance (R-ASYNC-4)
	// means one more allocation at vh_instantiate and one guard push/pop per vh_instance_call, and
	// the call is the hot path -- every _Process on every scripted node, every frame.
	//
	// Reported per operation in microseconds and per instance in kilobytes. The memory figure is
	// the whole cost of an instance, not the scope's share of it, which is the honest way round:
	// nothing can weigh a UObject and its task group apart from the object they belong to.
	double InstantiateUs = 0.0;
	double CallUs = 0.0;
	double InstanceKb = 0.0;
	{
		constexpr int InstanceCount = 200;
		constexpr int CallsPerInstance = 200;
		std::vector<vh_instance*> Instances;
		Instances.reserve(InstanceCount);

		PROCESS_MEMORY_COUNTERS_EX Before{};
		Before.cb = sizeof(Before);
		GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&Before), sizeof(Before));

		const Clock::time_point MakeStart = Clock::now();
		for (int Index = 0; Index < InstanceCount; ++Index)
		{
			vh_instance* Made = nullptr;
			if (InstantiateFn("exports_probe", 1000 + Index, &Made) == VH_OK && Made != nullptr)
			{
				Instances.push_back(Made);
			}
		}
		const double MakeMs = MillisSince(MakeStart);

		PROCESS_MEMORY_COUNTERS_EX After{};
		After.cb = sizeof(After);
		GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&After), sizeof(After));

		vh_value Args[2] = {};
		Args[0].Type = VH_TYPE_INT;
		Args[0].Int = 17;
		Args[1].Type = VH_TYPE_INT;
		Args[1].Int = 25;
		vh_value Result{};
		const Clock::time_point CallStart = Clock::now();
		for (int Round = 0; Round < CallsPerInstance; ++Round)
		{
			for (vh_instance* Instance : Instances)
			{
				InstanceCallFn(Instance, "(/user@localhost/exports:)AddInts(:int,:int)", Args, 2, nullptr, &Result);
			}
		}
		const double CallMs = MillisSince(CallStart);

		for (vh_instance* Instance : Instances)
		{
			ReleaseInstanceFn(Instance);
		}

		if (!Instances.empty())
		{
			InstantiateUs = (MakeMs * 1000.0) / static_cast<double>(Instances.size());
			CallUs = (CallMs * 1000.0)
				/ (static_cast<double>(Instances.size()) * static_cast<double>(CallsPerInstance));
			InstanceKb = (static_cast<double>(After.PrivateUsage) - static_cast<double>(Before.PrivateUsage))
				/ 1024.0 / static_cast<double>(Instances.size());
		}
	}

	printf("\n");
	// Generations, against a real project rather than the two fixtures above: R-ITER-6 asks
	// for a figure measured on something the size of a game, and dodge-the-creeps is the one
	// this repo has -- five files, a class each, and the whole mirror behind them.
	//
	// Two numbers come out of it. What a build costs, which is what an author pays on every
	// Play and the reason the trigger is Play rather than Ctrl+S; and what a generation
	// retains, which is the previous one's VPackage, its UPackage and their pinned exports,
	// none of which anything reclaims.
	std::vector<double> GenerationSamples;
	std::vector<double> RetainedKb;
	{
		const fs::path GameDir = VerseBase / "dodge-the-creeps" / "scripts";
		std::vector<std::string> GamePaths;
		std::error_code DirError;
		for (const fs::directory_entry& Entry : fs::directory_iterator(GameDir, DirError))
		{
			if (Entry.path().extension() == ".verse")
			{
				GamePaths.push_back(Entry.path().string());
			}
		}

		if (GamePaths.empty())
		{
			printf("[bench] generations: skipped -- no .verse under %ls\n", GameDir.c_str());
		}
		else
		{
			std::vector<vh_source_file> GameFiles;
			for (const std::string& Path : GamePaths)
			{
				GameFiles.push_back(vh_source_file{ Path.c_str(), "gameplay" });
			}

			for (int Iteration = 0; Iteration < Iterations; ++Iteration)
			{
				PROCESS_MEMORY_COUNTERS_EX Before{};
				Before.cb = sizeof(Before);
				GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&Before), sizeof(Before));

				int32_t Built = 0;
				const Clock::time_point GenStart = Clock::now();
				const int32_t GenResult =
					CompileProjectFn(GameFiles.data(), static_cast<int32_t>(GameFiles.size()), &Built);
				const double GenMs = MillisSince(GenStart);

				PROCESS_MEMORY_COUNTERS_EX After{};
				After.cb = sizeof(After);
				GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&After), sizeof(After));

				if (GenResult != VH_OK)
				{
					printf("[bench] generations: build %d failed (%d)\n", Iteration + 1, GenResult);
					break;
				}
				GenerationSamples.push_back(GenMs);
				RetainedKb.push_back(
					(static_cast<double>(After.PrivateUsage) - static_cast<double>(Before.PrivateUsage)) / 1024.0);
			}
		}
	}

	printf("[bench] %-28s %8.1f ms\n", "LoadLibrary", LoadMs);
	printf("[bench] %-28s %8.1f ms\n", "vh_init", InitMs);
	printf("[bench] %-28s %8.1f ms\n", "vh_compile_project", CompileMs);
	ReportSeries("vh_check_project", CheckSamples);
	ReportSeries("check begin+poll (wall)", AsyncSamples);
	ReportCounts("check begin+poll (polls)", AsyncPolls);
	printf("[bench] %-28s %8.1f ms  (vh_class_members during an analysis, %d member(s))\n",
		   "blocked read", BlockedMembersMs, BlockedMemberCount);
	printf("[bench] %-28s %8.1f ms  (vh_class_export_list during the same analysis, %d export(s))\n",
		   "blocked read 2", BlockedExportsMs, BlockedExportCount);
	printf("[bench] %-28s %d wait(s), %.1f ms  (vh_tick_stats, ABI v6.1+)\n",
		   "analysis wait since last tick", WaitStats.AnalysisWaits, WaitStats.AnalysisWaitSeconds * 1000.0);
	ReportSeries("complete members (refused)", MemberRefusedSamples);
	ReportSeries("complete members (analysis)", MemberAnalysisSamples);
	ReportSeries("complete members (warm)", MemberWarmSamples);
	ReportSeries("complete scope (refused)", ScopeRefusedSamples);
	ReportSeries("complete scope (analysis)", ScopeAnalysisSamples);
	ReportSeries("complete scope (warm)", ScopeWarmSamples);
	ReportSeries("vh_signature_at (refused)", SignatureRefusedSamples);
	ReportSeries("vh_signature_at (warm)", SignatureSamples);
	ReportSeries("vh_lookup_symbol (warm)", LookupSamples);
	printf("[bench] %-28s %d of %d refused with VH_ERR_STATE\n",
		   "completion refusals", RefusalsSeen, RefusalsExpected);
	ReportSeries("vh_class_members", ClassMembersSamples);
	ReportSeries("vh_class_export_list", ExportListSamples);
	ReportSeries("generation (5-file game)", GenerationSamples);
	if (!RetainedKb.empty())
	{
		std::vector<double> Sorted = RetainedKb;
		std::sort(Sorted.begin(), Sorted.end());
		double Total = 0.0;
		for (double Sample : RetainedKb)
		{
			Total += Sample;
		}
		printf("[bench] %-28s n=%zu  median %8.0f KB  mean %8.0f KB  total %8.0f KB\n",
			   "retained per generation",
			   Sorted.size(),
			   Sorted[Sorted.size() / 2],
			   Total / static_cast<double>(Sorted.size()),
			   Total);
	}
	printf("[bench] %-28s %8.1f us\n", "vh_instantiate (per node)", InstantiateUs);
	printf("[bench] %-28s %8.2f us\n", "vh_instance_call (per call)", CallUs);
	printf("[bench] %-28s %8.1f KB\n", "retained per instance", InstanceKb);
	// The completion analyses above are counted here too, and each reports the placeholder as an
	// unknown identifier: a completion buffer never analyses clean. The editor drops those -- a
	// completion-shaped check, dropped in poll_check -- rather than draw a line nobody wrote.
	printf("[bench] diagnostics reported as errors: %d  (completion buffers included)\n", ErrorCount);

	ShutdownFn();
	return 0;
}
