#pragma once

#include <cstdint>
#include <vector>

// How vm/ reads a file out of the cooked directory, settable by the embedder before vh_init
// (design §8). vm/ cannot read a `.pck` itself -- that is the one thing it needs from whichever
// consumer links it in -- so the DLL build and the wasm-into-GDExtension build differ only in
// which reader is installed: the DLL leaves the default (the C library) in place, and the
// GDExtension calls vm_set_file_reader with one over Godot's FileAccess before it calls vh_init,
// so verse_data reads the same from a directory beside an executable and from inside a `.pck`.
//
// A reader answers true and fills r_out on success, and answers false and leaves r_out untouched
// on failure. There is no error text here: the caller -- the .vbc and sidecar loaders -- is what
// knows which file this was and can say why it mattered.
//
// This is not one of the vh_* entry points: it is set once, before vh_init, and vh_init itself
// takes no reader parameter, so it has to be reachable some other way -- a small free function is
// the whole of what's needed, and it is the one piece of mutable global state vm/ carries beyond
// the runtime object the ABI hands back, because nothing else can hold it before that object
// exists.
typedef bool (*VmFileReaderFn)(const char *p_path, std::vector<uint8_t> &r_out);

void vm_set_file_reader(VmFileReaderFn p_reader);
VmFileReaderFn vm_get_file_reader();

// Reads the whole file through the C library (fopen/fread/fclose). This is what vm_get_file_reader
// answers until something calls vm_set_file_reader, and it is what the DLL build and the wasm
// object files both use as-is.
bool vm_default_file_reader(const char *p_path, std::vector<uint8_t> &r_out);
