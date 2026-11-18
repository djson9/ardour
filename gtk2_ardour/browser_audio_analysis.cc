/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "browser_audio_analysis.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "ardour/sndfileimportable.h"
#include "ardour/mp3fileimportable.h"
#include "ardour/ffmpegfileimportable.h"
#ifdef __APPLE__
#include "ardour/caimportable.h"
#endif
#include "audiographer/general/loudness_reader.h"
#include "audiographer/process_context.h"

namespace BrowserAudio {
static std::shared_ptr<ARDOUR::ImportableSource> open (std::string const& path)
{
 try { return std::make_shared<ARDOUR::SndFileImportableSource> (path); } catch (...) {}
#ifdef __APPLE__
 try { return std::make_shared<ARDOUR::CAImportableSource> (path); } catch (...) {}
#endif
 try { return std::make_shared<ARDOUR::Mp3FileImportableSource> (path); } catch (...) {}
 return std::make_shared<ARDOUR::FFMPEGFileImportableSource> (path);
}

std::shared_ptr<Analysis> analyze (std::string const& path, std::function<bool ()> const& cancelled)
{
 auto result = std::make_shared<Analysis> ();
 result->path = path;
 try {
  if (cancelled ()) return nullptr;
  auto source = open (path);
  auto channels = source->channels ();
  auto length = source->length ();
  auto rate = source->samplerate ();
  if (!channels || channels > 64 || length <= 0 || rate <= 0) throw std::runtime_error ("Invalid audio format");
  if (cancelled ()) return nullptr;
  constexpr size_t block = 8192;
  constexpr size_t bins = 2048;
  std::vector<float> samples (block * channels);
  AudioGrapher::LoudnessReader meter (rate, channels, samples.size ());
  result->peaks.resize (bins);
  result->channels = channels;
  ARDOUR::samplecnt_t position = 0;
  while (position < length) {
   if (cancelled ()) return nullptr;
   auto count = source->read (samples.data (), std::min<ARDOUR::samplecnt_t> (block, length-position) * channels);
   if (count <= 0 || count % channels) throw std::runtime_error ("Incomplete audio read");
   auto frames = count / channels;
   for (ARDOUR::samplecnt_t frame = 0; frame < frames; ++frame) {
    size_t bin = std::min (bins-1, size_t (double (position + frame) * bins / length));
    for (unsigned ch = 0; ch < channels; ++ch) {
     float sample = samples[frame*channels+ch];
     if (!std::isfinite (sample)) throw std::runtime_error ("Audio contains non-finite samples");
     result->sample_peak = std::max (result->sample_peak, std::abs (sample));
     result->peaks[bin].low = std::min (result->peaks[bin].low, sample);
     result->peaks[bin].high = std::max (result->peaks[bin].high, sample);
    }
   }
   meter.process (AudioGrapher::ProcessContext<float> (samples.data (), count, channels));
   position += frames;
  }
  if (cancelled ()) return nullptr;
  result->seconds = double (position) / rate;
  float integrated = -200, short_term = -200;
  result->loudness_measured = meter.get_loudness (&integrated, &short_term) && (integrated > -180 || short_term > -180);
  float inverse_gain = meter.calc_peak (-18, -1);
  if (result->sample_peak > 1e-9f && std::isfinite (inverse_gain) && inverse_gain > 0) {
   /* Also enforce the sample-peak ceiling and cap amplification at +24 dB.
    * Silent audio keeps unity gain; all channels retain their balance. */
   result->gain = std::min ({1.f / inverse_gain, 15.848932f, .89125094f / result->sample_peak});
  }
 } catch (...) {
  result->error = "Audio preview unavailable";
  result->peaks.clear ();
 }
 return cancelled () ? nullptr : result;
}
}
