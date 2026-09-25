#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vm_cell.h"
#include "vm_heap.h"

// program.vbc into cells (format.md). Nothing runs: the file is a program whose initialization has
// already run, and running it again would build a second of every class (spec/modules.md §2).
namespace vm {

enum class ClassOrigin : uint8_t {
	Script = 0,
	Mirrored = 1,
	Binding = 2,
};

struct ClassIndexEntry {
	ClassOrigin origin = ClassOrigin::Script;
	const NameCell *name = nullptr;
	const ClassCell *class_cell = nullptr;
};

struct Program {
	uint64_t abi = 0;
	std::string host_id;
	std::string engine_commit;
	uint64_t generation = 0;

	std::vector<const PackageCell *> packages;
	const PackageCell *builtin_package = nullptr;
	const FunctionCell *missing_procedure = nullptr;

	const ClassCell *task_class = nullptr;
	const EnumeratorCell *accessor_enumerator = nullptr;

	std::vector<ClassIndexEntry> classes;

	size_t cell_count = 0;
	size_t procedure_count = 0;
	size_t op_count = 0;
	size_t native_count = 0;
	size_t unbound_native_count = 0;

	const ClassIndexEntry *find_class(ClassOrigin p_origin, std::string_view p_name) const;
	// The class index's entry for a class cell, or null for a class the index does not name.
	const ClassIndexEntry *entry_for(const ClassCell *p_class) const;

	std::unordered_map<std::string, size_t> classes_by_name[3];
	std::unordered_map<const ClassCell *, size_t> classes_by_cell;
};

// Loads a whole .vbc into r_heap. On failure answers false with a sentence naming p_path; the heap
// may then hold cells nothing roots, which the next collection frees.
bool load_program(Heap &r_heap, const uint8_t *p_data, size_t p_size, const std::string &p_path, Program &r_program, std::string &r_error);

} // namespace vm
