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

const NameCell *Heap::find_interned(std::string_view p_text) const {
	const auto found = interned.find(std::string(p_text));
	return found == interned.end() ? nullptr : found->second;
}

bool Heap::owns(const Cell *p_cell) const {
	for (const Cell *cell = first_allocated; cell != nullptr; cell = cell->next_allocated) {
		if (cell == p_cell) {
			return true;
		}
	}
	return false;
}

void Heap::remove_root_source(RootSource *p_source) {
	for (size_t index = 0; index < root_sources.size(); ++index) {
		if (root_sources[index] == p_source) {
			root_sources.erase(root_sources.begin() + ptrdiff_t(index));
			return;
		}
	}
}

size_t Heap::collect() {
	Marker marker;
	marker.visit(false_value());
	marker.visit(true_value());
	// Interned names are held strongly. Names compare by cell identity -- layouts, named arguments
	// and the host's method lookup key on the pointer -- and every name is the program's own text or
	// one of a handful a native asks for, so the table cannot grow with what a script does.
	for (const auto &entry : interned) {
		marker.visit(entry.second);
	}
	for (Value root : permanent_roots) {
		marker.visit(root);
	}
	for (const Value *slot : root_slots) {
		marker.visit(*slot);
	}
	for (const Value *slot : handle_roots) {
		marker.visit(*slot);
	}
	for (const RootSource *source : root_sources) {
		source->visit_roots(marker);
	}
	marker.drain();
	for (RootSource *source : root_sources) {
		source->sweep_weak();
	}

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
	live_after_collect = cell_count;
	allocated_since_collect = 0;
	++collections;
	const std::vector<RootSource *> sources = root_sources;
	for (RootSource *source : sources) {
		source->after_collect();
	}
	return freed;
}

} // namespace vm
