#include "vbc_reader.h"

#include <cstdint>
#include <cstring>

namespace {

void fail(VbcReader &r_reader, const std::string &p_message) {
	if (r_reader.error.empty()) {
		r_reader.error = p_message;
	}
}

// True and leaves the cursor where a read of p_count bytes may start; false and records a
// truncation, leaving the cursor untouched, when that many bytes are not left in the buffer.
bool require(VbcReader &r_reader, size_t p_count) {
	if (!r_reader.ok()) {
		return false;
	}
	if (r_reader.offset + p_count < r_reader.offset || r_reader.offset + p_count > r_reader.size) {
		fail(r_reader,
				"truncated .vbc: need " + std::to_string(p_count) + " more byte(s) at offset "
						+ std::to_string(r_reader.offset) + " of " + std::to_string(r_reader.size));
		return false;
	}
	return true;
}

} // namespace

VbcReader vbc_reader_make(const uint8_t *p_data, size_t p_size) {
	VbcReader reader;
	reader.data = p_data;
	reader.size = p_size;
	return reader;
}

uint8_t vbc_read_u8(VbcReader &r_reader) {
	if (!require(r_reader, 1)) {
		return 0;
	}
	return r_reader.data[r_reader.offset++];
}

uint64_t vbc_read_uv(VbcReader &r_reader) {
	if (!r_reader.ok()) {
		return 0;
	}
	uint64_t value = 0;
	for (int shift = 0; shift < 70; shift += 7) {
		const uint8_t byte = vbc_read_u8(r_reader);
		if (!r_reader.ok()) {
			return 0;
		}
		value |= static_cast<uint64_t>(byte & 0x7F) << shift;
		if ((byte & 0x80) == 0) {
			return value;
		}
	}
	fail(r_reader, "malformed .vbc: uv does not terminate within 10 bytes, ending at offset "
			+ std::to_string(r_reader.offset));
	return 0;
}

int64_t vbc_read_sv(VbcReader &r_reader) {
	const uint64_t zigzag = vbc_read_uv(r_reader);
	if (!r_reader.ok()) {
		return 0;
	}
	return static_cast<int64_t>(zigzag >> 1) ^ -static_cast<int64_t>(zigzag & 1);
}

double vbc_read_f64(VbcReader &r_reader) {
	if (!require(r_reader, sizeof(double))) {
		return 0.0;
	}
	// A plain memcpy is correct without a byte swap because both build targets (x86-64, wasm32)
	// are little-endian, matching format.md §1's encoding; a big-endian host would need one here.
	double value = 0.0;
	std::memcpy(&value, r_reader.data + r_reader.offset, sizeof(value));
	r_reader.offset += sizeof(value);
	return value;
}

std::string vbc_read_str(VbcReader &r_reader) {
	const uint64_t length = vbc_read_uv(r_reader);
	if (!r_reader.ok()) {
		return std::string();
	}
	// wasm32's size_t is 32 bits; a length a 64-bit uv can state but this platform cannot index is
	// a truncation of a different kind and must be refused the same way.
	if (length > static_cast<uint64_t>(SIZE_MAX)) {
		fail(r_reader, "truncated .vbc: string length " + std::to_string(length)
				+ " does not fit this platform's size_t at offset " + std::to_string(r_reader.offset));
		return std::string();
	}
	const size_t length_sz = static_cast<size_t>(length);
	if (!require(r_reader, length_sz)) {
		return std::string();
	}
	std::string value(reinterpret_cast<const char *>(r_reader.data + r_reader.offset), length_sz);
	r_reader.offset += length_sz;
	return value;
}

uint64_t vbc_read_ref(VbcReader &r_reader) {
	return vbc_read_uv(r_reader);
}
