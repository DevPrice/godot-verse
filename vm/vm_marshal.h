#pragma once

#include <cstdint>
#include <string>

#include "verse_host_abi.h"
#include "vm_heap.h"
#include "vm_value.h"

// vh_value <-> Verse values for vh_instance_call, by the declared vh_type the sidecar records
// (spec/godot-natives.md §12.3). Scalars, strings, arrays, tuples, options and maps; objects,
// references, variants, structs and enums are T5.1's.
namespace vm {

// An argument for a parameter declared p_type. False when the wire value has no Verse spelling
// for that declaration.
bool wire_to_value(Heap &r_heap, const vh_value &p_wire, int32_t p_type, Value &r_value);

// A result declared p_type (tagged p_tag for the consumer), written into p_arena. False with
// r_why when it cannot be carried.
bool value_to_wire(Value p_value, int32_t p_type, int32_t p_tag, vh_arena *p_arena, vh_value &r_wire, std::string &r_why);

} // namespace vm
