/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

/* Editor-side shape model. Playback uses the existing AutomationList, compiled
 * adaptively from these handles. No new work is added to the audio callback. */
namespace AutomationClipCurve {
struct Point {
 int64_t when;
 double value;
 double bend = 0; // outgoing segment, -1 .. 1; zero is a straight line
};
inline bool operator== (Point const& a, Point const& b) { return a.when == b.when && a.value == b.value && a.bend == b.bend; }
inline double evaluate (Point const& a, Point const& b, double when) {
 if (when <= a.when) return a.value;
 if (when >= b.when) return b.value;
 const double t = (when - a.when) / (b.when - a.when);
 const double k = -8.0 * a.bend;
 const double u = std::abs (k) < 1e-8 ? t : std::expm1 (k * t) / std::expm1 (k);
 return a.value + (b.value - a.value) * u;
}
inline void subdivide (Point const& a, Point const& b, Point l, Point r, std::vector<Point>& out, int depth = 0) {
 double error = 0;
 for (int q = 1; q < 4; ++q) {
  const double x = l.when + (r.when - l.when) * (q / 4.0);
  const double linear = l.value + (r.value - l.value) * (q / 4.0);
  error = std::max (error, std::abs (evaluate (a, b, x) - linear));
 }
 if (r.when - l.when > 1 && depth < 24 && error > 0.00005) {
  const int64_t x = l.when + (r.when - l.when) / 2;
  const Point mid {x, evaluate (a, b, x), 0};
  subdivide (a, b, l, mid, out, depth + 1);
  subdivide (a, b, mid, r, out, depth + 1);
 } else { out.push_back (r); }
}
inline std::vector<Point> compile (std::vector<Point> const& points) {
 std::vector<Point> out;
 if (points.empty ()) return out;
 out.push_back (points.front ());
 for (size_t i = 1; i < points.size (); ++i) subdivide (points[i-1], points[i], points[i-1], points[i], out);
 return out;
}
}
