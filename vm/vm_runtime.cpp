#include "vm_runtime.h"

#include <cstring>
#include <string_view>
#include <utility>

#include "../src/verse_api_classes.h"
#include "vm_file_reader.h"
#include "vm_marshal.h"

namespace vm {

namespace {

const char *const kSidecarFile = "verse_classes.json";
const char *const kProgramFile = "program.vbc";

std::string join_path(const std::string &p_directory, const char *p_file) {
	if (p_directory.empty()) {
		return p_file;
	}
	const char last = p_directory.back();
	return p_directory + (last == '/' || last == '\\' ? "" : "/") + p_file;
}

std::string parent_directory(std::string p_directory) {
	while (p_directory.size() > 1 && (p_directory.back() == '/' || p_directory.back() == '\\')) {
		p_directory.pop_back();
	}
	const size_t separator = p_directory.find_last_of("/\\");
	return separator == std::string::npos ? std::string() : p_directory.substr(0, separator);
}

std::string missing_data(const std::string &p_path) {
	return "Verse data not found at " + p_path + ". The export is incomplete; export the project again.";
}

// spec/sidecar.md's stamp sentence. This runtime has no host id of its own: it compares `abi` only,
// and names itself where the UE host names its build digest.
std::string stamp_mismatch(int64_t p_cooked_abi, const std::string &p_cooked_host_id) {
	return "This game's Verse data was cooked by a different build of godot-verse (cooked " + std::to_string(p_cooked_abi) + "/" +
			p_cooked_host_id.substr(0, 7) + ", host " + std::to_string(VH_ABI_VERSION) + "/verse_vm). Export the project again.";
}

const ClassCell *superclass_of(const ClassCell *p_class) {
	if (p_class->inherited.empty()) {
		return nullptr;
	}
	const ClassCell *first = p_class->inherited.front();
	return first != nullptr && first->class_kind != ClassKind::Interface ? first : nullptr;
}

template <typename Desc>
int32_t answer_list(std::vector<Desc> &p_descs, const Desc **r_out, int32_t *r_count) {
	if (r_out != nullptr) {
		*r_out = p_descs.empty() ? nullptr : p_descs.data();
	}
	if (r_count != nullptr) {
		*r_count = int32_t(p_descs.size());
	}
	return VH_OK;
}

template <typename Desc>
int32_t answer_not_found(const Desc **r_out, int32_t *r_count) {
	if (r_out != nullptr) {
		*r_out = nullptr;
	}
	if (r_count != nullptr) {
		*r_count = 0;
	}
	return VH_ERR_NOT_FOUND;
}

vh_param_desc param_desc(const SidecarParam &p_param) {
	vh_param_desc desc = {};
	desc.NameUtf8 = p_param.name.c_str();
	desc.NameLen = int32_t(p_param.name.size());
	desc.Type = p_param.type;
	desc.VariantTag = p_param.tag;
	desc.HasDefault = p_param.has_default ? 1 : 0;
	desc.ClassUtf8 = p_param.class_name.c_str();
	desc.ClassLen = int32_t(p_param.class_name.size());
	desc.ClassKind = p_param.class_kind;
	return desc;
}

} // namespace

int32_t Runtime::boot(const std::string &p_cooked_dir, std::string &r_error) {
	const VmFileReaderFn read_file = vm_get_file_reader();

	std::vector<uint8_t> bytes;
	std::string data_dir;
	for (const std::string &candidate : { p_cooked_dir, parent_directory(p_cooked_dir) }) {
		if (!candidate.empty() && read_file(join_path(candidate, kSidecarFile).c_str(), bytes)) {
			data_dir = candidate;
			break;
		}
	}
	if (data_dir.empty()) {
		r_error = missing_data(join_path(p_cooked_dir, kSidecarFile));
		return VH_ERR_INIT;
	}
	sidecar_path = join_path(data_dir, kSidecarFile);
	if (!sidecar_parse(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()), sidecar_path, sidecar, r_error)) {
		return VH_ERR_INIT;
	}
	if (sidecar.abi != VH_ABI_VERSION) {
		r_error = stamp_mismatch(sidecar.abi, sidecar.host_id);
		return VH_ERR_INIT;
	}

	program_path = join_path(data_dir, kProgramFile);
	bytes.clear();
	if (!read_file(program_path.c_str(), bytes)) {
		r_error = missing_data(program_path);
		return VH_ERR_INIT;
	}
	if (!load_program(heap, bytes.data(), bytes.size(), program_path, program, r_error)) {
		return VH_ERR_INIT;
	}
	if (program.abi != uint64_t(VH_ABI_VERSION)) {
		r_error = stamp_mismatch(int64_t(program.abi), program.host_id);
		return VH_ERR_INIT;
	}
	if (program.host_id != sidecar.host_id || program.generation != uint64_t(sidecar.generation)) {
		r_error = program_path + " and " + sidecar_path + " were written by different cooks. Export the project again.";
		return VH_ERR_INIT;
	}
	interpreter.sidecar = &sidecar;
	interpreter.mirrored_godot_name = &mirrored_godot_name;
	for (ObjectCell *object : program.value_objects) {
		interpreter.layouts.lay_out_value_object(object);
	}
	// Not roots: a value object nothing in the program names is garbage, and would dangle here.
	program.value_objects.clear();
	interpreter.bridge = &bridge;
	bridge.bind_program();
	bridge.report_error = [this](const std::string &p_message) { report_error(p_message); };
	heap.tenure();
	return VH_OK;
}

bool Runtime::on_init_thread() const {
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
	return std::this_thread::get_id() == init_thread;
#else
	return true;
#endif
}

void Runtime::refuse_thread(const std::string &p_what) const {
	report_error(p_what + " was called from a thread other than the one Verse runs on, so it did not run. Call it from the main thread.");
}

HostArena &Runtime::result_arena(size_t p_depth) {
	while (result_arenas.size() <= p_depth) {
		result_arenas.push_back(std::make_unique<HostArena>());
	}
	result_arenas[p_depth]->reset();
	return *result_arenas[p_depth];
}

Runtime::Runtime() {
	heap.add_handle_root(&project_scope);
}

Runtime::~Runtime() {
	heap.remove_handle_root(&project_scope);
}

bool Runtime::collect_garbage() {
	if (interpreter.in_entry()) {
		return false;
	}
	heap.collect();
	return true;
}

void Runtime::report_error(const std::string &p_message) const {
	if (on_diagnostic != nullptr) {
		vh_diagnostic diagnostic = {};
		diagnostic.Severity = VH_SEVERITY_ERROR;
		diagnostic.MessageUtf8 = p_message.c_str();
		diagnostic.MessageLen = int32_t(p_message.size());
		diagnostic.FilePathUtf8 = "";
		diagnostic.SubjectTypeUtf8 = "";
		on_diagnostic(diagnostic_ctx, &diagnostic);
	} else if (on_runtime_error != nullptr) {
		vh_runtime_error runtime_error = {};
		runtime_error.MessageUtf8 = p_message.c_str();
		runtime_error.MessageLen = int32_t(p_message.size());
		on_runtime_error(runtime_error_ctx, &runtime_error);
	}
}

void Runtime::report_raised(const RaisedError &p_raised) const {
	const std::string line = p_raised.message_line();
	if (on_runtime_error == nullptr) {
		if (on_diagnostic != nullptr) {
			report_error(line);
		}
		return;
	}
	std::vector<vh_stack_frame> frames;
	frames.reserve(p_raised.frames.size());
	for (const ErrorFrame &source : p_raised.frames) {
		vh_stack_frame frame = {};
		frame.FunctionUtf8 = source.function.c_str();
		frame.FunctionLen = int32_t(source.function.size());
		frame.PathUtf8 = source.path.c_str();
		frame.PathLen = int32_t(source.path.size());
		frame.Line = source.line;
		frames.push_back(frame);
	}
	vh_runtime_error error = {};
	error.MessageUtf8 = line.c_str();
	error.MessageLen = int32_t(line.size());
	error.Frames = frames.empty() ? nullptr : frames.data();
	error.FrameCount = int32_t(frames.size());
	on_runtime_error(runtime_error_ctx, &error);
}

void Runtime::activate_scope(Value &r_scope) {
	if (!is_cell_kind(r_scope, CellKind::ContentScope) || cell_as<ContentScopeCell>(r_scope)->terminated) {
		r_scope = Value::from_cell(interpreter.make_scope());
	}
	interpreter.active_scope = cell_as<ContentScopeCell>(r_scope);
}

// A raise in one sleeper rolls back and reports that sleeper's run alone (godot-natives.md §10's
// nested transaction); the rest still wake.
void Runtime::tick(vh_tick_stats &r_stats) {
	const double started = monotonic_seconds();
	if (!interpreter.in_entry()) {
		for (TaskCell *sleeper : interpreter.take_due_sleepers(interpreter.clock())) {
			interpreter.begin_entry();
			const Outcome outcome = interpreter.complete(sleeper, heap.false_value());
			interpreter.end_entry(outcome == Outcome::Ok);
			if (outcome != Outcome::Ok) {
				report_raised(interpreter.error());
			}
			++r_stats.JobsRun;
		}
	}
	if (heap.wants_collection()) {
		collect_garbage();
	}
	r_stats.Sleeping = int32_t(interpreter.sleepers.size());
	for (const Value *scope : instance_scopes) {
		if (is_cell_kind(*scope, CellKind::ContentScope)) {
			const ContentScopeCell *cell = cell_as<ContentScopeCell>(*scope);
			if (!cell->terminated && int32_t(cell->group.size()) > r_stats.PeakInstanceTasks) {
				r_stats.PeakInstanceTasks = int32_t(cell->group.size());
			}
		}
	}
	r_stats.ElapsedSeconds = monotonic_seconds() - started;
}

HostArena::HostArena() {
	Alloc = &HostArena::allocate;
}

void HostArena::reset() {
	size_t kept = blocks.size();
	for (size_t index = 0; index < blocks.size(); ++index) {
		if (blocks[index].size <= kMaxBlockSize && (kept == blocks.size() || blocks[index].size > blocks[kept].size)) {
			kept = index;
		}
	}
	if (kept == blocks.size()) {
		blocks.clear();
	} else {
		if (kept != 0) {
			std::swap(blocks[0], blocks[kept]);
		}
		blocks.resize(1);
	}
	used = 0;
}

void *HostArena::allocate(vh_arena *p_self, size_t p_size, size_t p_align) {
	HostArena *arena = static_cast<HostArena *>(p_self);
	if (p_align == 0) {
		p_align = 1;
	}
	const auto fit = [arena, p_size, p_align]() -> void * {
		const Block &block = arena->blocks.back();
		const uintptr_t start = reinterpret_cast<uintptr_t>(block.bytes.get());
		const uintptr_t aligned = (start + arena->used + p_align - 1) / p_align * p_align;
		if (aligned + p_size > start + block.size) {
			return nullptr;
		}
		arena->used = aligned + p_size - start;
		// Every allocation reads as zeroes, as each did when it was a block of its own.
		std::memset(reinterpret_cast<void *>(aligned), 0, p_size);
		return reinterpret_cast<void *>(aligned);
	};
	if (!arena->blocks.empty()) {
		if (void *memory = fit()) {
			return memory;
		}
	}
	size_t size = arena->blocks.empty() ? kMinBlockSize : arena->blocks.back().size * 2;
	if (size > kMaxBlockSize) {
		size = kMaxBlockSize;
	}
	if (size < p_size + p_align) {
		size = p_size + p_align;
	}
	Block block;
	block.bytes.reset(new unsigned char[size]);
	block.size = size;
	arena->blocks.push_back(std::move(block));
	arena->used = 0;
	return fit();
}

int32_t Runtime::default_field(const char *p_class, const char *p_name, const vh_value **r_value) {
	if (r_value != nullptr) {
		*r_value = nullptr;
	}
	if (p_class == nullptr || p_name == nullptr || r_value == nullptr) {
		return VH_ERR_ARGUMENT;
	}
	const SidecarClass *entry = find(p_class);
	const ClassIndexEntry *index = program.find_class(ClassOrigin::Script, p_class);
	if (entry == nullptr || index == nullptr) {
		return VH_ERR_NOT_FOUND;
	}
	const SidecarMemberType *type = nullptr;
	for (const std::pair<std::string, SidecarMemberType> &member : entry->member_types) {
		if (member.first == p_name) {
			type = &member.second;
			break;
		}
	}
	if (type == nullptr) {
		return VH_ERR_NOT_FOUND;
	}

	activate_scope(project_scope);
	interpreter.begin_entry();
	Value object;
	RootScope root(heap, &object);
	// VhAdoptOrMint answers no peer when the host cannot instantiate one; hiding the callback keeps
	// that true for every object a member default builds, whatever the native reads of the flag.
	const auto instantiate = interpreter.godot.InstantiateClass;
	interpreter.godot.InstantiateClass = nullptr;
	interpreter.mint_suppressed = true;
	const Outcome outcome = interpreter.construct(index->class_cell, 0, object, false);
	interpreter.mint_suppressed = false;
	interpreter.godot.InstantiateClass = instantiate;
	if (outcome != Outcome::Ok) {
		interpreter.end_entry(false);
		if (outcome != Outcome::Fail && !interpreter.in_entry()) {
			report_raised(interpreter.error());
		}
		return outcome == Outcome::Unsupported ? VH_ERR_UNSUPPORTED : VH_ERR_RUNTIME;
	}

	const ObjectCell *instance = cell_as<ObjectCell>(object);
	const LayoutField *field = find_slot_by_unqualified_name(*instance->layout, p_name);
	if (field == nullptr) {
		interpreter.end_entry(false);
		return VH_ERR_NOT_FOUND;
	}
	Value value = follow(instance->field_values[field->slot]);
	if (is_cell_kind(value, CellKind::Ref)) {
		value = follow(cell_as<RefCell>(value)->content);
	}
	default_arena.reset();
	std::string why;
	const bool carried = bridge.member_to_wire(value, *type, &default_arena, default_answer, why);
	interpreter.end_entry(false);
	if (!carried) {
		return VH_ERR_UNSUPPORTED;
	}
	*r_value = &default_answer;
	return VH_OK;
}

const char *mirrored_godot_name(std::string_view p_verse_name) {
	for (const verse_api::class_mapping &mapping : verse_api::classes) {
		if (p_verse_name == mapping.verse_name) {
			return mapping.godot_name;
		}
	}
	return nullptr;
}

const SidecarClass *Runtime::find(const char *p_name) const {
	return p_name == nullptr ? nullptr : sidecar.find_class(p_name);
}

bool Runtime::has_class(const char *p_name) const {
	const SidecarClass *entry = find(p_name);
	return entry != nullptr && entry->published;
}

bool Runtime::is_abstract(const char *p_name) const {
	const SidecarClass *entry = find(p_name);
	return entry != nullptr && entry->abstract;
}

const char *Runtime::godot_name_for(const std::string &p_verse_name) {
	if (godot_names.empty()) {
		for (const verse_api::class_mapping &mapping : verse_api::classes) {
			godot_names.emplace(mapping.verse_name, mapping.godot_name);
		}
	}
	const auto found = godot_names.find(p_verse_name);
	return found == godot_names.end() ? nullptr : found->second;
}

// The Godot class a script attaches to: the first class up its superclass chain that Godot can
// name -- a mirrored class by its Godot name, a binding by the name its sidecar row keys on. A
// class the cook did not publish has none (spec/sidecar.md).
int32_t Runtime::base_type(const char *p_name, const char **r_utf8) {
	if (r_utf8 != nullptr) {
		*r_utf8 = nullptr;
	}
	const SidecarClass *entry = find(p_name);
	const ClassIndexEntry *start = entry != nullptr && entry->published ? program.find_class(ClassOrigin::Script, p_name) : nullptr;
	if (start == nullptr) {
		return VH_ERR_NOT_FOUND;
	}
	const ClassCell *current = start->class_cell;
	for (size_t depth = 0; depth < program.classes.size() + 1; ++depth) {
		current = superclass_of(current);
		if (current == nullptr) {
			break;
		}
		const ClassIndexEntry *base = program.entry_for(current);
		if (base == nullptr || base->origin == ClassOrigin::Script) {
			continue;
		}
		const char *answer = nullptr;
		if (base->origin == ClassOrigin::Mirrored) {
			answer = godot_name_for(base->name->text);
		} else if (const SidecarBinding *binding = sidecar.find_binding(base->name->text)) {
			answer = binding->godot.empty() ? binding->script.c_str() : binding->godot.c_str();
		}
		if (answer == nullptr) {
			break;
		}
		base_type_answer = answer;
		if (r_utf8 != nullptr) {
			*r_utf8 = base_type_answer.c_str();
		}
		return VH_OK;
	}
	return VH_ERR_NOT_FOUND;
}

int32_t Runtime::method_list(const char *p_name, const vh_method_desc **r_methods, int32_t *r_count) {
	const SidecarClass *entry = find(p_name);
	if (entry == nullptr) {
		return answer_not_found(r_methods, r_count);
	}
	method_descs.clear();
	method_params.clear();
	size_t total = 0;
	for (const SidecarMethod &method : entry->methods) {
		total += method.params.size();
	}
	method_params.reserve(total);
	for (const SidecarMethod &method : entry->methods) {
		vh_method_desc desc = {};
		desc.NameUtf8 = method.name.c_str();
		desc.NameLen = int32_t(method.name.size());
		desc.DecoratedUtf8 = method.decorated.c_str();
		desc.DecoratedLen = int32_t(method.decorated.size());
		desc.Params = method.params.empty() ? nullptr : method_params.data() + method_params.size();
		desc.ParamCount = int32_t(method.params.size());
		for (const SidecarParam &param : method.params) {
			method_params.push_back(param_desc(param));
		}
		desc.RequiredParamCount = method.required;
		desc.ResultType = method.result;
		desc.ResultVariantTag = method.result_tag;
		desc.ResultClassUtf8 = method.result_class.c_str();
		desc.ResultClassLen = int32_t(method.result_class.size());
		desc.ResultClassKind = method.result_class_kind;
		desc.CanFail = method.can_fail ? 1 : 0;
		desc.Suspends = method.suspends ? 1 : 0;
		desc.GodotVirtualUtf8 = method.godot_virtual.c_str();
		desc.GodotVirtualLen = int32_t(method.godot_virtual.size());
		desc.Line = method.line;
		desc.Column = method.column;
		method_descs.push_back(desc);
	}
	return answer_list(method_descs, r_methods, r_count);
}

int32_t Runtime::signal_list(const char *p_name, const vh_signal_desc **r_signals, int32_t *r_count) {
	const SidecarClass *entry = find(p_name);
	if (entry == nullptr) {
		return answer_not_found(r_signals, r_count);
	}
	signal_descs.clear();
	signal_args.clear();
	size_t total = 0;
	for (const SidecarSignal &signal : entry->signals) {
		total += signal.args.size();
	}
	signal_args.reserve(total);
	for (const SidecarSignal &signal : entry->signals) {
		vh_signal_desc desc = {};
		desc.NameUtf8 = signal.name.c_str();
		desc.NameLen = int32_t(signal.name.size());
		desc.Args = signal.args.empty() ? nullptr : signal_args.data() + signal_args.size();
		desc.ArgCount = int32_t(signal.args.size());
		for (const SidecarParam &arg : signal.args) {
			signal_args.push_back(param_desc(arg));
		}
		desc.Line = signal.line;
		desc.Column = signal.column;
		desc.Reject = signal.reject;
		desc.RejectDetailUtf8 = signal.reject_detail.c_str();
		desc.RejectDetailLen = int32_t(signal.reject_detail.size());
		signal_descs.push_back(desc);
	}
	return answer_list(signal_descs, r_signals, r_count);
}

int32_t Runtime::rpc_list(const char *p_name, const vh_rpc_desc **r_rpcs, int32_t *r_count) {
	const SidecarClass *entry = find(p_name);
	if (entry == nullptr) {
		return answer_not_found(r_rpcs, r_count);
	}
	rpc_descs.clear();
	for (const SidecarRpc &rpc : entry->rpcs) {
		vh_rpc_desc desc = {};
		desc.NameUtf8 = rpc.name.c_str();
		desc.NameLen = int32_t(rpc.name.size());
		desc.RpcMode = rpc.mode;
		desc.CallLocal = rpc.call_local ? 1 : 0;
		desc.TransferMode = rpc.transfer;
		desc.Channel = rpc.channel;
		desc.Line = rpc.line;
		desc.Column = rpc.column;
		desc.Reject = rpc.reject;
		desc.RejectDetailUtf8 = rpc.reject_detail.c_str();
		desc.RejectDetailLen = int32_t(rpc.reject_detail.size());
		rpc_descs.push_back(desc);
	}
	return answer_list(rpc_descs, r_rpcs, r_count);
}

void Runtime::fill_value(const SidecarValue &p_value, vh_value &r_value) {
	r_value = vh_value{};
	r_value.Type = p_value.type;
	r_value.VariantTag = p_value.tag;
	switch (p_value.type) {
		case VH_TYPE_LOGIC:
			r_value.Logic = p_value.logic ? 1 : 0;
			break;
		case VH_TYPE_INT:
			r_value.Int = p_value.integer;
			break;
		case VH_TYPE_FLOAT:
			r_value.Float = p_value.number;
			break;
		case VH_TYPE_CHAR:
			r_value.Char = p_value.code_point;
			break;
		case VH_TYPE_STRING:
			r_value.String.Utf8 = p_value.text.c_str();
			r_value.String.Len = int32_t(p_value.text.size());
			break;
		case VH_TYPE_ARRAY:
		case VH_TYPE_TUPLE:
		case VH_TYPE_OPTION: {
			if (p_value.elements.empty()) {
				break;
			}
			// A block per sequence, so a nested one's items stay where they were written however many
			// blocks follow it.
			static_value_blocks.emplace_back(p_value.elements.size());
			const size_t block = static_value_blocks.size() - 1;
			for (size_t index = 0; index < p_value.elements.size(); ++index) {
				fill_value(p_value.elements[index], static_value_blocks[block][index]);
			}
			const vh_value *items = static_value_blocks[block].data();
			if (p_value.type == VH_TYPE_OPTION) {
				r_value.Option = items;
			} else {
				r_value.Seq.Items = items;
				r_value.Seq.Count = int32_t(p_value.elements.size());
			}
			break;
		}
		default:
			break;
	}
}

int32_t Runtime::static_list(const char *p_name, const vh_static_desc **r_statics, int32_t *r_count) {
	const SidecarClass *entry = find(p_name);
	if (entry == nullptr) {
		return answer_not_found(r_statics, r_count);
	}
	static_descs.clear();
	static_value_blocks.clear();
	for (const SidecarStatic &member : entry->statics) {
		vh_static_desc desc = {};
		desc.NameUtf8 = member.name.c_str();
		desc.NameLen = int32_t(member.name.size());
		desc.IsFunction = member.is_function ? 1 : 0;
		if (!member.is_function) {
			fill_value(member.value, desc.Value);
		}
		desc.Line = member.line;
		desc.Column = member.column;
		static_descs.push_back(desc);
	}
	return answer_list(static_descs, r_statics, r_count);
}

int32_t Runtime::export_list(const char *p_name, const vh_export_desc **r_exports, int32_t *r_count) {
	const SidecarClass *entry = find(p_name);
	if (entry == nullptr) {
		return answer_not_found(r_exports, r_count);
	}
	export_descs.clear();
	for (const SidecarExport &member : entry->exports) {
		vh_export_desc desc = {};
		desc.NameUtf8 = member.name.c_str();
		desc.NameLen = int32_t(member.name.size());
		desc.Type = vh_type(member.type);
		desc.VariantTag = member.tag;
		desc.ElementVariantTag = member.element_tag;
		desc.IsVar = member.is_var ? 1 : 0;
		desc.Hint = member.hint;
		desc.HintStringUtf8 = member.hint_string.c_str();
		desc.HintStringLen = int32_t(member.hint_string.size());
		desc.NativeClassUtf8 = member.native_class.c_str();
		desc.NativeClassLen = int32_t(member.native_class.size());
		desc.RangeMin = member.range_min;
		desc.RangeMax = member.range_max;
		desc.HasRangeMin = member.has_range_min ? 1 : 0;
		desc.HasRangeMax = member.has_range_max ? 1 : 0;
		desc.GroupKind = member.group_kind;
		desc.GroupNameUtf8 = member.group_name.c_str();
		desc.GroupNameLen = int32_t(member.group_name.size());
		desc.Line = member.line;
		desc.Column = member.column;
		desc.Reject = member.reject;
		export_descs.push_back(desc);
	}
	return answer_list(export_descs, r_exports, r_count);
}

} // namespace vm
