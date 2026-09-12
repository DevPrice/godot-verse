#include "verse_value.h"

#include "verse_ref_table.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/plane.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/transform2d.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector3i.hpp>
#include <godot_cpp/variant/vector4.hpp>
#include <godot_cpp/variant/vector4i.hpp>

#include <cstring>

using namespace godot;

VerseArena::VerseArena() {
	arena.Alloc = &VerseArena::alloc;
}

void *VerseArena::alloc(vh_arena *p_self, size_t p_size, size_t p_align) {
	// new[] is aligned for any fundamental type, which covers every alignment a vh_value asks for.
	if (p_size == 0 || p_align > alignof(std::max_align_t)) {
		return nullptr;
	}
	VerseArena *self = reinterpret_cast<VerseArena *>(p_self);
	self->blocks.push_back(std::make_unique<uint8_t[]>(p_size));
	return self->blocks.back().get();
}

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

// Every fixed-size math Variant crosses as a tuple of its components in Godot's own order, so
// one pair of helpers covers Vector2 through Projection.
bool floats_to_tuple(vh_arena *p_arena, const double *p_components, int32_t p_count, int32_t p_tag, vh_value &r_out) {
	vh_value *items = alloc_values(p_arena, p_count);
	if (items == nullptr) {
		return false;
	}
	for (int32_t i = 0; i < p_count; i++) {
		items[i].Type = VH_TYPE_FLOAT;
		items[i].Float = p_components[i];
	}
	r_out.Type = VH_TYPE_TUPLE;
	r_out.VariantTag = p_tag;
	r_out.Seq.Items = items;
	r_out.Seq.Count = p_count;
	return true;
}

// Reads up to p_max components out of a tuple or array value, zero-filling the rest, and
// returns how many were actually present.
int32_t tuple_to_floats(const vh_value &p_value, double *r_components, int32_t p_max) {
	for (int32_t i = 0; i < p_max; i++) {
		r_components[i] = 0.0;
	}
	if (p_value.Type != VH_TYPE_TUPLE && p_value.Type != VH_TYPE_ARRAY) {
		return 0;
	}
	const int32_t count = p_value.Seq.Count < p_max ? p_value.Seq.Count : p_max;
	for (int32_t i = 0; i < count; i++) {
		const vh_value &item = p_value.Seq.Items[i];
		if (item.Type == VH_TYPE_FLOAT) {
			r_components[i] = item.Float;
		} else if (item.Type == VH_TYPE_INT) {
			r_components[i] = (double)item.Int;
		}
	}
	return count;
}

// Vectors flatten componentwise, so a PackedVector2Array of n elements crosses as 2n floats.
template <typename PackedType>
bool packed_vectors_to_vh(vh_arena *p_arena, const PackedType &p_packed, int32_t p_stride, int32_t p_tag, vh_value &r_out) {
	const int32_t count = (int32_t)p_packed.size() * p_stride;
	vh_value *items = alloc_values(p_arena, count);
	if (count > 0 && items == nullptr) {
		return false;
	}
	for (int32_t i = 0; i < (int32_t)p_packed.size(); i++) {
		for (int32_t c = 0; c < p_stride; c++) {
			items[i * p_stride + c].Type = VH_TYPE_FLOAT;
			items[i * p_stride + c].Float = (double)p_packed[i][c];
		}
	}
	r_out.Type = VH_TYPE_ARRAY;
	r_out.VariantTag = p_tag;
	r_out.Seq.Items = items;
	r_out.Seq.Count = count;
	return true;
}

bool scalars_to_vh(vh_arena *p_arena, const Array &p_values, int32_t p_tag, vh_value &r_out) {
	const int32_t count = (int32_t)p_values.size();
	vh_value *items = alloc_values(p_arena, count);
	if (count > 0 && items == nullptr) {
		return false;
	}
	for (int32_t i = 0; i < count; i++) {
		if (!variant_to_vh(p_values[i], p_arena, items[i])) {
			return false;
		}
	}
	r_out.Type = VH_TYPE_ARRAY;
	r_out.VariantTag = p_tag;
	r_out.Seq.Items = items;
	r_out.Seq.Count = count;
	return true;
}

String string_of(const vh_value &p_value) {
	if (p_value.Type != VH_TYPE_STRING || p_value.String.Len <= 0) {
		return String();
	}
	return String::utf8(p_value.String.Utf8, p_value.String.Len);
}

int64_t int_of(const vh_value &p_value) {
	switch (p_value.Type) {
		case VH_TYPE_INT:
			return p_value.Int;
		case VH_TYPE_FLOAT:
			return (int64_t)p_value.Float;
		case VH_TYPE_LOGIC:
			return p_value.Logic != 0 ? 1 : 0;
		default:
			return 0;
	}
}

double double_of(const vh_value &p_value) {
	switch (p_value.Type) {
		case VH_TYPE_FLOAT:
			return p_value.Float;
		case VH_TYPE_INT:
			return (double)p_value.Int;
		case VH_TYPE_LOGIC:
			return p_value.Logic != 0 ? 1.0 : 0.0;
		default:
			return 0.0;
	}
}

Variant math_variant(int32_t p_tag, const vh_value &p_value) {
	double c[16];
	tuple_to_floats(p_value, c, 16);

	switch (p_tag) {
		case VH_VARIANT_VECTOR2:
			return Vector2((real_t)c[0], (real_t)c[1]);
		case VH_VARIANT_VECTOR2I:
			return Vector2i((int32_t)c[0], (int32_t)c[1]);
		case VH_VARIANT_VECTOR3:
			return Vector3((real_t)c[0], (real_t)c[1], (real_t)c[2]);
		case VH_VARIANT_VECTOR3I:
			return Vector3i((int32_t)c[0], (int32_t)c[1], (int32_t)c[2]);
		case VH_VARIANT_VECTOR4:
			return Vector4((real_t)c[0], (real_t)c[1], (real_t)c[2], (real_t)c[3]);
		case VH_VARIANT_VECTOR4I:
			return Vector4i((int32_t)c[0], (int32_t)c[1], (int32_t)c[2], (int32_t)c[3]);
		case VH_VARIANT_RECT2:
			return Rect2((real_t)c[0], (real_t)c[1], (real_t)c[2], (real_t)c[3]);
		case VH_VARIANT_RECT2I:
			return Rect2i((int32_t)c[0], (int32_t)c[1], (int32_t)c[2], (int32_t)c[3]);
		case VH_VARIANT_COLOR:
			return Color((float)c[0], (float)c[1], (float)c[2], (float)c[3]);
		case VH_VARIANT_QUATERNION:
			return Quaternion((real_t)c[0], (real_t)c[1], (real_t)c[2], (real_t)c[3]);
		case VH_VARIANT_PLANE:
			return Plane((real_t)c[0], (real_t)c[1], (real_t)c[2], (real_t)c[3]);
		case VH_VARIANT_AABB:
			return AABB(Vector3((real_t)c[0], (real_t)c[1], (real_t)c[2]),
					Vector3((real_t)c[3], (real_t)c[4], (real_t)c[5]));
		case VH_VARIANT_TRANSFORM2D:
			return Transform2D(Vector2((real_t)c[0], (real_t)c[1]),
					Vector2((real_t)c[2], (real_t)c[3]),
					Vector2((real_t)c[4], (real_t)c[5]));
		case VH_VARIANT_BASIS:
			return Basis(Vector3((real_t)c[0], (real_t)c[1], (real_t)c[2]),
					Vector3((real_t)c[3], (real_t)c[4], (real_t)c[5]),
					Vector3((real_t)c[6], (real_t)c[7], (real_t)c[8]));
		case VH_VARIANT_TRANSFORM3D:
			return Transform3D(Basis(Vector3((real_t)c[0], (real_t)c[1], (real_t)c[2]),
									 Vector3((real_t)c[3], (real_t)c[4], (real_t)c[5]),
									 Vector3((real_t)c[6], (real_t)c[7], (real_t)c[8])),
					Vector3((real_t)c[9], (real_t)c[10], (real_t)c[11]));
		case VH_VARIANT_PROJECTION:
			return Projection(Vector4((real_t)c[0], (real_t)c[1], (real_t)c[2], (real_t)c[3]),
					Vector4((real_t)c[4], (real_t)c[5], (real_t)c[6], (real_t)c[7]),
					Vector4((real_t)c[8], (real_t)c[9], (real_t)c[10], (real_t)c[11]),
					Vector4((real_t)c[12], (real_t)c[13], (real_t)c[14], (real_t)c[15]));
		default:
			return Variant();
	}
}

// Every scalar under a sequence, one level of nesting deep.
//
// A Verse `[]vector2` reaches here as three tuples rather than as six floats: the host marshals each
// element as the struct it is, and a struct is a tuple on the wire. The packed array of a math type
// wants the components, so this is what flattens them -- the same rule the host applies in the other
// direction (GodotBindings.cpp's VhRefFloats splices a tuple's components rather than appending it).
// A sequence of plain scalars passes through untouched, which is what the six scalar packed arrays
// send.
static void flatten_components(const vh_value &p_value, Vector<const vh_value *> &r_out) {
	if (p_value.Type != VH_TYPE_ARRAY && p_value.Type != VH_TYPE_TUPLE) {
		return;
	}
	for (int32_t i = 0; i < p_value.Seq.Count; i++) {
		const vh_value &item = p_value.Seq.Items[i];
		if (item.Type == VH_TYPE_ARRAY || item.Type == VH_TYPE_TUPLE) {
			for (int32_t inner = 0; inner < item.Seq.Count; inner++) {
				r_out.push_back(&item.Seq.Items[inner]);
			}
		} else {
			r_out.push_back(&item);
		}
	}
}

Variant packed_variant(int32_t p_tag, const vh_value &p_value) {
	const int32_t count = (p_value.Type == VH_TYPE_ARRAY || p_value.Type == VH_TYPE_TUPLE) ? p_value.Seq.Count : 0;

	switch (p_tag) {
		case VH_VARIANT_PACKED_BYTE_ARRAY: {
			PackedByteArray out;
			out.resize(count);
			for (int32_t i = 0; i < count; i++) {
				out[i] = (uint8_t)int_of(p_value.Seq.Items[i]);
			}
			return out;
		}
		case VH_VARIANT_PACKED_INT32_ARRAY: {
			PackedInt32Array out;
			out.resize(count);
			for (int32_t i = 0; i < count; i++) {
				out[i] = (int32_t)int_of(p_value.Seq.Items[i]);
			}
			return out;
		}
		case VH_VARIANT_PACKED_INT64_ARRAY: {
			PackedInt64Array out;
			out.resize(count);
			for (int32_t i = 0; i < count; i++) {
				out[i] = int_of(p_value.Seq.Items[i]);
			}
			return out;
		}
		case VH_VARIANT_PACKED_FLOAT32_ARRAY: {
			PackedFloat32Array out;
			out.resize(count);
			for (int32_t i = 0; i < count; i++) {
				out[i] = (float)double_of(p_value.Seq.Items[i]);
			}
			return out;
		}
		case VH_VARIANT_PACKED_FLOAT64_ARRAY: {
			PackedFloat64Array out;
			out.resize(count);
			for (int32_t i = 0; i < count; i++) {
				out[i] = double_of(p_value.Seq.Items[i]);
			}
			return out;
		}
		case VH_VARIANT_PACKED_STRING_ARRAY: {
			PackedStringArray out;
			out.resize(count);
			for (int32_t i = 0; i < count; i++) {
				out[i] = string_of(p_value.Seq.Items[i]);
			}
			return out;
		}
		case VH_VARIANT_PACKED_VECTOR2_ARRAY: {
			Vector<const vh_value *> c;
			flatten_components(p_value, c);
			PackedVector2Array out;
			out.resize(c.size() / 2);
			for (int32_t i = 0; i + 1 < c.size(); i += 2) {
				out[i / 2] = Vector2((real_t)double_of(*c[i]), (real_t)double_of(*c[i + 1]));
			}
			return out;
		}
		case VH_VARIANT_PACKED_VECTOR3_ARRAY: {
			Vector<const vh_value *> c;
			flatten_components(p_value, c);
			PackedVector3Array out;
			out.resize(c.size() / 3);
			for (int32_t i = 0; i + 2 < c.size(); i += 3) {
				out[i / 3] = Vector3((real_t)double_of(*c[i]), (real_t)double_of(*c[i + 1]),
						(real_t)double_of(*c[i + 2]));
			}
			return out;
		}
		case VH_VARIANT_PACKED_VECTOR4_ARRAY: {
			Vector<const vh_value *> c;
			flatten_components(p_value, c);
			PackedVector4Array out;
			out.resize(c.size() / 4);
			for (int32_t i = 0; i + 3 < c.size(); i += 4) {
				out[i / 4] = Vector4((real_t)double_of(*c[i]), (real_t)double_of(*c[i + 1]),
						(real_t)double_of(*c[i + 2]), (real_t)double_of(*c[i + 3]));
			}
			return out;
		}
		case VH_VARIANT_PACKED_COLOR_ARRAY: {
			Vector<const vh_value *> c;
			flatten_components(p_value, c);
			PackedColorArray out;
			out.resize(c.size() / 4);
			for (int32_t i = 0; i + 3 < c.size(); i += 4) {
				out[i / 4] = Color((float)double_of(*c[i]), (float)double_of(*c[i + 1]),
						(float)double_of(*c[i + 2]), (float)double_of(*c[i + 3]));
			}
			return out;
		}
		default:
			return Variant();
	}
}

} // namespace

bool variant_to_vh(const Variant &p_value, vh_arena *p_arena, vh_value &r_out) {
	r_out = vh_value{};
	r_out.VariantTag = (int32_t)p_value.get_type();

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

		// An object crosses as its instance id, never as a pointer: the id is the only
		// reference Verse can hold that stays checkable after Godot frees the object.
		case Variant::OBJECT: {
			Object *obj = p_value;
			r_out.Type = VH_TYPE_INT;
			r_out.Int = obj != nullptr ? (int64_t)obj->get_instance_id() : 0;
			return true;
		}

		case Variant::RID:
			r_out.Type = VH_TYPE_INT;
			r_out.Int = (int64_t)((RID)p_value).get_id();
			return true;

		case Variant::VECTOR2: {
			const Vector2 v = p_value;
			const double c[2] = { (double)v.x, (double)v.y };
			return floats_to_tuple(p_arena, c, 2, VH_VARIANT_VECTOR2, r_out);
		}
		case Variant::VECTOR2I: {
			const Vector2i v = p_value;
			const double c[2] = { (double)v.x, (double)v.y };
			return floats_to_tuple(p_arena, c, 2, VH_VARIANT_VECTOR2I, r_out);
		}
		case Variant::VECTOR3: {
			const Vector3 v = p_value;
			const double c[3] = { (double)v.x, (double)v.y, (double)v.z };
			return floats_to_tuple(p_arena, c, 3, VH_VARIANT_VECTOR3, r_out);
		}
		case Variant::VECTOR3I: {
			const Vector3i v = p_value;
			const double c[3] = { (double)v.x, (double)v.y, (double)v.z };
			return floats_to_tuple(p_arena, c, 3, VH_VARIANT_VECTOR3I, r_out);
		}
		case Variant::VECTOR4: {
			const Vector4 v = p_value;
			const double c[4] = { (double)v.x, (double)v.y, (double)v.z, (double)v.w };
			return floats_to_tuple(p_arena, c, 4, VH_VARIANT_VECTOR4, r_out);
		}
		case Variant::VECTOR4I: {
			const Vector4i v = p_value;
			const double c[4] = { (double)v.x, (double)v.y, (double)v.z, (double)v.w };
			return floats_to_tuple(p_arena, c, 4, VH_VARIANT_VECTOR4I, r_out);
		}
		case Variant::RECT2: {
			const Rect2 v = p_value;
			const double c[4] = { (double)v.position.x, (double)v.position.y, (double)v.size.x, (double)v.size.y };
			return floats_to_tuple(p_arena, c, 4, VH_VARIANT_RECT2, r_out);
		}
		case Variant::RECT2I: {
			const Rect2i v = p_value;
			const double c[4] = { (double)v.position.x, (double)v.position.y, (double)v.size.x, (double)v.size.y };
			return floats_to_tuple(p_arena, c, 4, VH_VARIANT_RECT2I, r_out);
		}
		case Variant::COLOR: {
			const Color v = p_value;
			const double c[4] = { (double)v.r, (double)v.g, (double)v.b, (double)v.a };
			return floats_to_tuple(p_arena, c, 4, VH_VARIANT_COLOR, r_out);
		}
		case Variant::QUATERNION: {
			const Quaternion v = p_value;
			const double c[4] = { (double)v.x, (double)v.y, (double)v.z, (double)v.w };
			return floats_to_tuple(p_arena, c, 4, VH_VARIANT_QUATERNION, r_out);
		}
		case Variant::PLANE: {
			const Plane v = p_value;
			const double c[4] = { (double)v.normal.x, (double)v.normal.y, (double)v.normal.z, (double)v.d };
			return floats_to_tuple(p_arena, c, 4, VH_VARIANT_PLANE, r_out);
		}
		case Variant::AABB: {
			const AABB v = p_value;
			const double c[6] = { (double)v.position.x, (double)v.position.y, (double)v.position.z,
				(double)v.size.x, (double)v.size.y, (double)v.size.z };
			return floats_to_tuple(p_arena, c, 6, VH_VARIANT_AABB, r_out);
		}
		case Variant::TRANSFORM2D: {
			const Transform2D v = p_value;
			const double c[6] = { (double)v.columns[0].x, (double)v.columns[0].y,
				(double)v.columns[1].x, (double)v.columns[1].y,
				(double)v.columns[2].x, (double)v.columns[2].y };
			return floats_to_tuple(p_arena, c, 6, VH_VARIANT_TRANSFORM2D, r_out);
		}
		case Variant::BASIS: {
			const Basis v = p_value;
			// Columns, not rows. Godot's Basis.x, .y and .z are columns 0, 1 and 2 (Basis.xml),
			// and the read side builds through Basis(Vector3, Vector3, Vector3), which is
			// set_columns. Emitting rows here transposed every basis that crossed.
			const Vector3 cx = v.get_column(0), cy = v.get_column(1), cz = v.get_column(2);
			const double c[9] = { (double)cx.x, (double)cx.y, (double)cx.z,
				(double)cy.x, (double)cy.y, (double)cy.z,
				(double)cz.x, (double)cz.y, (double)cz.z };
			return floats_to_tuple(p_arena, c, 9, VH_VARIANT_BASIS, r_out);
		}
		case Variant::TRANSFORM3D: {
			const Transform3D v = p_value;
			// Columns, for the reason BASIS above gives.
			const Vector3 cx = v.basis.get_column(0), cy = v.basis.get_column(1), cz = v.basis.get_column(2);
			const double c[12] = { (double)cx.x, (double)cx.y, (double)cx.z,
				(double)cy.x, (double)cy.y, (double)cy.z,
				(double)cz.x, (double)cz.y, (double)cz.z,
				(double)v.origin.x, (double)v.origin.y, (double)v.origin.z };
			return floats_to_tuple(p_arena, c, 12, VH_VARIANT_TRANSFORM3D, r_out);
		}
		case Variant::PROJECTION: {
			const Projection v = p_value;
			double c[16];
			for (int32_t col = 0; col < 4; col++) {
				c[col * 4 + 0] = (double)v.columns[col].x;
				c[col * 4 + 1] = (double)v.columns[col].y;
				c[col * 4 + 2] = (double)v.columns[col].z;
				c[col * 4 + 3] = (double)v.columns[col].w;
			}
			return floats_to_tuple(p_arena, c, 16, VH_VARIANT_PROJECTION, r_out);
		}

		// The reference types. Copying an Array or a Dictionary across would turn Godot's
		// reference semantics into value semantics without saying so, and a Callable cannot be
		// decomposed at all; the packed arrays are values but have no fixed width. All of them
		// cross as an id into the table, which the host releases when Verse drops the wrapper.
		case Variant::ARRAY:
		case Variant::DICTIONARY:
		case Variant::CALLABLE:
		case Variant::SIGNAL:
			r_out.Type = VH_TYPE_REF;
			r_out.VariantTag = (int32_t)p_value.get_type();
			r_out.Ref = verse_ref_table().mint(p_value);
			return true;

		case Variant::PACKED_BYTE_ARRAY:
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY:
		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
		case Variant::PACKED_STRING_ARRAY:
		case Variant::PACKED_VECTOR2_ARRAY:
		case Variant::PACKED_VECTOR3_ARRAY:
		case Variant::PACKED_COLOR_ARRAY:
		case Variant::PACKED_VECTOR4_ARRAY:
			r_out.Type = VH_TYPE_REF;
			r_out.VariantTag = (int32_t)p_value.get_type();
			r_out.Ref = verse_ref_table().mint(p_value);
			return true;

		case Variant::VARIANT_MAX:
			return false;

		default:
			return false;
	}
}

Variant vh_to_variant(const vh_value &p_value) {
	// How the value is carried decides before what Godot type it is. A reference id is the whole
	// value, and the table already holds it as the right type -- where reading the tag first sent a
	// REF-carried PackedByteArray into packed_variant, which found no sequence to read and answered
	// with an empty one. Silently: the only symptom was a Godot method receiving nothing.
	if (p_value.Type == VH_TYPE_REF) {
		const Variant *found = verse_ref_table().find(p_value.Ref);
		return found != nullptr ? *found : Variant();
	}

	switch (p_value.VariantTag) {
		case VH_VARIANT_NIL:
			break; // fall through to the vh_type-driven path below

		case VH_VARIANT_STRING_NAME:
			return StringName(string_of(p_value));
		case VH_VARIANT_NODE_PATH:
			return NodePath(string_of(p_value));
		case VH_VARIANT_OBJECT:
			return UtilityFunctions::instance_from_id(int_of(p_value));
		case VH_VARIANT_RID:
			return Variant();

		case VH_VARIANT_VECTOR2:
		case VH_VARIANT_VECTOR2I:
		case VH_VARIANT_VECTOR3:
		case VH_VARIANT_VECTOR3I:
		case VH_VARIANT_VECTOR4:
		case VH_VARIANT_VECTOR4I:
		case VH_VARIANT_RECT2:
		case VH_VARIANT_RECT2I:
		case VH_VARIANT_COLOR:
		case VH_VARIANT_QUATERNION:
		case VH_VARIANT_PLANE:
		case VH_VARIANT_AABB:
		case VH_VARIANT_TRANSFORM2D:
		case VH_VARIANT_BASIS:
		case VH_VARIANT_TRANSFORM3D:
		case VH_VARIANT_PROJECTION:
			return math_variant(p_value.VariantTag, p_value);

		case VH_VARIANT_PACKED_BYTE_ARRAY:
		case VH_VARIANT_PACKED_INT32_ARRAY:
		case VH_VARIANT_PACKED_INT64_ARRAY:
		case VH_VARIANT_PACKED_FLOAT32_ARRAY:
		case VH_VARIANT_PACKED_FLOAT64_ARRAY:
		case VH_VARIANT_PACKED_STRING_ARRAY:
		case VH_VARIANT_PACKED_VECTOR2_ARRAY:
		case VH_VARIANT_PACKED_VECTOR3_ARRAY:
		case VH_VARIANT_PACKED_COLOR_ARRAY:
		case VH_VARIANT_PACKED_VECTOR4_ARRAY:
			return packed_variant(p_value.VariantTag, p_value);

		default:
			break;
	}

	switch (p_value.Type) {
		case VH_TYPE_VOID:
			return Variant();

		// The table owns the value; handing back a copy of the Variant is handing back the same
		// Array or Dictionary, because those are references in Godot too.
		case VH_TYPE_REF: {
			const Variant *found = verse_ref_table().find(p_value.Ref);
			return found != nullptr ? *found : Variant();
		}

		case VH_TYPE_LOGIC:
			return Variant(p_value.Logic != 0);

		case VH_TYPE_INT:
			return Variant(p_value.Int);

		case VH_TYPE_FLOAT:
			return Variant(p_value.Float);

		case VH_TYPE_STRING:
			return Variant(string_of(p_value));

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
			// An untagged two-float tuple is the Phase 2 Vector2 encoding, still used by
			// SetVector2 and the property helpers.
			if (p_value.Seq.Count == 2 && p_value.Seq.Items != nullptr &&
					p_value.Seq.Items[0].Type == VH_TYPE_FLOAT && p_value.Seq.Items[1].Type == VH_TYPE_FLOAT) {
				return Variant(Vector2((real_t)p_value.Seq.Items[0].Float, (real_t)p_value.Seq.Items[1].Float));
			}
			return Variant();

		default:
			return Variant();
	}
}

Variant::Type variant_type_for(int32_t p_type, int32_t p_variant_tag) {
	if (p_variant_tag > VH_VARIANT_NIL && p_variant_tag < VH_VARIANT_MAX) {
		return (Variant::Type)p_variant_tag;
	}
	switch (p_type) {
		case VH_TYPE_LOGIC:
			return Variant::BOOL;
		case VH_TYPE_INT:
		case VH_TYPE_CHAR:
			return Variant::INT;
		case VH_TYPE_FLOAT:
			return Variant::FLOAT;
		case VH_TYPE_STRING:
			return Variant::STRING;
		case VH_TYPE_MAP:
			return Variant::DICTIONARY;
		// A tuple is an Array to Godot unless the tag named a math struct, which the tag test above
		// has already answered for.
		case VH_TYPE_TUPLE:
			return Variant::ARRAY;
		// An untagged array is an `Array` to Godot. A Verse `[]float` is equally a
		// PackedFloat32Array and a PackedFloat64Array, and Godot converts between none of the
		// three, so a script method taking one receives the general type -- which GDScript can
		// build from any of them, where the reverse is not true.
		case VH_TYPE_ARRAY:
			return Variant::ARRAY;
		default:
			return Variant::NIL;
	}
}

bool array_to_vh_seq(const Array &p_values, vh_arena *p_arena, vh_value &r_out) {
	return scalars_to_vh(p_arena, p_values, VH_VARIANT_ARRAY, r_out);
}
