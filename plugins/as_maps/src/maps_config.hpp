// as_maps — where maps.ini is read from and how its entries are ordered.
//
// The original module hard-coded the Metamod path "addons/configs/admin_system/maps.ini" and read
// it through the game's file system. pisexcompat resolved that spelling with
// libs/pisexcompat/src/paths.cpp Candidates()/ResolvePath(): the path *as given* was tried first
// (relative to the server's working directory, the Metamod installation), then the rest of the
// Metamod spelling mapped into the Plugify directories. The port keeps exactly that order:
//
//   1. "addons/configs/admin_system/maps.ini"                    raw path, relative to the cwd
//   2. <GetConfigsDir()>/admin_system/maps.ini                   shared admin config directory
//   3. <GetConfigsDir()>/admin_system/translations/maps.ini      the translations candidate of
//                                                                pisexcompat's Candidates()
//   4. <GetDataDir()>/admin_system/maps.ini                      the data candidate
//
// ResolvePath() returned the *first existing* file and KeyValues::LoadFromFile() then parsed only
// that one: a file that exists but is malformed is authoritative as well - it makes the module log
// the load failure and run with an empty map list, it is never silently skipped in favour of a
// later candidate (the raw copy cannot be hidden). LoadMaps() keeps that: the walk stops at the
// first candidate that opens, and its parse result is the result.
//
// The file is a KV1 "Maps" { "Display name" "map_file" } block. The original stored
// `g_mapMaps[value] = key`, i.e. keyed by the map *file* name in a std::map, so the menu showed
// the maps in the alphabetical order of the map file name, not in file order, and a duplicate map
// file replaced the earlier display name. ParseMaps() and Maps keep that (std::map) ordering:
// the order is part of the visible behaviour and of the selection indices.
//
// Header-only and engine-free: src/plugin.cpp and tests/as_maps_config_tests.cpp share it.

#pragma once

#include "kv1.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace as_maps {

// Exactly what the Pisex module / pisexcompat used.
inline constexpr std::string_view kMetamodMapsPath = "addons/configs/admin_system/maps.ini";
// The shared admin-module config directory; "translations" and the data directory are the other
// two candidates pisexcompat appended (libs/pisexcompat/src/paths.cpp Candidates()).
inline constexpr std::string_view kAdminSystemDir = "admin_system";
inline constexpr std::string_view kTranslationsDir = "translations";
inline constexpr std::string_view kMapsFileName = "maps.ini";

// map file name -> display name; the std::map ordering is the menu ordering (see the header).
using Maps = std::map<std::string, std::string>;

inline std::string JoinPath(std::string_view base, std::string_view name) {
	if (base.empty()) {
		return std::string(name);
	}
	if (name.empty()) {
		return std::string(base);
	}
	std::string out(base);
	if (out.back() != '/') {
		out.push_back('/');
	}
	if (name.front() == '/') {
		name.remove_prefix(1);
	}
	out.append(name);
	return out;
}

// The lookup order of the header comment. The raw Metamod spelling always comes first, even when
// the host reports no directory (the plugin then runs with that one candidate).
inline std::vector<std::string> MapsFileCandidates(std::string_view configsDir, std::string_view dataDir) {
	std::vector<std::string> candidates;
	candidates.emplace_back(kMetamodMapsPath);
	const std::string adminSystemDir = JoinPath(configsDir, kAdminSystemDir);
	if (!configsDir.empty()) {
		candidates.push_back(JoinPath(adminSystemDir, kMapsFileName));
		candidates.push_back(JoinPath(JoinPath(adminSystemDir, kTranslationsDir), kMapsFileName));
	}
	if (!dataDir.empty()) {
		candidates.push_back(JoinPath(JoinPath(dataDir, kAdminSystemDir), kMapsFileName));
	}
	return candidates;
}

// Parses the file's text. std::nullopt when the KV1 is malformed (a half-read list would silently
// change the map menu). A missing value, a pure block and an empty map name are skipped: the
// previous port ignored an empty map file name as well (`if (szMapName && *szMapName)`).
inline std::optional<Maps> ParseMaps(std::string_view text) {
	const std::optional<KvNode> root = ParseKv1(text);
	if (!root) {
		return std::nullopt;
	}
	Maps maps;
	for (const KvNode& entry : root->children) {
		if (!entry.hasValue || entry.value.empty()) {
			continue;
		}
		maps[entry.value] = entry.name;
	}
	return maps;
}

// The result of the candidate walk. found == false: none of the candidates exists on disk;
// ok == false with found == true: the first existing file is not valid KV1 (path names it).
struct LoadResult {
	bool found = false;
	bool ok = false;
	std::string path;
	Maps maps;
};

// ResolvePath() semantics: the first candidate that opens is the authoritative file. A malformed
// first-existing file is returned as !ok (the caller logs and keeps an empty list), never skipped
// for a later candidate.
inline LoadResult LoadMaps(const std::vector<std::string>& candidates) {
	LoadResult result;
	for (const std::string& candidate : candidates) {
		const std::optional<std::string> text = ReadTextFile(candidate);
		if (!text) {
			continue; // not installed: try the next spelling
		}
		result.found = true;
		result.path = candidate;
		std::optional<Maps> parsed = ParseMaps(*text);
		if (!parsed) {
			return result; // first existing file is authoritative; ok stays false
		}
		result.ok = true;
		result.maps = std::move(*parsed);
		return result;
	}
	return result;
}

} // namespace as_maps
