#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// verse_classes.json, spec/sidecar.md: what every class-describing read answers, and the declared
// types the bytecode does not carry. The fields keep the ABI's numbering (vh_type, vh_variant_tag,
// the reject enums) as the file writes them; nothing here interprets one.
namespace vm {

constexpr int64_t kSidecarVersion = 8;

struct SidecarParam {
	std::string name;
	int32_t type = 0;
	int32_t tag = 0;
	bool has_default = false;
	std::string class_name;
	int32_t class_kind = 0;
};

struct SidecarMethod {
	std::string name;
	std::string decorated;
	std::vector<SidecarParam> params;
	int32_t required = 0;
	int32_t result = 0;
	int32_t result_tag = 0;
	std::string result_class;
	int32_t result_class_kind = 0;
	bool can_fail = false;
	bool suspends = false;
	std::string godot_virtual;
	int32_t line = -1;
	int32_t column = -1;
};

struct SidecarSignal {
	std::string name;
	std::vector<SidecarParam> args;
	int32_t line = -1;
	int32_t column = -1;
	int32_t reject = 0;
	std::string reject_detail;
};

struct SidecarRpc {
	std::string name;
	int32_t mode = 0;
	bool call_local = false;
	int32_t transfer = 0;
	int32_t channel = 0;
	int32_t line = -1;
	int32_t column = -1;
	int32_t reject = 0;
	std::string reject_detail;
};

struct SidecarExport {
	std::string name;
	int32_t type = 0;
	int32_t tag = 0;
	int32_t element_tag = 0;
	bool is_var = false;
	int32_t hint = 0;
	std::string hint_string;
	std::string native_class;
	double range_min = 0.0;
	double range_max = 0.0;
	bool has_range_min = false;
	bool has_range_max = false;
	int32_t group_kind = 0;
	std::string group_name;
	int32_t line = -1;
	int32_t column = -1;
	int32_t reject = 0;
};

// A statics member's value: `v` read by `type` (spec/sidecar.md, statics). An option holds zero or
// one element; an int is carried as the decimal string's value.
struct SidecarValue {
	int32_t type = 0;
	int32_t tag = 0;
	bool logic = false;
	int64_t integer = 0;
	double number = 0.0;
	uint32_t code_point = 0;
	std::string text;
	std::vector<SidecarValue> elements;
};

struct SidecarStatic {
	std::string name;
	bool is_function = false;
	int32_t line = -1;
	int32_t column = -1;
	SidecarValue value;
};

struct SidecarMemberType {
	SidecarExport described;
	bool is_var = false;

	bool has_ref = false;
	std::string ref;
	std::string ref_path;
	int32_t ref_origin = 0;
	bool ref_option = false;

	std::string struct_name;

	bool has_enum = false;
	std::string enum_name;
	int32_t enumerators = 0;

	bool has_user_struct = false;
	std::string user_struct_name;
	std::vector<std::string> field_names;
	std::vector<std::string> field_keys;
	std::vector<SidecarMemberType> field_types;
};

struct SidecarPayloadArg {
	std::string name;
	std::string key;
	SidecarMemberType type;
};

struct SidecarPayloadShape {
	int32_t kind = 0;
	int32_t reject = 0;
	std::string reject_detail;
	bool has_whole = false;
	SidecarMemberType whole;
	std::vector<SidecarPayloadArg> args;
};

struct SidecarMethodTypes {
	std::vector<SidecarMemberType> params;
	SidecarMemberType result;
};

struct SidecarClass {
	std::string name;
	bool abstract = false;
	bool published = false;
	bool exports_harvested = false;
	std::string to_string;
	std::vector<SidecarMethod> methods;
	std::vector<SidecarSignal> signals;
	std::vector<SidecarRpc> rpcs;
	std::vector<SidecarExport> exports;
	std::vector<SidecarStatic> statics;

	// Keyed as spec/sidecar.md says, in the file's order; a repeated member name keeps the
	// derived class's, which the file lists first.
	std::vector<std::pair<std::string, SidecarMemberType>> member_types;
	std::vector<std::pair<std::string, SidecarMethodTypes>> method_types;
	std::vector<std::pair<std::string, SidecarPayloadShape>> signal_types;
};

struct SidecarBinding {
	std::string verse;
	std::string godot;
	std::string script;
};

struct Sidecar {
	int64_t version = 0;
	int64_t abi = 0;
	std::string host_id;
	std::string engine_commit;
	int64_t generation = 0;
	std::vector<std::string> packages;
	std::vector<SidecarBinding> bindings;
	std::vector<SidecarPayloadShape> engine_signal_shapes;
	std::unordered_map<std::string, size_t> engine_signal_keys;
	std::vector<SidecarClass> classes;
	std::unordered_map<std::string, size_t> classes_by_name;

	const SidecarClass *find_class(std::string_view p_name) const;
	const SidecarBinding *find_binding(std::string_view p_verse_name) const;
};

// Reads a sidecar's text. p_path is only for the sentences, each of which names it: a version other
// than 8 and invalid JSON are refused with spec/sidecar.md's own texts, anything else malformed
// with a sentence of this runtime's.
bool sidecar_parse(std::string_view p_text, const std::string &p_path, Sidecar &r_sidecar, std::string &r_error);

} // namespace vm
