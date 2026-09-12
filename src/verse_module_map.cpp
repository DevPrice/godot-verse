#include "verse_module_map.h"

#include <algorithm>

namespace {

std::string strip_res(const std::string &p_path) {
	static const std::string prefix = "res://";
	return p_path.rfind(prefix, 0) == 0 ? p_path.substr(prefix.size()) : p_path;
}

// Everything before the last '/', or "" for a file at the root.
std::string directory_of(const std::string &p_path) {
	const size_t slash = p_path.rfind('/');
	return slash == std::string::npos ? std::string() : p_path.substr(0, slash);
}

std::string file_of(const std::string &p_path) {
	const size_t slash = p_path.rfind('/');
	return slash == std::string::npos ? p_path : p_path.substr(slash + 1);
}

std::string stem_of(const std::string &p_file) {
	const size_t dot = p_file.rfind('.');
	return dot == std::string::npos ? p_file : p_file.substr(0, dot);
}

// The directory itself, then each ancestor, then "". Walking outward is the whole rule: a file
// belongs to the nearest marked ancestor.
std::vector<std::string> self_and_ancestors(const std::string &p_dir) {
	std::vector<std::string> walk;
	std::string current = p_dir;
	while (!current.empty()) {
		walk.push_back(current);
		current = directory_of(current);
	}
	walk.push_back(std::string());
	return walk;
}

} // namespace

bool verse_is_valid_module_name(const std::string &p_name) {
	if (p_name.empty()) {
		return false;
	}
	const char first = p_name[0];
	if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) {
		return false;
	}
	return std::all_of(p_name.begin() + 1, p_name.end(), [](char c) {
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
	});
}

VerseModuleMap verse_build_module_map(const std::vector<std::string> &p_source_paths,
		const std::vector<std::string> &p_marker_paths) {
	VerseModuleMap map;

	// Module name for each marked directory. A directory carrying more than one marker is the
	// author's mistake and takes the first by sorted order, so the answer does not depend on the
	// order the filesystem happened to hand them over in.
	std::vector<std::string> markers;
	markers.reserve(p_marker_paths.size());
	for (const std::string &marker : p_marker_paths) {
		markers.push_back(strip_res(marker));
	}
	std::sort(markers.begin(), markers.end());

	std::map<std::string, std::string> name_by_directory;
	for (const std::string &marker : markers) {
		const std::string directory = directory_of(marker);
		const std::string name = stem_of(file_of(marker));
		if (!verse_is_valid_module_name(name)) {
			map.diagnostics.push_back({ marker,
					std::string("\"") + name
							+ "\" is not a Verse module name, so this directory is not a module and its "
							  "scripts stay in the one above it. A module name is a letter or underscore "
							  "followed by letters, digits or underscores -- gameplay.vmodule, not "
							  "my-stuff.vmodule." });
			continue;
		}
		name_by_directory.emplace(directory, name);
	}

	// A module's own path is its name prefixed by the module its marked directory sits inside,
	// which is found by the same outward walk a source file uses.
	std::map<std::string, std::string> module_by_directory;
	for (const auto &entry : name_by_directory) {
		std::string parent;
		const std::vector<std::string> walk = self_and_ancestors(directory_of(entry.first));
		for (const std::string &ancestor : walk) {
			const auto found = name_by_directory.find(ancestor);
			if (found != name_by_directory.end()) {
				// Already resolved: an ancestor's path is a strict prefix of this one, so it
				// sorted earlier in the map being iterated.
				parent = module_by_directory[ancestor];
				break;
			}
		}
		module_by_directory[entry.first] = parent.empty() ? entry.second : parent + "/" + entry.second;
	}

	for (const auto &entry : module_by_directory) {
		map.directories_by_module[entry.second].push_back(entry.first);
	}

	for (const std::string &source : p_source_paths) {
		const std::string relative = strip_res(source);
		std::string module;
		for (const std::string &ancestor : self_and_ancestors(directory_of(relative))) {
			const auto found = module_by_directory.find(ancestor);
			if (found != module_by_directory.end()) {
				module = found->second;
				break;
			}
		}
		map.module_by_source[source] = module;
	}

	return map;
}
