#pragma once

#include <cstdint>
#include <string_view>

#include "vm_cell.h"
#include "vm_heap.h"
#include "vm_status.h"
#include "vm_value.h"

// Natives by binding key (spec/natives.md §3.2). The loader binds every native procedure cell to
// the implementation this table names for its key, or to the stand-in.
namespace vm {

class Interpreter;
struct Program;

// The real class of a library type a native builds an object of (a message, a diagnostic, an
// event_subscription), found by its package-definitions decorated path (format.md §7) rather than
// fabricated, so a script's field access on the result, or a class check against it, sees the
// class the rest of the program does. Null when the program has no such definition.
const ClassCell *find_library_class(const Program &p_program, std::string_view p_decorated_path);

// One call of a native: its Self and its positional arguments already adapted to the native's
// count (spec/natives.md §3.3, §3.4). An implementation answers Ok with `result`, Fail, or Error
// with `error` filled. `interpreter` is the running one, null when a test calls a native bare.
struct NativeCall {
	Heap &heap;
	Interpreter *interpreter = nullptr;
	const NativeProcedureCell *procedure = nullptr;
	Value self;
	const Value *arguments = nullptr;
	uint32_t argument_count = 0;
	Value result;
	RuntimeError error;

	explicit NativeCall(Heap &r_heap) :
			heap(r_heap) {}
};

// The argument already followed through placeholders (spec/natives.md §3.4, §3.6).
Value argument(const NativeCall &p_call, uint32_t p_index);
// True (with the placeholder left in r_call.result, for the caller to answer Outcome::Park) when
// any argument is not yet concrete. Shared by every native in vm_natives.cpp and vm_math.cpp.
bool unbound_argument(NativeCall &r_call);

// The implementation for p_binding_key, or null when this runtime has none.
NativeFn native_implementation(std::string_view p_binding_key);

// What every native with no implementation is bound to.
Outcome native_not_implemented(NativeCall &r_call);

// The built-in package's missing-procedure function (spec/calls.md §10.1).
extern const char *const kMissingProcedureKey;
extern const char *const kMissingProcedureName;
Outcome native_missing_procedure(NativeCall &r_call);

} // namespace vm
