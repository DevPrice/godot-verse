#pragma once

#include "vm_heap.h"

namespace vm {

// The entry point over the interpreter: a switch loop over decoded ops, no tail-call threading
// and no computed goto (design §7.4 -- portable is the point, and wasm rules the second one out
// anyway). Skeleton only: T3.4 builds move/control/arithmetic/call dispatch, T3.5 failure
// contexts, T3.6 objects, T3.7 unification, T3.8 the native surface, and T4.* the task ops.
class Interpreter {
public:
	explicit Interpreter(Heap &p_heap) :
			heap(p_heap) {}

	Heap &heap;
};

} // namespace vm
