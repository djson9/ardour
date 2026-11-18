/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstring>
#include "ardour/audiosource.h"
#include "ardour/session.h"

namespace ARDOUR {
/* A file-free carrier for timeline envelopes. Never produces sound. */
class LIBARDOUR_API AutomationClipSource : public AudioSource {
public:
 AutomationClipSource (Session& s)
  : Source (s, DataType::AUDIO, "Automation", Source::NoPeakFile), AudioSource (s, "Automation") {
  _length = timepos_t (samplecnt_t (s.sample_rate()) * 60 * 60 * 24);
 }
 AutomationClipSource (Session& s, XMLNode const& n)
  : Source (s, n), AudioSource (s, n) {
  _length = timepos_t (samplecnt_t (s.sample_rate()) * 60 * 60 * 24);
 }
 XMLNode& get_state () const override {
  XMLNode& n = AudioSource::get_state(); n.set_property ("automation-clip", true); return n;
 }
 float sample_rate () const override { return _session.sample_rate(); }
 bool clamped_at_unity () const override { return true; }
 samplecnt_t available_peaks (double) const override { return _length.samples(); }
protected:
 samplecnt_t read_unlocked (Sample* dst, samplepos_t, samplecnt_t n) const override {
  memset (dst, 0, sizeof (Sample) * n); return n;
 }
 samplecnt_t write_unlocked (Sample const*, samplecnt_t) override { return 0; }
 std::string construct_peak_filepath (std::string const&, bool, bool) const override { return ""; }
 int read_peaks_with_fpp (PeakData* dst, samplecnt_t n, samplepos_t, samplecnt_t, double, samplecnt_t) const override {
  memset (dst, 0, sizeof (PeakData) * n); return 0;
 }
};
}
