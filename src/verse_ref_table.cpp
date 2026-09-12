#include "verse_ref_table.h"

using namespace godot;

int64_t VerseRefTable::mint(const Variant &p_value) {
	const int64_t id = next_id++;
	Entry entry;
	entry.value = p_value;
	entry.claims = 1;
	entries.insert(id, entry);
	return id;
}

int64_t VerseRefTable::retain(int64_t p_id) {
	Entry *entry = entries.getptr(p_id);
	if (entry == nullptr) {
		return 0;
	}
	entry->claims++;
	return p_id;
}

void VerseRefTable::release(int64_t p_id) {
	Entry *entry = entries.getptr(p_id);
	if (entry == nullptr) {
		return;
	}
	if (--entry->claims <= 0) {
		entries.erase(p_id);
	}
}

const Variant *VerseRefTable::find(int64_t p_id) const {
	const Entry *entry = entries.getptr(p_id);
	return entry != nullptr ? &entry->value : nullptr;
}

bool VerseRefTable::assign(int64_t p_id, const Variant &p_value) {
	Entry *entry = entries.getptr(p_id);
	if (entry == nullptr) {
		return false;
	}
	entry->value = p_value;
	return true;
}

void VerseRefTable::clear() {
	entries.clear();
	// next_id is deliberately not reset: an id the host is still holding must not come back
	// meaning something else, and the host outlives an unload only in the sense that it may be
	// mid-teardown when this runs.
}

VerseRefTable &verse_ref_table() {
	// Deliberately never destroyed. The entries are Variants -- Arrays, Dictionaries, Callables --
	// and a static destructor runs at process exit, by which time Godot's object database and
	// allocator are gone; freeing one there is a crash rather than a tidy-up. unload_host empties
	// it while Godot is still alive, which is the moment that actually frees anything.
	static VerseRefTable *table = new VerseRefTable();
	return *table;
}
