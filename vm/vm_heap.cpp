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

// An allowlist, so a kind added later is remembered until someone shows it cannot change.
bool is_immutable_after_creation(CellKind p_kind) {
	switch (p_kind) {
		case CellKind::False:
		case CellKind::True:
		case CellKind::BuiltinPackage:
		case CellKind::Name:
		case CellKind::Array:
		case CellKind::Map:
		case CellKind::Option:
		case CellKind::HeapInt:
		case CellKind::Rational:
		case CellKind::Procedure:
		case CellKind::NativeProcedure:
		case CellKind::Function:
		case CellKind::Scope:
		case CellKind::Class:
		case CellKind::Archetype:
		case CellKind::AccessSpecifier:
		case CellKind::Enumeration:
		case CellKind::Enumerator:
		case CellKind::Package:
		case CellKind::Module:
		case CellKind::IntType:
		case CellKind::FloatType:
		case CellKind::TupleType:
		case CellKind::MapType:
		case CellKind::SimpleType:
		case CellKind::Accessor:
		case CellKind::ArrayType:
		case CellKind::OptionType:
		case CellKind::PointerType:
			return true;
		default:
			return false;
	}
}

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
	untenured_names.push_back(name);
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
	for (const NameCell *name : untenured_names) {
		marker.visit(name);
	}
	for (const Cell *cell : remembered) {
		cell->visit_references(marker);
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
	while (*link != first_tenured) {
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
	live_after_collect = cell_count - tenured_count;
	allocated_since_collect = 0;
	++collections;
	const std::vector<RootSource *> sources = root_sources;
	for (RootSource *source : sources) {
		source->after_collect();
	}
	return freed;
}

void Heap::tenure() {
	collect();
	for (Cell *cell = first_allocated; cell != first_tenured; cell = cell->next_allocated) {
		cell->marked = true;
		cell->tenured = true;
		++tenured_count;
		if (!is_immutable_after_creation(cell->kind)) {
			remembered.push_back(cell);
		}
	}
	first_tenured = first_allocated;
	untenured_names.clear();
	live_after_collect = 0;
}

} // namespace vm
