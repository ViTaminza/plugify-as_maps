// as_maps — the cs2-menus colour tags of the Pisex phrase files.
//
// The admin_system phrases carry cs2-menus colour tags ("Changing_Map" is
// "Смена карты на {LIGHTRED}%s{DEFAULT}..."), and pisexcompat decoded them before printing:
// PrintToChatAll() colourised the control bytes for chat, while the server console got the tags
// stripped. The port keeps the same table and the same two operations (the sibling standalone
// plugins as_rcon/resetscore/vip_vips carry their own copy — no shared helper library,
// docs/PLUGIN_STANDARD.md §7).
//
// Header-only and engine-free: src/plugin.cpp and the plugin tests share it.

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace as_maps {

struct ColorTag {
	std::string_view tag;
	std::string_view bytes;
};

// cs2-menus utils.h colors_text[]/colors_hex[], the same table pisexcompat carried. Only
// {LIGHTRED} and {DEFAULT} appear in the Changing_Map / Changing_Map_Admin / UsageChangeMap
// phrases; the whole table is kept so a translated phrase with any tag behaves like it did.
inline constexpr std::array<ColorTag, 17> kColorTags = {{{"{DEFAULT}", "\x01"},
														 {"{WHITE}", "\x01"},
														 {"{RED}", "\x02"},
														 {"{LIGHTPURPLE}", "\x03"},
														 {"{GREEN}", "\x04"},
														 {"{LIME}", "\x05"},
														 {"{LIGHTGREEN}", "\x06"},
														 {"{DARKRED}", "\x07"},
														 {"{GRAY}", "\x08"},
														 {"{LIGHTOLIVE}", "\x09"},
														 {"{OLIVE}", "\x10"},
														 {"{LIGHTBLUE}", "\x0B"},
														 {"{BLUE}", "\x0C"},
														 {"{PURPLE}", "\x0E"},
														 {"{LIGHTRED}", "\x0F"},
														 {"{GRAYBLUE}", "\x0A"},
														 {"\\n", "\xe2\x80\xa9"}}};

// strip=false: the tag becomes its chat/console control byte; strip=true: it is removed and the
// phrase-file line break "\n" becomes a real newline (server console output).
inline std::string ReplaceColorTags(std::string_view text, bool strip) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		const std::string_view replacement =
				strip ? (color.tag == "\\n" ? std::string_view("\n") : std::string_view()) : color.bytes;
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), replacement);
			pos += replacement.size();
		}
	}
	return out;
}

// What pisexcompat's PrintToChatAll() did with the formatted phrase.
inline std::string Colorize(std::string_view text) {
	return ReplaceColorTags(text, /*strip=*/false);
}

// What the server console got (META_CONPRINTF of the original): the tags have no meaning there.
inline std::string StripColorTags(std::string_view text) {
	return ReplaceColorTags(text, /*strip=*/true);
}

} // namespace as_maps
