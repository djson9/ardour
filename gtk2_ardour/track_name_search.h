/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
#include <string>
#include <vector>

/* Inputs are case-folded by the caller. Prefer exact/prefix/substring matches,
 * then abbreviations, then a typo-tolerant substring edit distance. */
inline double track_name_score (std::string const& name, std::string const& query) {
 if (query.empty ()) return 1e9;
 if (name == query) return 0;
 const auto position = name.find (query);
 if (position != std::string::npos) return (position == 0 ? 1.0 : 2.0) + .001 * (name.size ()-query.size ());
 size_t matched = 0;
 for (auto c : name) if (matched < query.size () && c == query[matched]) ++matched;
 if (matched == query.size ()) return 3 + .001 * name.size ();
 std::vector<size_t> previous (name.size ()+1, 0), current (name.size ()+1);
 for (size_t i=1; i<=query.size (); ++i) {
  current[0] = i;
  for (size_t j=1; j<=name.size (); ++j) current[j] = std::min ({previous[j]+1,current[j-1]+1,previous[j-1]+size_t (query[i-1]!=name[j-1])});
  previous.swap (current);
 }
 return 4 + *std::min_element (previous.begin (), previous.end ()) + .001 * name.size ();
}
