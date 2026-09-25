#pragma once

#include <cstdint>

#include "vm_value.h"

namespace vm {

// The precise mark-sweep collector over 32-bit cell indices (design §7.3): package roots, live
// tasks and their frames, instance and callback handles the host holds, and an explicit root
// stack for a native mid-call. Skeleton only -- no cell is allocated yet. T3.2 gives cells their
// real shapes (values, classes, procedures...) and T3.9 builds collection itself; both need a
// type to hang off, which is what this reserves.
class Heap {
public:
	Heap() = default;
};

} // namespace vm
