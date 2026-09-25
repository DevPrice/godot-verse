#pragma once

#include <cstdint>
#include <string>

namespace vm {

// What a value operation answers. Fail is Verse failure (spec/failure.md), not an error. Error is a
// Verse runtime error, described in a RuntimeError. Park means the operation met an unbound
// placeholder it must wait on, which it hands back in place of a result (spec/unification.md §6).
// Invalid means the operands are outside the contract (spec/values.md §15): a malformed program,
// which the interpreter reports naming the op and line. Yield is a native suspending the task
// (spec/calls.md §4.3), and a VM entry whose task suspended. Unsupported is bytecode this runtime
// cannot run yet: reported like a runtime error, but the script did nothing wrong.
enum class Outcome : uint8_t {
	Ok,
	Fail,
	Error,
	Park,
	Invalid,
	Yield,
	Unsupported,
};

struct RuntimeError {
	std::string diagnostic;
	std::string description;
	std::string message;
};

} // namespace vm
