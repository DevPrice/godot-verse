#include "verse_value.h"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <cstring>

using namespace godot;

namespace {

bool alloc_string(vh_arena *p_arena, const CharString &p_utf8, vh_value &r_out) {
	const int32_t len = (int32_t)p_utf8.length();
	r_out.Type = VH_TYPE_STRING;
	if (len == 0) {
		r_out.String.Utf8 = "";
		r_out.String.Len = 0;
		return true;
	}

	void *bytes = p_arena->Alloc(p_arena, (size_t)len, 1);
	if (bytes == nullptr) {
		return false;
	}
	memcpy(bytes, p_utf8.get_data(), (size_t)len);
	r_out.String.Utf8 = static_cast<const char *>(bytes);
	r_out.String.Len = len;
	return true;
}

vh_value *alloc_values(vh_arena *p_arena, int32_t p_count) {
	if (p_count <= 0) {
		return nullptr;
	}
	void *mem = p_arena->Alloc(p_arena, sizeof(vh_value) * (size_t)p_count, alignof(vh_value));
	if (mem == nullptr) {
		return nullptr;
	}
	memset(mem, 0, sizeof(vh_value) * (size_t)p_count);
	return static_cast<vh_value *>(mem);
}

vh_pair *alloc_pairs(vh_arena *p_arena, int32_t p_count) {
	if (p_count <= 0) {
		return nullptr;
	}
	void *mem = p_arena->Alloc(p_arena, sizeof(vh_pair) * (size_t)p_count, alignof(vh_pair));
	if (mem == nullptr) {
		return nullptr;
	}
	memset(mem, 0, sizeof(vh_pair) * (size_t)p_count);
	return static_cast<vh_pair *>(mem);
}

bool tuple_from_xy(double p_x, double p_y, vh_arena *p_arena, vh_value &r_out) {
	vh_value *items = alloc_values(p_arena, 2);
	if (items == nullptr) {
		return false;
	}
	items[0].Type = VH_TYPE_FLOAT;
	items[0].Float = p_x;
	items[1].Type = VH_TYPE_FLOAT;
	items[1].Float = p_y;

	r_out.Type = VH_TYPE_TUPLE;
	r_out.Seq.Items = items;
	r_out.Seq.Count = 2;
	return true;
}

} // namespace

bool variant_to_vh(const Variant &p_value, vh_arena *p_arena, vh_value &r_out) {
	r_out = vh_value{};

	switch (p_value.get_type()) {
		case Variant::NIL:
			r_out.Type = VH_TYPE_VOID;
			return true;

		case Variant::BOOL:
			r_out.Type = VH_TYPE_LOGIC;
			r_out.Logic = (bool)p_value ? 1 : 0;
			return true;

		case Variant::INT:
			r_out.Type = VH_TYPE_INT;
			r_out.Int = (int64_t)p_value;
			return true;

		case Variant::FLOAT:
			r_out.Type = VH_TYPE_FLOAT;
			r_out.Float = (double)p_value;
			return true;

		case Variant::STRING:
			return alloc_string(p_arena, String(p_value).utf8(), r_out);

		case Variant::STRING_NAME:
			return alloc_string(p_arena, String(StringName(p_value)).utf8(), r_out);

		case Variant::NODE_PATH:
			return alloc_string(p_arena, String(NodePath(p_value)).utf8(), r_out);

		case Variant::VECTOR2: {
			const Vector2 v = p_value;
			return tuple_from_xy((double)v.x, (double)v.y, p_arena, r_out);
		}

		case Variant::VECTOR2I: {
			const Vector2i v = p_value;
			return tuple_from_xy((double)v.x, (double)v.y, p_arena, r_out);
		}

		case Variant::ARRAY: {
			const Array arr = p_value;
			const int32_t count = (int32_t)arr.size();
			vh_value *items = nullptr;
			if (count > 0) {
				items = alloc_values(p_arena, count);
				if (items == nullptr) {
					return false;
				}
				for (int32_t i = 0; i < count; i++) {
					if (!variant_to_vh(arr[i], p_arena, items[i])) {
						return false;
					}
				}
			}
			r_out.Type = VH_TYPE_ARRAY;
			r_out.Seq.Items = items;
			r_out.Seq.Count = count;
			return true;
		}

		case Variant::DICTIONARY: {
			const Dictionary dict = p_value;
			const Array keys = dict.keys();
			const int32_t count = (int32_t)keys.size();
			vh_pair *pairs = nullptr;
			if (count > 0) {
				pairs = alloc_pairs(p_arena, count);
				if (pairs == nullptr) {
					return false;
				}
				for (int32_t i = 0; i < count; i++) {
					const Variant key = keys[i];
					if (!variant_to_vh(key, p_arena, pairs[i].Key)) {
						return false;
					}
					if (!variant_to_vh(dict[key], p_arena, pairs[i].Value)) {
						return false;
					}
				}
			}
			r_out.Type = VH_TYPE_MAP;
			r_out.Map.Pairs = pairs;
			r_out.Map.Count = count;
			return true;
		}

		default:
			return false;
	}
}

Variant vh_to_variant(const vh_value &p_value) {
	switch (p_value.Type) {
		case VH_TYPE_VOID:
			return Variant();

		case VH_TYPE_LOGIC:
			return Variant(p_value.Logic != 0);

		case VH_TYPE_INT:
			return Variant(p_value.Int);

		case VH_TYPE_FLOAT:
			return Variant(p_value.Float);

		case VH_TYPE_STRING:
			return p_value.String.Len > 0 ? Variant(String::utf8(p_value.String.Utf8, p_value.String.Len)) : Variant(String());

		case VH_TYPE_ARRAY: {
			Array arr;
			for (int32_t i = 0; i < p_value.Seq.Count; i++) {
				arr.push_back(vh_to_variant(p_value.Seq.Items[i]));
			}
			return arr;
		}

		case VH_TYPE_MAP: {
			Dictionary dict;
			for (int32_t i = 0; i < p_value.Map.Count; i++) {
				const vh_pair &pair = p_value.Map.Pairs[i];
				dict[vh_to_variant(pair.Key)] = vh_to_variant(pair.Value);
			}
			return dict;
		}

		case VH_TYPE_TUPLE:
			if (p_value.Seq.Count == 2 && p_value.Seq.Items != nullptr &&
					p_value.Seq.Items[0].Type == VH_TYPE_FLOAT && p_value.Seq.Items[1].Type == VH_TYPE_FLOAT) {
				return Variant(Vector2((real_t)p_value.Seq.Items[0].Float, (real_t)p_value.Seq.Items[1].Float));
			}
			return Variant();

		default:
			return Variant();
	}
}
