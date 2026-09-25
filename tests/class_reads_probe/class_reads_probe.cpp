// Prints every class-describing read a runtime host answers, one line per field, so two hosts can
// be diffed: the UE runtime host (verse_host_runtime.dll) is the reference and the interpreter
// (bin/verse_vm.dll) is held to it (docs/phase-7.5-design.md §10.2, tasks.md T3.3).
//
//   bin/class_reads_probe.exe <host dll> <engine dir> <cooked dir> [--sidecar <verse_classes.json>] [class...]
//
// With --sidecar every class the sidecar lists is read, in sorted order, after any named on the
// command line. The engine and cooked directories are what vh_init_desc carries: for an export's
// `verse_data`, `<data>/Engine` and `<data>/Cooked`.
//
// stdout is the transcript and carries nothing that differs between two correct hosts. Load time,
// diagnostics and runtime errors go to stderr.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "verse_host_abi.h"
#include "vm_file_reader.h"
#include "vm_json.h"

namespace {

void ProbePrint(void *, const char *Utf8, int32_t Len) {
	fprintf(stderr, "[verse] %.*s\n", Len, Utf8);
}
vh_bool ProbeIsValid(void *, vh_handle) {
	return 1;
}
int32_t ProbeGetProperty(void *, vh_handle, const char *, int32_t, vh_arena *, vh_value *) {
	return VH_CALL_DEAD_OBJECT;
}
int32_t ProbeSetProperty(void *, vh_handle, const char *, int32_t, const vh_value *) {
	return VH_CALL_DEAD_OBJECT;
}
int32_t ProbeCallMethod(void *, vh_handle, const char *, int32_t, const vh_value *, int32_t, vh_arena *, vh_value *Out) {
	Out->Type = VH_TYPE_VOID;
	return VH_CALL_OK;
}
vh_handle ProbeGetSingleton(void *, const char *, int32_t) {
	return 0;
}
int32_t ProbeGetClassOf(void *, vh_handle, vh_arena *, vh_value *) {
	return VH_CALL_DEAD_OBJECT;
}
void ProbeOnDiagnostic(void *, const vh_diagnostic *D) {
	fprintf(stderr, "[diag] %.*s\n", D->MessageLen, D->MessageUtf8);
}
void ProbeOnRuntimeError(void *, const vh_runtime_error *E) {
	fprintf(stderr, "[error] %.*s\n", E->MessageLen, E->MessageUtf8);
}

const char *StatusName(int32_t Status) {
	switch (Status) {
		case VH_OK:
			return "VH_OK";
		case VH_ERR_ABI:
			return "VH_ERR_ABI";
		case VH_ERR_STATE:
			return "VH_ERR_STATE";
		case VH_ERR_INIT:
			return "VH_ERR_INIT";
		case VH_ERR_COMPILE:
			return "VH_ERR_COMPILE";
		case VH_ERR_NOT_FOUND:
			return "VH_ERR_NOT_FOUND";
		case VH_ERR_RUNTIME:
			return "VH_ERR_RUNTIME";
		case VH_ERR_ARGUMENT:
			return "VH_ERR_ARGUMENT";
		case VH_ERR_FAILED:
			return "VH_ERR_FAILED";
		case VH_ERR_HALTED:
			return "VH_ERR_HALTED";
		case VH_ERR_THREAD:
			return "VH_ERR_THREAD";
		case VH_ERR_STOPPED:
			return "VH_ERR_STOPPED";
		case VH_ERR_UNSUPPORTED:
			return "VH_ERR_UNSUPPORTED";
		default:
			return "?";
	}
}

// Quoted, with every byte outside printable ASCII escaped, so a stray NUL or a trailing space shows.
std::string Quote(const char *Utf8, int32_t Len) {
	if (Utf8 == nullptr) {
		return Len == 0 ? "null" : "null(len " + std::to_string(Len) + ")";
	}
	std::string Out = "\"";
	for (int32_t Index = 0; Index < Len; ++Index) {
		const unsigned char Byte = static_cast<unsigned char>(Utf8[Index]);
		if (Byte == '"' || Byte == '\\') {
			Out += '\\';
			Out += static_cast<char>(Byte);
		} else if (Byte >= 0x20 && Byte < 0x7F) {
			Out += static_cast<char>(Byte);
		} else {
			char Hex[8];
			snprintf(Hex, sizeof(Hex), "\\x%02X", Byte);
			Out += Hex;
		}
	}
	return Out + "\"";
}

std::string Quote(const char *Utf8) {
	return Utf8 == nullptr ? "null" : Quote(Utf8, static_cast<int32_t>(strlen(Utf8)));
}

std::string Float(double Value) {
	char Text[64];
	snprintf(Text, sizeof(Text), "%.17g", Value);
	return Text;
}

std::string ValueText(const vh_value &Value) {
	const std::string Head = "type " + std::to_string(Value.Type) + " tag " + std::to_string(Value.VariantTag);
	switch (Value.Type) {
		case VH_TYPE_VOID:
			return Head;
		case VH_TYPE_LOGIC:
			return Head + " " + std::to_string(Value.Logic);
		case VH_TYPE_INT:
			return Head + " " + std::to_string(Value.Int);
		case VH_TYPE_FLOAT:
			return Head + " " + Float(Value.Float);
		case VH_TYPE_CHAR:
			return Head + " " + std::to_string(Value.Char);
		case VH_TYPE_STRING:
			return Head + " " + Quote(Value.String.Utf8, Value.String.Len);
		case VH_TYPE_ARRAY:
		case VH_TYPE_TUPLE: {
			std::string Out = Head + " [";
			for (int32_t Index = 0; Index < Value.Seq.Count; ++Index) {
				Out += (Index == 0 ? "" : ", ") + ValueText(Value.Seq.Items[Index]);
			}
			return Out + "]";
		}
		case VH_TYPE_MAP: {
			std::string Out = Head + " {";
			for (int32_t Index = 0; Index < Value.Map.Count; ++Index) {
				Out += (Index == 0 ? "" : ", ") + ValueText(Value.Map.Pairs[Index].Key) + ": " + ValueText(Value.Map.Pairs[Index].Value);
			}
			return Out + "}";
		}
		case VH_TYPE_OPTION:
			return Value.Option == nullptr ? Head + " none" : Head + " some(" + ValueText(*Value.Option) + ")";
		case VH_TYPE_REF:
			return Head + " ref " + std::to_string(Value.Ref);
		default:
			return Head + " ?";
	}
}

std::string ParamText(const vh_param_desc &Param) {
	return "name " + Quote(Param.NameUtf8, Param.NameLen) + " type " + std::to_string(Param.Type) +
			" tag " + std::to_string(Param.VariantTag) + " default " + std::to_string(Param.HasDefault) +
			" class " + Quote(Param.ClassUtf8, Param.ClassLen) + " classKind " + std::to_string(Param.ClassKind);
}

struct Host {
	vh_init_fn Init = nullptr;
	vh_shutdown_fn Shutdown = nullptr;
	vh_has_class_fn HasClass = nullptr;
	vh_class_is_abstract_fn IsAbstract = nullptr;
	vh_class_base_type_fn BaseType = nullptr;
	vh_class_method_list_fn Methods = nullptr;
	vh_class_signal_list_fn Signals = nullptr;
	vh_class_rpc_list_fn Rpcs = nullptr;
	vh_class_static_list_fn Statics = nullptr;
	vh_class_export_list_fn Exports = nullptr;
};

template <typename Fn>
bool Resolve(HMODULE Module, const char *Name, Fn &r_Fn) {
	r_Fn = reinterpret_cast<Fn>(GetProcAddress(Module, Name));
	if (r_Fn == nullptr) {
		printf("[probe] resolve %s: FAIL\n", Name);
		return false;
	}
	return true;
}

void ReadClass(const Host &H, const std::string &Name) {
	const char *C = Name.c_str();
	printf("class %s\n", Quote(C).c_str());
	printf("  has_class %d\n", H.HasClass(C));
	printf("  is_abstract %d\n", H.IsAbstract(C));

	const char *Base = nullptr;
	int32_t Status = H.BaseType(C, &Base);
	printf("  base_type %s %s\n", StatusName(Status), Status == VH_OK ? Quote(Base).c_str() : "");

	const vh_method_desc *Methods = nullptr;
	int32_t Count = -1;
	Status = H.Methods(C, &Methods, &Count);
	printf("  methods %s count %d\n", StatusName(Status), Count);
	for (int32_t Index = 0; Status == VH_OK && Index < Count; ++Index) {
		const vh_method_desc &M = Methods[Index];
		printf("    method %s decorated %s required %d result %d resultTag %d resultClass %s resultClassKind %d canFail %d suspends %d virtual %s at %d:%d\n",
				Quote(M.NameUtf8, M.NameLen).c_str(), Quote(M.DecoratedUtf8, M.DecoratedLen).c_str(), M.RequiredParamCount,
				M.ResultType, M.ResultVariantTag, Quote(M.ResultClassUtf8, M.ResultClassLen).c_str(), M.ResultClassKind,
				M.CanFail, M.Suspends, Quote(M.GodotVirtualUtf8, M.GodotVirtualLen).c_str(), M.Line, M.Column);
		for (int32_t Param = 0; Param < M.ParamCount; ++Param) {
			printf("      param %s\n", ParamText(M.Params[Param]).c_str());
		}
	}

	const vh_signal_desc *Signals = nullptr;
	Count = -1;
	Status = H.Signals(C, &Signals, &Count);
	printf("  signals %s count %d\n", StatusName(Status), Count);
	for (int32_t Index = 0; Status == VH_OK && Index < Count; ++Index) {
		const vh_signal_desc &S = Signals[Index];
		printf("    signal %s at %d:%d reject %d detail %s\n", Quote(S.NameUtf8, S.NameLen).c_str(), S.Line, S.Column,
				S.Reject, Quote(S.RejectDetailUtf8, S.RejectDetailLen).c_str());
		for (int32_t Arg = 0; Arg < S.ArgCount; ++Arg) {
			printf("      arg %s\n", ParamText(S.Args[Arg]).c_str());
		}
	}

	const vh_rpc_desc *Rpcs = nullptr;
	Count = -1;
	Status = H.Rpcs(C, &Rpcs, &Count);
	printf("  rpcs %s count %d\n", StatusName(Status), Count);
	for (int32_t Index = 0; Status == VH_OK && Index < Count; ++Index) {
		const vh_rpc_desc &R = Rpcs[Index];
		printf("    rpc %s mode %d callLocal %d transfer %d channel %d at %d:%d reject %d detail %s\n",
				Quote(R.NameUtf8, R.NameLen).c_str(), R.RpcMode, R.CallLocal, R.TransferMode, R.Channel, R.Line, R.Column,
				R.Reject, Quote(R.RejectDetailUtf8, R.RejectDetailLen).c_str());
	}

	const vh_static_desc *Statics = nullptr;
	Count = -1;
	Status = H.Statics(C, &Statics, &Count);
	printf("  statics %s count %d\n", StatusName(Status), Count);
	for (int32_t Index = 0; Status == VH_OK && Index < Count; ++Index) {
		const vh_static_desc &S = Statics[Index];
		printf("    static %s function %d at %d:%d%s%s\n", Quote(S.NameUtf8, S.NameLen).c_str(), S.IsFunction, S.Line, S.Column,
				S.IsFunction ? "" : " value ", S.IsFunction ? "" : ValueText(S.Value).c_str());
	}

	const vh_export_desc *Exports = nullptr;
	Count = -1;
	Status = H.Exports(C, &Exports, &Count);
	printf("  exports %s count %d\n", StatusName(Status), Count);
	for (int32_t Index = 0; Status == VH_OK && Index < Count; ++Index) {
		const vh_export_desc &E = Exports[Index];
		printf("    export %s type %d tag %d elementTag %d var %d hint %d hintString %s nativeClass %s range %s%s..%s%s group %d %s at %d:%d reject %d\n",
				Quote(E.NameUtf8, E.NameLen).c_str(), static_cast<int32_t>(E.Type), E.VariantTag, E.ElementVariantTag, E.IsVar, E.Hint,
				Quote(E.HintStringUtf8, E.HintStringLen).c_str(), Quote(E.NativeClassUtf8, E.NativeClassLen).c_str(),
				E.HasRangeMin ? "" : "~", Float(E.RangeMin).c_str(), E.HasRangeMax ? "" : "~", Float(E.RangeMax).c_str(),
				E.GroupKind, Quote(E.GroupNameUtf8, E.GroupNameLen).c_str(), E.Line, E.Column, E.Reject);
	}
}

bool SidecarClasses(const char *Path, std::vector<std::string> &r_Names) {
	std::vector<uint8_t> Bytes;
	if (!vm_default_file_reader(Path, Bytes)) {
		printf("[probe] cannot read %s\n", Path);
		return false;
	}
	vm::JsonValue Root;
	std::string Error;
	if (!vm::json_parse(std::string_view(reinterpret_cast<const char *>(Bytes.data()), Bytes.size()), Root, Error)) {
		printf("[probe] %s: %s\n", Path, Error.c_str());
		return false;
	}
	const vm::JsonValue *Classes = Root.find("classes");
	if (Classes == nullptr || !Classes->is_object()) {
		printf("[probe] %s has no classes object\n", Path);
		return false;
	}
	std::vector<std::string> Names(Classes->keys.begin(), Classes->keys.end());
	std::sort(Names.begin(), Names.end());
	r_Names.insert(r_Names.end(), Names.begin(), Names.end());
	return true;
}

} // namespace

int main(int argc, char **argv) {
	setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc < 4) {
		printf("usage: class_reads_probe <host dll> <engine dir> <cooked dir> [--sidecar <path>] [class...]\n");
		return 2;
	}
	const std::string DllPath = argv[1];
	const std::string EngineDir = argv[2];
	const std::string CookedDir = argv[3];

	std::vector<std::string> Named;
	std::string SidecarPath;
	for (int Index = 4; Index < argc; ++Index) {
		if (std::string(argv[Index]) == "--sidecar" && Index + 1 < argc) {
			SidecarPath = argv[++Index];
		} else {
			Named.push_back(argv[Index]);
		}
	}
	if (!SidecarPath.empty() && !SidecarClasses(SidecarPath.c_str(), Named)) {
		return 1;
	}

	const std::wstring DllPathW(DllPath.begin(), DllPath.end());
	HMODULE Module = LoadLibraryExW(DllPathW.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (Module == nullptr) {
		printf("[probe] LoadLibrary failed: %lu\n", GetLastError());
		return 1;
	}
	Host H;
	bool Ok = Resolve(Module, "vh_init", H.Init);
	Ok = Resolve(Module, "vh_shutdown", H.Shutdown) && Ok;
	Ok = Resolve(Module, "vh_has_class", H.HasClass) && Ok;
	Ok = Resolve(Module, "vh_class_is_abstract", H.IsAbstract) && Ok;
	Ok = Resolve(Module, "vh_class_base_type", H.BaseType) && Ok;
	Ok = Resolve(Module, "vh_class_method_list", H.Methods) && Ok;
	Ok = Resolve(Module, "vh_class_signal_list", H.Signals) && Ok;
	Ok = Resolve(Module, "vh_class_rpc_list", H.Rpcs) && Ok;
	Ok = Resolve(Module, "vh_class_static_list", H.Statics) && Ok;
	Ok = Resolve(Module, "vh_class_export_list", H.Exports) && Ok;
	if (!Ok) {
		return 1;
	}

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
	Desc.OnDiagnostic = &ProbeOnDiagnostic;
	Desc.OnRuntimeError = &ProbeOnRuntimeError;
	Desc.CookedDirUtf8 = CookedDir.c_str();

	const auto Start = std::chrono::steady_clock::now();
	const int32_t InitStatus = H.Init(&Desc);
	const double Millis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
	fprintf(stderr, "[time] vh_init %.1f ms\n", Millis);
	printf("vh_init %s\n", StatusName(InitStatus));
	if (InitStatus != VH_OK) {
		return 1;
	}

	for (const std::string &Name : Named) {
		ReadClass(H, Name);
	}

	H.Shutdown();
	printf("done\n");
	return 0;
}
