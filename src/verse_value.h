#pragma once

#include "verse_host_abi.h"

#include <godot_cpp/variant/variant.hpp>

#include <memory>
#include <vector>

// A vh_arena the GDExtension owns, for the one direction where the host does not hand one over: a
// value written with vh_instance_set_field is built on this side, and everything a vh_value points
// at rather than holds -- a string's bytes, the items of a tuple or an array -- has to outlive the
// call.
//
// Every block is its own allocation and none is ever moved. A bump allocator that grew by
// reallocating would invalidate a pointer it had already handed out, which for a nested value means
// the outer one ends up pointing at freed memory.
class VerseArena {
public:
	VerseArena();
	vh_arena *get() { return &arena; }

private:
	static void *alloc(vh_arena *p_self, size_t p_size, size_t p_align);

	// First member, and it has to stay first: Alloc is handed the vh_arena the ABI knows about and
	// nothing else, so it casts that address back to the owner.
	vh_arena arena = {};
	std::vector<std::unique_ptr<uint8_t[]>> blocks;
};

// Converts one Variant into a vh_value. Any string or container data the value needs is
// allocated out of p_arena, which must outlive the call this vh_value is handed to.
//
// Returns false (leaving r_out unspecified) when the Variant holds a type with no vh_value
// representation, or when an arena allocation fails; never crashes either way.
bool variant_to_vh(const godot::Variant &p_value, vh_arena *p_arena, vh_value &r_out);

// The inverse of variant_to_vh. A vh_value with no Variant representation converts to a nil
// Variant.
godot::Variant vh_to_variant(const vh_value &p_value);

// The Godot type a value of this (vh_type, vh_variant_tag) pair rebuilds as, for the places that
// have a declaration but no value: a method's parameter and return types.
//
// The tag wins where it says anything, because it is Godot's own Variant::Type and is numbered to
// match; VH_VARIANT_NIL means it has nothing to say and the layout decides.
godot::Variant::Type variant_type_for(int32_t p_type, int32_t p_variant_tag);
