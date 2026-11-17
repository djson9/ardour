#pragma once
#include <memory>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "ardour/audiosource.h"

/* Private raw-PCM preview storage. A mapping retains the rendered bytes after
 * the worker's scratch file is unlinked; no source is registered with Session,
 * and no preview WAV or peak file is created in the user's project. Reads occur
 * on the preview renderer thread, never on the audio callback. */
class ClipWarpPreviewNotReady : public std::exception {};
class ClipWarpPreviewPCM {
public:
 const float* data;
 size_t bytes;
 std::atomic<size_t> available_bytes {0};
 ClipWarpPreviewPCM (std::string const& path, size_t size) : data (nullptr), bytes (size) {
  const int fd = ::open (path.c_str (), O_RDONLY);
  if (fd < 0) throw std::runtime_error ("Cannot open preview PCM");
  void* address = mmap (nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);::close (fd);
  if (address == MAP_FAILED) throw std::runtime_error ("Cannot map preview PCM");
  data = static_cast<const float*> (address);
 }
 ~ClipWarpPreviewPCM () { munmap (const_cast<float*> (data), bytes); }
 ClipWarpPreviewPCM (ClipWarpPreviewPCM const&) = delete;
};
class ClipWarpPreviewSource : public ARDOUR::AudioSource {
 std::shared_ptr<ClipWarpPreviewPCM> pcm;
 unsigned channel, channels;
 ARDOUR::samplecnt_t frames;
 float rate;
 bool reversed;
public:
 ClipWarpPreviewSource (ARDOUR::Session& session, std::shared_ptr<ClipWarpPreviewPCM> mapping,
                        unsigned ch, unsigned count, ARDOUR::samplecnt_t length, float sample_rate, bool reverse)
 : ARDOUR::Source (session, ARDOUR::DataType::AUDIO, "warp-preview", ARDOUR::Source::Flag (ARDOUR::Source::Transient | ARDOUR::Source::NoPeakFile)),
   ARDOUR::AudioSource (session, "warp-preview"), pcm (mapping), channel (ch), channels (count), frames (length), rate (sample_rate), reversed (reverse) {
  _length = Temporal::timepos_t (frames);
 }
 float sample_rate () const override { return rate; }
 bool clamped_at_unity () const override { return false; }
 bool ready (ARDOUR::samplepos_t start, ARDOUR::samplecnt_t count) const {
  if (start < 0 || start >= frames || count <= 0) return true;
  count = std::min (count, frames-start);
  const auto required = reversed ? frames-start : start+count;
  return size_t(required)*channels*sizeof(float) <= pcm->available_bytes.load (std::memory_order_acquire);
 }
protected:
 ARDOUR::samplecnt_t read_unlocked (ARDOUR::Sample* dst, ARDOUR::samplepos_t start, ARDOUR::samplecnt_t count) const override {
  if (start < 0 || start >= frames) return 0;
  count = std::min (count, frames-start);
  if (!ready (start, count)) throw ClipWarpPreviewNotReady ();
  for (ARDOUR::samplecnt_t i=0;i<count;++i) {
   const auto frame = reversed ? frames-1-start-i : start+i;
   const float value = pcm->data[size_t(frame)*channels+channel];
   if (!std::isfinite (value)) throw std::runtime_error ("Non-finite preview PCM");
   dst[i] = value;
  }
  return count;
 }
 ARDOUR::samplecnt_t write_unlocked (ARDOUR::Sample const*, ARDOUR::samplecnt_t) override { return 0; }
 std::string construct_peak_filepath (std::string const&, bool, bool) const override { return ""; }
};
