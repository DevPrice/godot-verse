#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "verse_host_abi.h"
#include "vm_heap.h"
#include "vm_interpreter.h"
#include "vm_loader.h"
#include "vm_sidecar.h"

// The one runtime object the vh_* entry points act on: the loaded program, the sidecar, the
// interpreter, what the consumer handed vh_init, and the storage each class read's answer lives in
// until that entry point is called again (include/verse_host_abi.h).
namespace vm {

// The Godot name of the mirrored class p_verse_name, from src/verse_api_classes.h, or null.
const char *mirrored_godot_name(std::string_view p_verse_name);

// A vh_arena whose allocations live until the next reset(): the storage a read answering into
// host-owned memory builds its answer in.
class HostArena : public vh_arena {
public:
	HostArena();
	HostArena(const HostArena &) = delete;
	HostArena &operator=(const HostArena &) = delete;
	void reset() { blocks.clear(); }

private:
	std::vector<std::unique_ptr<unsigned char[]>> blocks;

	static void *allocate(vh_arena *p_self, size_t p_size, size_t p_align);
};

class Runtime {
public:
	Runtime();
	~Runtime();
	Runtime(const Runtime &) = delete;
	Runtime &operator=(const Runtime &) = delete;

	Heap heap;
	Program program;
	Sidecar sidecar;
	Interpreter interpreter{ heap, program };
	std::string sidecar_path;
	std::string program_path;

	// Copied out of vh_init_desc, the callback table zeroed past the consumer's StructSize.
	vh_godot_api godot = {};
	vh_diagnostic_fn on_diagnostic = nullptr;
	void *diagnostic_ctx = nullptr;
	vh_runtime_error_fn on_runtime_error = nullptr;
	void *runtime_error_ctx = nullptr;

	// The content scope of everything that is not a call into a script instance (spec/tasks.md §8.1).
	Value project_scope;
	// The scope slot of every instance the host holds, which vh_tick_stats' PeakInstanceTasks is
	// counted over.
	std::vector<const Value *> instance_scopes;

	// vh_tick's work (godot-natives.md §10): every sleeper due now, earliest first, each resumed in a
	// VM entry of its own, whatever the budget; then a collection, when the heap wants one. Fills
	// every field of r_stats but StructSize.
	void tick(vh_tick_stats &r_stats);

	// A full collection (design §7.3), or nothing while an entry is running: a Value an op or a
	// native holds in a C++ local is no root. Answers whether it collected.
	bool collect_garbage();
	// Set from VERSE_VM_GC_STRESS at vh_init: collect after every entry the host makes, so a missing
	// root shows up as a wrong answer or a crash at the next call rather than eventually.
	bool gc_stress = false;

	// Makes r_scope's content scope the active one for an entry, first replacing it with a fresh one
	// when it is missing or terminated: a terminated scope is never revived (CLAUDE.md, R-ASYNC-4).
	// r_scope must be a heap root.
	void activate_scope(Value &r_scope);

	// Reads verse_classes.json and program.vbc from p_cooked_dir, or from its parent: an export
	// hands vh_init `verse_data/Cooked`, and the cooker writes both files in `verse_data`. VH_OK, or
	// VH_ERR_INIT with r_error the sentence to report.
	int32_t boot(const std::string &p_cooked_dir, std::string &r_error);

	// Reports a refusal the way a runtime host reports one: an error diagnostic with no location,
	// or, failing that, a runtime error with no frames.
	void report_error(const std::string &p_message) const;

	// Hands a runtime error to OnRuntimeError with its frames, or, with no such callback, folds its
	// message line into an error diagnostic (include/verse_host_abi.h, vh_init_desc).
	void report_raised(const RaisedError &p_raised) const;

	bool has_class(const char *p_name) const;
	bool is_abstract(const char *p_name) const;
	int32_t base_type(const char *p_name, const char **r_utf8);
	int32_t method_list(const char *p_name, const vh_method_desc **r_methods, int32_t *r_count);
	int32_t signal_list(const char *p_name, const vh_signal_desc **r_signals, int32_t *r_count);
	int32_t rpc_list(const char *p_name, const vh_rpc_desc **r_rpcs, int32_t *r_count);
	int32_t static_list(const char *p_name, const vh_static_desc **r_statics, int32_t *r_count);
	int32_t export_list(const char *p_name, const vh_export_desc **r_exports, int32_t *r_count);
	// spec/sidecar.md: a transient instance built with minting suppressed and without its blocks,
	// read and dropped. Nothing is stored between calls but the answer.
	int32_t default_field(const char *p_class, const char *p_name, const vh_value **r_value);

private:
	HostArena default_arena;
	vh_value default_answer = {};
	std::string base_type_answer;
	std::vector<vh_method_desc> method_descs;
	std::vector<vh_param_desc> method_params;
	std::vector<vh_signal_desc> signal_descs;
	std::vector<vh_param_desc> signal_args;
	std::vector<vh_rpc_desc> rpc_descs;
	std::vector<vh_static_desc> static_descs;
	std::vector<std::vector<vh_value>> static_value_blocks;
	std::vector<vh_export_desc> export_descs;
	std::unordered_map<std::string, const char *> godot_names;

	const SidecarClass *find(const char *p_name) const;
	const char *godot_name_for(const std::string &p_verse_name);
	void fill_value(const SidecarValue &p_value, vh_value &r_value);
};

} // namespace vm
