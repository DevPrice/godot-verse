#pragma once

#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <cstdint>

// The reference table the ABI's `Ref` lane names (see include/verse_host_abi.h, "reference
// values").
//
// godot-cpp splits Godot's types in two and this follows it: a value type is copied, and a
// reference type is a handle into engine-owned storage. `Array` and `Dictionary` have reference
// semantics an author can observe -- a Dictionary passed to a function and mutated is mutated for
// the caller -- so copying them across would silently change what the program means. `Callable` and
// `Signal` cannot be decomposed into scalars at all, and the ten packed arrays are value types but
// variable-length ones, which a fixed-width variant has no lanes for.
//
// `Object` is deliberately not here: Godot's instance id is already a stable name for an object, so
// it needs no table entry.
//
// Lifetime. An id handed to the host is retained until the host releases it, which it does when the
// Verse value wrapping it is collected. Release is therefore deferred by up to one collection
// cycle. An id minted for a call that then raises is leaked, because the raise unwinds past the
// point where Verse would have wrapped it -- bounded by how often a script raises, and preferable
// to the alternative of releasing eagerly and handing Verse an id that names nothing.
class VerseRefTable {
public:
	// Stores a value and returns a fresh id already claimed once. Never returns 0, which the ABI
	// reserves for "no reference".
	int64_t mint(const godot::Variant &p_value);

	// A second claim on an id, for a Verse value that was copied. Returns the id, or 0 if it names
	// nothing -- which is a host that kept an id past its release, and worth failing loudly on.
	int64_t retain(int64_t p_id);

	// Drops one claim. The entry goes when the last one does.
	void release(int64_t p_id);

	// The value an id names, or null. Borrowed: the table owns it, and a caller that wants to keep
	// it past the next mutation of the same entry must copy it.
	const godot::Variant *find(int64_t p_id) const;

	// Replaces what an id names, for the mutating container operations. False for an unknown id.
	bool assign(int64_t p_id, const godot::Variant &p_value);

	// Everything, dropped. Called when the host unloads: the ids only mean anything to a host that
	// is still running, and the values are Godot's to free.
	void clear();

	int64_t size() const { return entries.size(); }

private:
	struct Entry {
		godot::Variant value;
		int32_t claims = 0;
	};

	// Monotonic, and never reused even after an entry goes. A reused id would let a host holding a
	// released one reach a value it has no claim on, which is the one failure mode a table like
	// this exists to make impossible.
	int64_t next_id = 1;
	godot::HashMap<int64_t, Entry> entries;
};

// The process's table. One, because the ids in it are handed to a host there is also only one of;
// VerseRuntime clears it when that host unloads.
VerseRefTable &verse_ref_table();
