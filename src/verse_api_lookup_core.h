#pragma once

#include "verse_api_classes.h"

#include <cstddef>
#include <iterator>
#include <string>

// The godot-cpp-free half of verse_api_lookup.cpp's class-name lookup: verse_godot_class_for and
// mirrored_class are thin wrappers over these two, kept apart the way verse_bindings.cpp's naming
// rules are so tests/verse_api_lookup can exercise the linear scan and its round trip without
// linking godot-cpp (docs/architecture-review.md item 3 step 5).

inline const char *verse_api_godot_name_for(const std::string &p_verse_class) {
	for (size_t i = 0; i < std::size(verse_api::classes); i++) {
		if (p_verse_class == verse_api::classes[i].verse_name) {
			return verse_api::classes[i].godot_name;
		}
	}
	return nullptr;
}

inline const char *verse_api_verse_name_for(const std::string &p_godot_class) {
	for (size_t i = 0; i < std::size(verse_api::classes); i++) {
		if (p_godot_class == verse_api::classes[i].godot_name) {
			return verse_api::classes[i].verse_name;
		}
	}
	return nullptr;
}
