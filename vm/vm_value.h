#pragma once

#include <cstdint>

namespace vm {

// A Verse value: a tagged 64-bit word on both build targets, deliberately not sizeof(void*) --
// that is 4 on wasm32 (design §7.3). The tag/payload layout (small ints, doubles, chars and
// 32-bit cell indices inline, cells for everything else) is T3.2's to build; this only reserves
// the name and size so vm_heap.h and vm_interpreter.h have a type to refer to.
struct Value {
	uint64_t bits = 0;
};

} // namespace vm
