#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vm_heap.h"
#include "vm_status.h"
#include "vm_value.h"

// Strings, arrays, maps, options, Melt and Freeze: spec/values.md §4-§10 and spec/ops.md §8.3, §9.
// Every operand is followed through bound placeholders first. Where an operation must wait on an
// unbound one it answers Park with the placeholder in its result.
namespace vm {

Value make_string(Heap &r_heap, std::string_view p_bytes);
// The bytes of an array whose every element is a char, or of `false` (spec/values.md §5.2).
bool string_bytes(Value p_value, std::string &r_bytes);

// ToString(:char) and ToString(:char32) (spec/values.md §4). A code point with no UTF-8 encoding
// -- above U+10FFFF -- becomes U+FFFD; a surrogate is encoded as its three bytes.
std::string char32_to_utf8(uint32_t p_code_point);

// ToString of an int, a float, a char, a char32 or a string (spec/values.md §14).
Outcome value_to_string(Heap &r_heap, Value p_value, Value &r_result, RuntimeError &r_error);

Value make_array(Heap &r_heap, const std::vector<Value> &p_elements, bool p_mutable);

// Length (spec/ops.md §9): arrays, maps and `false`.
Outcome value_length(Value p_container, int64_t &r_length);

// `Call` on an array and ArrayIndexFastFail: fails for `false` (spec/values.md §16.1 Q1) and for
// any index that is not an integer in [0, length).
Outcome array_index(Value p_array, Value p_index, Value &r_result);

// Add on arrays: a new immutable array; `false` on either side answers the other unchanged.
Outcome array_concat(Heap &r_heap, Value p_left, Value p_right, Value &r_result);

// ArrayAdd: appends as is.
Outcome array_append(Value p_container, Value p_value);
// InPlaceMakeImmutable. Undoing it is setting the kind back to MutableArray.
Outcome array_make_immutable(Value p_container);
// FastAppendToArray: appends every element of p_right melted, or nothing if one is unbound.
// Undoing it is ArrayCell::truncate to the length before.
Outcome array_fast_append(Heap &r_heap, Value p_left, Value p_right, Value &r_parked);
// CallSet on a mutable array: fails for an index outside [0, length). r_old is what the element
// held, for the undo log.
Outcome array_set(Value p_container, Value p_index, Value p_value, Value &r_old);

// Where a mutable array's element or a mutable map's value is stored, which is where a hidden
// variable stands (spec/ops.md §3.1); null when there is no such element or entry. A packed string
// element has a slot only once p_spread unpacks the string, and nothing packed is ever a variable.
Value *element_slot(Value p_container, Value p_key, bool p_spread);

// NewMap: a repeated key takes the last value and the last position (spec/values.md §8.1).
Outcome make_map(Heap &r_heap, const std::vector<Value> &p_keys, const std::vector<Value> &p_values, Value &r_result);
// ConcatenateMaps: as a literal listing the left's entries then the right's.
Outcome concatenate_maps(Heap &r_heap, Value p_left, Value p_right, Value &r_result);
// `Call` on a map: the value for an equal key, or failure.
Outcome map_lookup(Value p_map, Value p_key, Value &r_result);
// CallSet on a mutable map: replaces in place or appends. r_inserted and r_old are for the undo log;
// undoing an insert is map_remove_last.
Outcome map_set(Value p_map, Value p_key, Value p_value, bool &r_inserted, Value &r_old);
void map_remove_last(Value p_map);
// Hashes every entry and builds the index, for a map whose entries were written directly: the
// loader's, whose keys are not concrete until every cell is filled.
void map_reindex(MapCell *r_map);
// MapKey and MapValue.
Outcome map_key_at(Value p_map, Value p_index, Value &r_result);
Outcome map_value_at(Value p_map, Value p_index, Value &r_result);

Value make_option(Heap &r_heap, Value p_content);
// Query: fails for `false`; `true` answers `false`.
Outcome option_query(Heap &r_heap, Value p_option, Value &r_result);

// Melt and Freeze (spec/values.md §7). Freezing an immutable array or map, or a value holding an
// unbound placeholder, is Invalid (spec/ops.md Freeze).
Outcome melt(Heap &r_heap, Value p_value, Value &r_result);
Outcome freeze(Heap &r_heap, Value p_value, Value &r_result);

} // namespace vm
