#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Sequential reader over an in-memory .vbc buffer, decoding format.md §1's primitives: u8, uv
// (unsigned LEB128, at most 10 bytes), sv (zigzag then uv), f64, str and ref. A read that would
// run past the end of the buffer, or a uv that never terminates within 10 bytes, leaves a
// sentence-style error naming the offset and stops advancing -- format.md §9 requires every
// refusal to read that way rather than as a crash.
//
// Once ok() is false every further read answers a defined placeholder (0, 0.0, "") without
// touching the cursor again, so a caller may decode a whole procedure and check once at the end
// instead of after every field.
struct VbcReader {
	const uint8_t *data = nullptr;
	size_t size = 0;
	size_t offset = 0;
	std::string error;

	bool ok() const { return error.empty(); }
};

VbcReader vbc_reader_make(const uint8_t *p_data, size_t p_size);

uint8_t vbc_read_u8(VbcReader &r_reader);
uint64_t vbc_read_uv(VbcReader &r_reader);
int64_t vbc_read_sv(VbcReader &r_reader);
double vbc_read_f64(VbcReader &r_reader);
std::string vbc_read_str(VbcReader &r_reader);

// format.md §1: a uv cell index plus one, with 0 meaning "none" where the field allows it. Its own
// function because a future caller may want to validate the index against the cell table here;
// today it is exactly vbc_read_uv.
uint64_t vbc_read_ref(VbcReader &r_reader);
