// Standalone driver for the .vbc primitive reader (format.md §1). No Godot and no godot-cpp, like
// the lexer and module-map tests beside it.
#include "vbc_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool Step(const char *Name, bool Result) {
	printf("[verse_vm_test] %s: %s\n", Name, Result ? "ok" : "FAIL");
	return Result;
}

// Test-only LEB128/zigzag encoders, so a round trip exercises the reader against an independent
// implementation rather than only against itself.
void AppendUv(std::vector<uint8_t> &Bytes, uint64_t Value) {
	do {
		uint8_t Byte = Value & 0x7F;
		Value >>= 7;
		if (Value != 0) {
			Byte |= 0x80;
		}
		Bytes.push_back(Byte);
	} while (Value != 0);
}

void AppendSv(std::vector<uint8_t> &Bytes, int64_t Value) {
	const uint64_t Zigzag = (static_cast<uint64_t>(Value) << 1) ^ static_cast<uint64_t>(Value >> 63);
	AppendUv(Bytes, Zigzag);
}

bool TestUvRoundTrip() {
	const uint64_t Values[] = { 0, 1, 127, 128, 300, 16384, 0xFFFFFFFFULL, UINT64_MAX };
	for (uint64_t Value : Values) {
		std::vector<uint8_t> Bytes;
		AppendUv(Bytes, Value);
		VbcReader Reader = vbc_reader_make(Bytes.data(), Bytes.size());
		const uint64_t Decoded = vbc_read_uv(Reader);
		if (!Reader.ok() || Decoded != Value || Reader.offset != Bytes.size()) {
			return false;
		}
	}
	return true;
}

bool TestUvKnownEncoding() {
	// 300 is the textbook LEB128 example: 0xAC 0x02.
	const uint8_t Bytes[] = { 0xAC, 0x02 };
	VbcReader Reader = vbc_reader_make(Bytes, sizeof(Bytes));
	const uint64_t Decoded = vbc_read_uv(Reader);
	return Reader.ok() && Decoded == 300 && Reader.offset == sizeof(Bytes);
}

bool TestSvRoundTrip() {
	const int64_t Values[] = { 0, -1, 1, -2, 2, INT32_MIN, INT32_MAX, INT64_MIN, INT64_MAX };
	for (int64_t Value : Values) {
		std::vector<uint8_t> Bytes;
		AppendSv(Bytes, Value);
		VbcReader Reader = vbc_reader_make(Bytes.data(), Bytes.size());
		const int64_t Decoded = vbc_read_sv(Reader);
		if (!Reader.ok() || Decoded != Value) {
			return false;
		}
	}
	return true;
}

bool TestSvKnownEncoding() {
	// Zigzag by hand: 0->0, -1->1, 1->2, -2->3, 2->4.
	struct Case {
		uint8_t byte;
		int64_t expected;
	};
	const Case Cases[] = { { 0x00, 0 }, { 0x01, -1 }, { 0x02, 1 }, { 0x03, -2 }, { 0x04, 2 } };
	for (const Case &C : Cases) {
		VbcReader Reader = vbc_reader_make(&C.byte, 1);
		const int64_t Decoded = vbc_read_sv(Reader);
		if (!Reader.ok() || Decoded != C.expected) {
			return false;
		}
	}
	return true;
}

bool TestUvDanglingContinuationRefused() {
	const uint8_t Bytes[] = { 0x80 }; // continuation bit set, nothing follows
	VbcReader Reader = vbc_reader_make(Bytes, sizeof(Bytes));
	vbc_read_uv(Reader);
	return !Reader.ok() && !Reader.error.empty();
}

bool TestUvNeverTerminatesRefused() {
	// 10 bytes, every continuation bit set: the most format.md §1 allows, and none of them ends it.
	std::vector<uint8_t> Bytes(10, 0x80);
	VbcReader Reader = vbc_reader_make(Bytes.data(), Bytes.size());
	vbc_read_uv(Reader);
	return !Reader.ok();
}

bool TestF64RoundTrip() {
	const double Value = 1.5;
	std::vector<uint8_t> Bytes(sizeof(Value));
	std::memcpy(Bytes.data(), &Value, sizeof(Value));
	VbcReader Reader = vbc_reader_make(Bytes.data(), Bytes.size());
	const double Decoded = vbc_read_f64(Reader);
	return Reader.ok() && Decoded == Value;
}

bool TestF64TruncationRefused() {
	const uint8_t Bytes[4] = {};
	VbcReader Reader = vbc_reader_make(Bytes, sizeof(Bytes));
	vbc_read_f64(Reader);
	return !Reader.ok();
}

bool TestStrRoundTrip() {
	const std::string Text = "verse";
	std::vector<uint8_t> Bytes;
	AppendUv(Bytes, Text.size());
	Bytes.insert(Bytes.end(), Text.begin(), Text.end());
	VbcReader Reader = vbc_reader_make(Bytes.data(), Bytes.size());
	const std::string Decoded = vbc_read_str(Reader);
	return Reader.ok() && Decoded == Text && Reader.offset == Bytes.size();
}

bool TestStrTruncationRefused() {
	std::vector<uint8_t> Bytes;
	AppendUv(Bytes, 10); // claims 10 bytes of body, supplies none
	VbcReader Reader = vbc_reader_make(Bytes.data(), Bytes.size());
	vbc_read_str(Reader);
	return !Reader.ok();
}

bool TestRefIsUv() {
	std::vector<uint8_t> Bytes;
	AppendUv(Bytes, 42);
	VbcReader Reader = vbc_reader_make(Bytes.data(), Bytes.size());
	return vbc_read_ref(Reader) == 42 && Reader.ok();
}

bool TestFailureIsSticky() {
	// Once truncated, later reads answer a defined placeholder and stop moving the cursor.
	const uint8_t Bytes[] = { 0x80 };
	VbcReader Reader = vbc_reader_make(Bytes, sizeof(Bytes));
	vbc_read_uv(Reader);
	const size_t OffsetAfterFailure = Reader.offset;
	const uint64_t Second = vbc_read_uv(Reader);
	return !Reader.ok() && Second == 0 && Reader.offset == OffsetAfterFailure;
}

} // namespace

int main() {
	bool AllOk = true;
	AllOk &= Step("uv round trips", TestUvRoundTrip());
	AllOk &= Step("uv matches the textbook 300 encoding", TestUvKnownEncoding());
	AllOk &= Step("sv round trips", TestSvRoundTrip());
	AllOk &= Step("sv matches zigzag by hand", TestSvKnownEncoding());
	AllOk &= Step("a uv with a dangling continuation bit is refused", TestUvDanglingContinuationRefused());
	AllOk &= Step("a uv that never terminates is refused", TestUvNeverTerminatesRefused());
	AllOk &= Step("f64 round trips", TestF64RoundTrip());
	AllOk &= Step("a truncated f64 is refused", TestF64TruncationRefused());
	AllOk &= Step("str round trips", TestStrRoundTrip());
	AllOk &= Step("a str whose body is missing is refused", TestStrTruncationRefused());
	AllOk &= Step("ref decodes as a uv", TestRefIsUv());
	AllOk &= Step("a reader that has failed stays failed", TestFailureIsSticky());
	return AllOk ? 0 : 1;
}
