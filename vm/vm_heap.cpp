#include "vm_heap.h"

namespace vm {

namespace {

class Marker : public CellVisitor {
public:
	std::vector<const Cell *> pending;

	void visit(Value p_value) override {
		if (!p_value.is_cell()) {
			return;
		}
		Cell *cell = p_value.as_cell();
		if (!cell->marked) {
			cell->marked = true;
			pending.push_back(cell);
		}
	}
	using CellVisitor::visit;

	void drain() {
		while (!pending.empty()) {
			const Cell *cell = pending.back();
			pending.pop_back();
			cell->visit_references(*this);
		}
	}
};

} // namespace

Heap::Heap() {
	false_cell = make<LogicCell>(false);
	true_cell = make<LogicCell>(true);
}

Heap::~Heap() {
	Cell *cell = first_allocated;
	while (cell != nullptr) {
		Cell *next = cell->next_allocated;
		delete cell;
		cell = next;
	}
}

const NameCell *Heap::intern(std::string_view p_text) {
	std::string key(p_text);
	auto found = interned.find(key);
	if (found != interned.end()) {
		return found->second;
	}
	const NameCell *name = make<NameCell>(key);
	interned.emplace(std::move(key), name);
	return name;
}

size_t Heap::collect() {
	Marker marker;
	marker.visit(false_value());
	marker.visit(true_value());
	// Interned names are held strongly: a field name must stay the same cell for the program's life.
	for (const auto &entry : interned) {
		marker.visit(entry.second);
	}
	for (Value root : permanent_roots) {
		marker.visit(root);
	}
	for (const Value *slot : root_slots) {
		marker.visit(*slot);
	}
	marker.drain();

	size_t freed = 0;
	Cell **link = &first_allocated;
	while (*link != nullptr) {
		Cell *cell = *link;
		if (cell->marked) {
			cell->marked = false;
			link = &cell->next_allocated;
		} else {
			*link = cell->next_allocated;
			delete cell;
			++freed;
		}
	}
	cell_count -= freed;
	return freed;
}

} // namespace vm
