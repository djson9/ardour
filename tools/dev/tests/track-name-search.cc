/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../../../gtk2_ardour/track_name_search.h"
#include <cassert>
#include <iostream>
int main () {
 const std::vector<std::string> names {"master", "automation", "chord grid - electric piano", "bass", "bass synth", "piano"};
 auto best = [&] (std::string const& q) { return *std::min_element (names.begin (), names.end (), [&] (auto const& a, auto const& b) {return track_name_score (a,q)<track_name_score (b,q);}); };
 assert (best ("piano")=="piano"); assert (best ("pinao")=="piano"); assert (best ("elctric")==names[2]);
 assert (best ("cgp")==names[2]); assert (best ("bass")=="bass"); assert (best ("bass sy")=="bass synth");
 assert (track_name_score ("piano", "") == 1e9);
 std::cout << "7 track-search checks passed\n";
}
