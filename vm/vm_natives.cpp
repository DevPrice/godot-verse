#include "vm_natives.h"

namespace vm {

namespace {

struct NativeBinding {
	const char *key;
	NativeFn implementation;
};

// Empty until the natives land (T3.4's Print, T3.8, T4.3, T5.2); the null row only keeps the
// array from being zero-length.
constexpr NativeBinding kNatives[] = {
	{ nullptr, nullptr },
};

} // namespace

const char *const kMissingProcedureKey = "(/Verse.org/Verse/(/Verse.org/Verse:)MissingProcedure:)Native";
const char *const kMissingProcedureName = "(/Verse.org/Verse:)MissingProcedure";

NativeFn native_implementation(std::string_view p_binding_key) {
	for (const NativeBinding &binding : kNatives) {
		if (binding.key != nullptr && p_binding_key == binding.key) {
			return binding.implementation;
		}
	}
	return nullptr;
}

Outcome native_not_implemented(NativeCall &r_call) {
	r_call.error.diagnostic = "ErrRuntime_NativeInternal";
	r_call.error.description = "An internal runtime error occurred in native code that was called from Verse. There is no other information available.";
	r_call.error.message = "The native function " + r_call.procedure->binding_key->text + " is not implemented by this runtime.";
	return Outcome::Error;
}

Outcome native_missing_procedure(NativeCall &r_call) {
	r_call.error.diagnostic = "ErrRuntime_InvalidFunctionCall";
	r_call.error.description = "Attempted to call an invalid function.";
	r_call.error.message = "Attempted to call an uninitialized function.";
	return Outcome::Error;
}

} // namespace vm
