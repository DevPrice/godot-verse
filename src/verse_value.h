#pragma once

#include "verse_host_abi.h"

#include <godot_cpp/variant/variant.hpp>

// Converts one Variant into a vh_value. Any string or container data the value needs is
// allocated out of p_arena, which must outlive the call this vh_value is handed to.
//
// Returns false (leaving r_out unspecified) when the Variant holds a type with no vh_value
// representation, or when an arena allocation fails; never crashes either way.
bool variant_to_vh(const godot::Variant &p_value, vh_arena *p_arena, vh_value &r_out);

// The inverse of variant_to_vh. A vh_value with no Variant representation converts to a nil
// Variant.
godot::Variant vh_to_variant(const vh_value &p_value);
