#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "vm_cell.h"
#include "vm_value.h"

namespace vm {

// Something outside the heap that holds cells: its roots are visited at every collection, and what
// it holds weakly it settles between marking and freeing, while an unreached cell is still intact.
class RootSource {
public:
	virtual ~RootSource() = default;
	virtual void visit_roots(CellVisitor &r_visitor) const = 0;
	// A cell nothing reached reads `marked` false here and is freed straight after.
	virtual void sweep_weak() {}
	// After every unreached cell is freed. The heap is consistent again, so this may call out to the
	// embedder, and the embedder may call back in.
	virtual void after_collect() {}
};

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
		++allocated_since_collect;
		return cell;
	}

	Value false_value() const { return Value::from_cell(false_cell); }
	Value true_value() const { return Value::from_cell(true_cell); }
	Value logic(bool p_value) const { return p_value ? true_value() : false_value(); }

	const NameCell *intern(std::string_view p_text);
	// The interned name for p_text, or null when none exists, without making one.
	const NameCell *find_interned(std::string_view p_text) const;

	// A root for a Value that lives outside the heap -- a native's local mid-call, a register file
	// the host is holding. Push and pop in stack order.
	void push_root(const Value *p_slot) { root_slots.push_back(p_slot); }
	void pop_root() { root_slots.pop_back(); }

	// Roots that live as long as the program: package cells and whatever else a loader pins.
	void add_permanent_root(Value p_value) { permanent_roots.push_back(p_value); }

	// A root whose lifetime is not stack-ordered: an instance the host holds (design §7.3).
	void add_handle_root(const Value *p_slot) { handle_roots.insert(p_slot); }
	void remove_handle_root(const Value *p_slot) { handle_roots.erase(p_slot); }

	void add_root_source(RootSource *p_source) { root_sources.push_back(p_source); }
	void remove_root_source(RootSource *p_source);

	// Marks from every root and frees what was not reached. Answers the number of cells freed.
	// Never call it mid-op: a Value held only in a C++ local is not a root. A tenured cell is
	// neither marked nor swept, so the cost is the untenured survivors plus the garbage.
	size_t collect();

	// Collects, then makes every surviving cell permanent: never marked, never swept, never freed
	// before the heap is. For the loaded program (spec/modules.md §5: module data is immutable), so
	// the cells it makes stop costing every collection a walk of the whole mirror.
	//
	// What makes skipping them sound is that a tenured cell can gain a reference to an untenured one
	// only if it can change after load, and only a few kinds can: an object (a field, a hidden
	// variable register_slot puts in one, the placeholder LoadField leaves in an empty slot, its
	// native state), a mutable array or map, and the run-time kinds a tenure after boot may catch --
	// a variable, a placeholder, a task, a frame, a content scope. Those stay in `remembered` and
	// have their references visited at every collection; every other kind is written only by the
	// loader or by the op that makes it (is_immutable_after_creation). Tenure outside an entry,
	// with nothing on the undo log: a rolled-back freeze is the one write that makes an array
	// mutable again.
	void tenure();

	// Whether enough has been allocated since the last collection to make another worth its cost:
	// as many cells as survived it untenured, and never fewer than `min_collect_trigger`.
	// Collecting when the untenured heap has doubled keeps the collector's share of the work
	// constant however large it grows.
	bool wants_collection() const {
		return allocated_since_collect >= (live_after_collect > min_collect_trigger ? live_after_collect : min_collect_trigger);
	}
	// With the conformance cook's 169k cells tenured, a collection costs what it frees: 65536 dead
	// cells take about 2 ms unoptimized and 1.2 ms at /O2, an idle one 0.02 ms
	// (`verse_vm_test --gc-bench`, docs/web-vm/measurements.md).
	size_t min_collect_trigger = 65536;

	size_t live_cell_count() const { return cell_count; }
	size_t tenured_cell_count() const { return tenured_count; }
	size_t remembered_cell_count() const { return remembered.size(); }
	size_t collection_count() const { return collections; }
	// Whether p_cell is a live cell of this heap. Walks every cell: for tests and assertions.
	bool owns(const Cell *p_cell) const;

private:
	// Cells are listed newest first, so the tenured ones are the list's tail from first_tenured on.
	Cell *first_allocated = nullptr;
	Cell *first_tenured = nullptr;
	size_t tenured_count = 0;
	std::vector<const Cell *> remembered;
	size_t cell_count = 0;
	size_t allocated_since_collect = 0;
	size_t live_after_collect = 0;
	size_t collections = 0;
	std::vector<RootSource *> root_sources;
	LogicCell *false_cell = nullptr;
	LogicCell *true_cell = nullptr;
	std::unordered_map<std::string, const NameCell *> interned;
	// The names interned since the last tenure: only these need marking.
	std::vector<const NameCell *> untenured_names;
	std::vector<const Value *> root_slots;
	std::vector<Value> permanent_roots;
	std::unordered_set<const Value *> handle_roots;
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
