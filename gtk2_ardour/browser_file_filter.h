/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <set>
#include <sstream>
#include <string>

namespace BrowserFileFilter {
inline std::string defaults ()
{
	return "wav, aif, aiff, aifc, flac, ogg, oga, opus, mp3, mp2, m4a, caf, "
	       "aac, adts, ac3, amr, mpa, mpeg, mp1, mp4, sd2, amb, au, cdr, htk, "
	       "iff, mat, paf, pvf, sf, smp, snd, maud, voc, vwe, w64, rf64, mid, midi";
}
inline std::string lowercase (std::string text)
{
	for (auto& c : text) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
	return text;
}
/* Extensions, .extensions or *.extensions, never arbitrary globs or paths.
 * An empty whitelist deliberately shows folders only. */
inline bool parse (std::string text, std::set<std::string>& extensions)
{
	std::set<std::string> parsed;
	for (auto& c : text) { if (c == ',' || c == ';') { c = ' '; } }
	std::istringstream stream (text);
	std::string ext;
	while (stream >> ext) {
		if (ext.compare (0, 2, "*.") == 0) { ext.erase (0, 2); }
		else if (ext[0] == '.') { ext.erase (0, 1); }
		ext = lowercase (ext);
		if (ext.empty () || ext.find_first_not_of ("abcdefghijklmnopqrstuvwxyz0123456789") != std::string::npos) { return false; }
		parsed.insert (ext);
	}
	extensions.swap (parsed);
	return true;
}
inline std::string serialize (std::set<std::string> const& extensions)
{
	std::string text;
	for (auto const& ext : extensions) { if (!text.empty ()) { text += ", "; } text += ext; }
	return text;
}
inline bool matches (std::string const& filename, std::set<std::string> const& extensions)
{
	auto dot = filename.rfind ('.');
	return dot != std::string::npos && dot > 0 && extensions.count (lowercase (filename.substr (dot + 1)));
}
}
