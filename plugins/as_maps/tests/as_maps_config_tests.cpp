// as_maps config tests — engine-free: the KV1 reader, the maps.ini lookup order and the menu
// ordering, all in the plugin-local headers (src/kv1.hpp, src/maps_config.hpp, src/chat_text.hpp).
//
// Covered:
//   * PITFALLS / PLUGIN_STANDARD §7: the file is read with the Pisex layout kept, so the production
//     maps.ini needs no conversion;
//   * the candidate order is the pisexcompat ResolvePath() one: the raw Metamod spelling relative
//     to the working directory first, then <configs>/admin_system/maps.ini,
//     <configs>/admin_system/translations/maps.ini and <data>/admin_system/maps.ini;
//   * "Display name" "map_file" becomes g_mapMaps[map_file] = display name (the original's
//     `g_mapMaps[value] = key`), i.e. a std::map keyed by the map file name: the menu order is the
//     alphabetical order of the map file, duplicates are last-wins, an empty map file is skipped;
//   * the first candidate that exists is authoritative: a malformed first-existing file is
//     rejected as a whole (never a half-read list) and is *not* hidden by a later valid file;
//   * the cs2-menus colour table of the phrase files (chat bytes, server-console stripping).
//
// Run by ./test.sh (build/tests/as_maps_config_tests).

#include <chat_text.hpp>
#include <maps_config.hpp>

#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

std::string DataFile(const char* name) {
	return std::string(AS_MAPS_TEST_DATA_DIR) + "/" + name;
}

std::string EmptyFile(const char* name) {
	return std::string(AS_MAPS_TEST_DATA_DIR) + "/does-not-exist-" + name;
}

// --- the lookup order ----------------------------------------------------------------------------

void TestCandidatesRawPathFirst() {
	const std::vector<std::string> candidates =
			as_maps::MapsFileCandidates("/srv/plugify/configs", "/srv/plugify/data");
	Check(candidates.size() == 4, "four candidates with configs and data directories");
	Check(candidates[0] == "addons/configs/admin_system/maps.ini",
		  "candidate 0 is the raw Metamod path (relative to the working directory)");
	Check(candidates[1] == "/srv/plugify/configs/admin_system/maps.ini",
		  "candidate 1 is <configs>/admin_system/maps.ini");
	Check(candidates[2] == "/srv/plugify/configs/admin_system/translations/maps.ini",
		  "candidate 2 is the pisexcompat translations candidate");
	Check(candidates[3] == "/srv/plugify/data/admin_system/maps.ini",
		  "candidate 3 is the pisexcompat data candidate");
}

void TestCandidatesWithoutDirectories() {
	const std::vector<std::string> candidates = as_maps::MapsFileCandidates("", "");
	Check(candidates.size() == 1, "the raw path is always tried, even without host directories");
	Check(candidates[0] == "addons/configs/admin_system/maps.ini", "the raw path is candidate 0");
}

void TestCandidatesPartialDirectories() {
	const std::vector<std::string> configsOnly = as_maps::MapsFileCandidates("/srv/configs", "");
	Check(configsOnly.size() == 3, "raw + two configs candidates without a data dir");
	Check(configsOnly[2] == "/srv/configs/admin_system/translations/maps.ini", "translations candidate");

	const std::vector<std::string> dataOnly = as_maps::MapsFileCandidates("", "/srv/data");
	Check(dataOnly.size() == 2, "raw + data candidate without a configs dir");
	Check(dataOnly[1] == "/srv/data/admin_system/maps.ini", "data candidate");
}

void TestCandidatesTrailingSlash() {
	const std::vector<std::string> candidates =
			as_maps::MapsFileCandidates("/srv/configs/", "/srv/data/");
	Check(candidates.size() == 4, "four candidates with a trailing slash");
	Check(candidates[1] == "/srv/configs/admin_system/maps.ini", "no double slash in candidate 1");
	Check(candidates[3] == "/srv/data/admin_system/maps.ini", "no double slash in candidate 3");
}

// --- parsing -------------------------------------------------------------------------------------

void TestProductionFile() {
	const as_maps::LoadResult result = as_maps::LoadMaps({DataFile("admin_system/maps.ini")});
	Check(result.found && result.ok, "the production-shaped maps.ini loads");
	Check(result.path == DataFile("admin_system/maps.ini"), "the loaded path is reported");
	Check(result.maps.size() == 3, "three maps");
	// std::map keyed by the map file name: awp_lego_2 < de_dust2 < de_mirage.
	const std::vector<std::pair<std::string, std::string>> expected = {
			{"awp_lego_2", "AWP LEGO 2"}, {"de_dust2", "Dust2"}, {"de_mirage", "Mirage"}};
	std::vector<std::pair<std::string, std::string>> actual(result.maps.begin(), result.maps.end());
	Check(actual == expected, "the menu order is the alphabetical order of the map file name");
}

void TestCommentsUnquotedAndEscapes() {
	const as_maps::LoadResult result = as_maps::LoadMaps({DataFile("maps_comments.ini")});
	Check(result.found && result.ok, "comments, unquoted tokens and escapes parse");
	Check(result.maps.size() == 3, "three entries");
	Check(result.maps.count("de_mirage") == 1 && result.maps.at("de_mirage") == "Mirage",
		  "a quoted pair after a block comment");
	Check(result.maps.count("de_dust2") == 1 && result.maps.at("de_dust2") == "Dust2",
		  "an unquoted pair");
	Check(result.maps.count("de_quoted") == 1 && result.maps.at("de_quoted") == "Quoted \"name\"",
		  "escaped quotes inside a quoted token");
}

void TestBomAndRawRoot() {
	// A UTF-8 BOM and no "Maps" wrapper: Valve's loader accepts both.
	const std::string text = "\xEF\xBB\xBF\"Mirage\" \"de_mirage\"\n\"Dust2\" \"de_dust2\"\n";
	const std::optional<as_maps::Maps> maps = as_maps::ParseMaps(text);
	Check(maps.has_value(), "a BOM and a root without a block are accepted");
	if (maps) {
		Check(maps->size() == 2 && maps->at("de_mirage") == "Mirage", "the BOM is not part of the key");
	}
}

void TestDuplicatesLastWinsAndEmptyMapSkipped() {
	const as_maps::LoadResult result = as_maps::LoadMaps({DataFile("maps_duplicates.ini")});
	Check(result.found && result.ok, "the duplicate file parses");
	Check(result.maps.size() == 2, "the empty map file name is skipped");
	Check(result.maps.count("de_mirage") == 1 && result.maps.at("de_mirage") == "Mirage 2",
		  "the later display name wins for a duplicate map file");
}

void TestEmptyBlock() {
	const as_maps::LoadResult result = as_maps::LoadMaps({DataFile("maps_empty.ini")});
	Check(result.found && result.ok, "an empty Maps block is a valid, empty list");
	Check(result.maps.empty(), "no maps in an empty block");
}

void TestMalformed() {
	Check(!as_maps::ParseMaps("\"Maps\" { \"Mirage\" \"de_mirage\"").has_value(),
		  "a missing '}' is rejected");
	Check(!as_maps::ParseMaps("\"Mirage\" \"de_mirage\" }").has_value(), "a stray '}' is rejected");
	Check(!as_maps::ParseMaps("\"Mirage\" \"de_mirage").has_value(), "an unterminated string is rejected");
	const std::optional<as_maps::Maps> valueLess = as_maps::ParseMaps("\"Maps\" { \"Only\" }");
	Check(valueLess.has_value() && valueLess->empty(), "a value-less key is a valid, empty entry");
}

// --- the candidate walk --------------------------------------------------------------------------

void TestMissingFile() {
	const as_maps::LoadResult result = as_maps::LoadMaps({EmptyFile("a.ini"), EmptyFile("b.ini")});
	Check(!result.found, "no candidate exists: found is false");
	Check(!result.ok, "no candidate exists: not ok");
	Check(result.path.empty(), "no path is reported");
	Check(result.maps.empty(), "the list stays empty");
}

void TestFirstValidCandidateWins() {
	const as_maps::LoadResult result =
			as_maps::LoadMaps({EmptyFile("missing.ini"), DataFile("maps_other.ini"), DataFile("admin_system/maps.ini")});
	Check(result.ok, "the first existing valid candidate loads");
	Check(result.path == DataFile("maps_other.ini"), "the later candidate is not read");
	Check(result.maps.size() == 1 && result.maps.count("de_nuke") == 1, "the first file's content is used");
}

void TestMalformedFirstExistingIsAuthoritative() {
	// ResolvePath() returned the first file that exists and KeyValues parsed only that one: a
	// malformed first-existing file must not be hidden by a later valid candidate.
	const as_maps::LoadResult result =
			as_maps::LoadMaps({DataFile("maps_malformed.ini"), DataFile("admin_system/maps.ini")});
	Check(result.found, "the malformed file exists");
	Check(!result.ok, "the malformed first-existing file fails the load");
	Check(result.path == DataFile("maps_malformed.ini"), "the malformed path is reported");
	Check(result.maps.empty(), "no maps from the later candidate are used");
}

void TestTranslationsAndDataCandidates() {
	const as_maps::LoadResult translations = as_maps::LoadMaps(
			{EmptyFile("raw-missing.ini"), EmptyFile("admin-missing.ini"), DataFile("maps_other.ini")});
	Check(translations.ok && translations.path == DataFile("maps_other.ini"),
		  "a later (translations/data) candidate is used when the earlier ones are missing");
}

void TestAllCandidatesMalformed() {
	const as_maps::LoadResult result =
			as_maps::LoadMaps({DataFile("maps_malformed.ini"), DataFile("maps_malformed.ini")});
	Check(result.found, "a malformed file exists");
	Check(!result.ok, "no candidate parsed");
	Check(result.path == DataFile("maps_malformed.ini"), "the malformed path is reported for the log");
}

// --- the colour table ----------------------------------------------------------------------------

void TestColours() {
	Check(as_maps::Colorize("a {LIGHTRED}b{DEFAULT}c") == "a \x0F" "b\x01" "c",
		  "chat output gets the cs2-menus control bytes");
	Check(as_maps::StripColorTags("a {LIGHTRED}b{DEFAULT}c") == "a bc",
		  "the server console gets the tags stripped");
	Check(as_maps::StripColorTags("a\\nb") == "a\nb", "the phrase-file line break becomes a newline");
}

} // namespace

int main() {
	TestCandidatesRawPathFirst();
	TestCandidatesWithoutDirectories();
	TestCandidatesPartialDirectories();
	TestCandidatesTrailingSlash();
	TestProductionFile();
	TestCommentsUnquotedAndEscapes();
	TestBomAndRawRoot();
	TestDuplicatesLastWinsAndEmptyMapSkipped();
	TestEmptyBlock();
	TestMalformed();
	TestMissingFile();
	TestFirstValidCandidateWins();
	TestMalformedFirstExistingIsAuthoritative();
	TestTranslationsAndDataCandidates();
	TestAllCandidatesMalformed();
	TestColours();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
