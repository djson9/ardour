/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace BrowserAudio {
struct Peak { float low = 0; float high = 0; };
struct Analysis {
 std::string path;
 std::string error;
 std::vector<Peak> peaks;
 double seconds = 0;
 float sample_peak = 0;
 float gain = 1;
 unsigned channels = 0;
 bool loudness_measured = false;
};
/* No Session or GTK access. Decodes in bounded blocks; a cancelled result is
 * never returned. The same scalar gain applies to every channel. */
std::shared_ptr<Analysis> analyze (std::string const&, std::function<bool ()> const& cancelled);
}
