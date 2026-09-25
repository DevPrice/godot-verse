#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "vm_cell.h"
#include "vm_value.h"

namespace vm {

// Owns every cell. Cells never move, so a Value holding a cell pointer stays valid until a
// collection finds the cell unreachable (design §7.3). Collection is precise: a cell is live only
// if it is reached from a root through CellVisitor, which is why every cell kind declares its
// references.
class Heap {
public:
	Heap();
	~Heap();
	Heap(const Heap &) = delete;
	Heap &operator=(const Heap &) = delete;

	template <typename T, typename... Args>
	T *make(Args &&...p_args) {
		T *cell = new T(std::forward<Args>(p_args)...);
		cell->next_allocated = first_allocated;
		first_allocated = cell;
		++cell_count;
		return cell;
	}

	Value false_value() const { return Value::from_cell(false_cell); }
	Value true_value() const { return Value::from_cell(true_cell); }
	Value logic(bool p_value) const { return p_value ? true_value() : false_value(); }

	const NameCell *intern(std::string_view p_text);

	// A root for a Value that lives outside the heap -- a native's local mid-call, a register file
	// the host is holding. Push and pop in stack order.
	void push_root(const Value *p_slot) { root_slots.push_back(p_slot); }
	void pop_root() { root_slots.pop_back(); }

	// Roots that live as long as the program: package cells and whatever else a loader pins.
	void add_permanent_root(Value p_value) { permanent_roots.push_back(p_value); }

	// Marks from every root and frees what was not reached. Answers the number of cells freed.
	// Never call it mid-op: a Value held only in a C++ local is not a root.
	size_t collect();

	size_t live_cell_count() const { return cell_count; }

private:
	Cell *first_allocated = nullptr;
	size_t cell_count = 0;
	LogicCell *false_cell = nullptr;
	LogicCell *true_cell = nullptr;
	std::unordered_map<std::string, const NameCell *> interned;
	std::vector<const Value *> root_slots;
	std::vector<Value> permanent_roots;
};

class RootScope {
public:
	RootScope(Heap &p_heap, const Value *p_slot) :
			heap(p_heap) { heap.push_root(p_slot); }
	~RootScope() { heap.pop_root(); }
	RootScope(const RootScope &) = delete;
	RootScope &operator=(const RootScope &) = delete;

private:
	Heap &heap;
};

} // namespace vm
