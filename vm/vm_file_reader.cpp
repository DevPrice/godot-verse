#include "vm_file_reader.h"

#include <cstdio>

namespace {

VmFileReaderFn g_file_reader = &vm_default_file_reader;

} // namespace

void vm_set_file_reader(VmFileReaderFn p_reader) {
	g_file_reader = p_reader != nullptr ? p_reader : &vm_default_file_reader;
}

VmFileReaderFn vm_get_file_reader() {
	return g_file_reader;
}

bool vm_default_file_reader(const char *p_path, std::vector<uint8_t> &r_out) {
	FILE *file = std::fopen(p_path, "rb");
	if (file == nullptr) {
		return false;
	}
	if (std::fseek(file, 0, SEEK_END) != 0) {
		std::fclose(file);
		return false;
	}
	const long length = std::ftell(file);
	if (length < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
		std::fclose(file);
		return false;
	}
	std::vector<uint8_t> bytes(static_cast<size_t>(length));
	const size_t read = bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), file);
	std::fclose(file);
	if (read != bytes.size()) {
		return false;
	}
	r_out = std::move(bytes);
	return true;
}
