#pragma once

#include <fstream>
#include <functional>
#include "pbd/progress.h"
#include "ardour/region_factory.h"
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include "ardour/filter.h"
#include "ardour/audioregion.h"
#include "ardour/audiosource.h"
#include "ardour/region_fx_plugin.h"
#include "ardour/timefx_request.h"
#include "clip_warp.h"
#include "clip_warp_preview_source.h"

extern char** environ;

/* One drag reads its immutable original source once. Later pointer updates
 * reuse the private input file while each worker gets a fresh output file. */
struct ClipWarpInputCache {
 std::string path;
 const ARDOUR::Region* region = nullptr;
 ARDOUR::samplepos_t start = 0;
 ARDOUR::samplecnt_t count = 0;
 unsigned channels = 0;
 bool valid = false;
 void ensure () {
  if (!path.empty ()) return;
  auto pattern = Glib::build_filename (Glib::get_tmp_dir (), "ardour-warp-input-XXXXXX");
  std::vector<char> p (pattern.begin (),pattern.end ());p.push_back (0);
  const int fd = g_mkstemp (p.data ());
  if (fd < 0) throw std::runtime_error ("Cannot create preview input");
  ::close (fd);path=p.data ();
 }
 ~ClipWarpInputCache () { if (!path.empty ()) g_unlink (path.c_str ()); }
};

class ClipWarpFilter : public ARDOUR::Filter {
 ARDOUR::TimeFXRequest& request;
 ClipWarpSettings settings;
 std::string& failure;
 std::function<bool()> stop;
 bool preview;
 ClipWarpInputCache* input_cache;
 std::function<void(std::shared_ptr<ARDOUR::Region>, ARDOUR::samplecnt_t)> preview_update;
 bool cancelled () const { return request.cancel || (stop && stop ()); }
 template<class T> static std::string text (T v) { std::ostringstream s; s.imbue (std::locale::classic ()); s << std::setprecision (17) << v; return s.str (); }
 struct Scratch {
  std::string path;
  Scratch () { std::string pattern = Glib::build_filename (Glib::get_tmp_dir (), "ardour-warp-XXXXXX"); std::vector<char> p (pattern.begin (), pattern.end ()); p.push_back (0); char* d = g_mkdtemp (p.data ()); if (!d) throw std::runtime_error ("Cannot create warp workspace"); path = d; }
  ~Scratch () { for (const char* f : {"input.f32", "output.f32", "worker.log"}) g_unlink (Glib::build_filename (path, f).c_str ()); g_rmdir (path.c_str ()); }
 };
 void worker (const Scratch& tmp, std::string const& input_path, int rate, unsigned channels, double ratio, std::function<void()> tick = {}) {
  const std::string dir = ClipWarpSettings::engine_directory ();
  std::vector<std::string> args = {Glib::build_filename (dir, "ArdourWarpRender"), settings.mode,
   input_path, Glib::build_filename (tmp.path, "output.f32"),
   text (rate), text (channels), text (ratio), text (settings.pitch), text (settings.formants), text (settings.envelope),
   text (settings.grain), text (settings.flux), text (settings.preserve), text (settings.loop), text (settings.transient_envelope)};
  std::vector<char*> argv; for (auto& a : args) argv.push_back (const_cast<char*> (a.c_str ())); argv.push_back (0);
  posix_spawn_file_actions_t actions; posix_spawn_file_actions_init (&actions);
  posix_spawn_file_actions_addopen (&actions, STDOUT_FILENO, Glib::build_filename (tmp.path, "worker.log").c_str (), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  posix_spawn_file_actions_adddup2 (&actions, STDOUT_FILENO, STDERR_FILENO);
  pid_t pid; int err = posix_spawn (&pid, argv[0], &actions, 0, argv.data (), environ);
  posix_spawn_file_actions_destroy (&actions);
  if (err) throw std::runtime_error ("Cannot start the local warp engine: " + std::string (strerror (err)));
  int status = 0; const auto start = std::chrono::steady_clock::now ();
  bool reaped = false;
  try {
  while (true) {
   pid_t result = waitpid (pid, &status, WNOHANG);
   if (result == pid) { reaped = true;break; }
   if (result < 0 && errno != EINTR) throw std::runtime_error ("Cannot wait for warp engine");
   if (cancelled () || std::chrono::steady_clock::now () - start > std::chrono::minutes (5)) {
    throw std::runtime_error (cancelled () ? "Warp cancelled" : "Warp engine timed out");
   }
   if (tick) tick ();
   Glib::usleep (10000);
  }
  } catch (...) {
   if (!reaped) { kill (pid, SIGKILL);while (waitpid (pid, &status, 0) < 0 && errno == EINTR) {} }
   throw;
  }
  if (!WIFEXITED (status) || WEXITSTATUS (status)) {
   std::ifstream log (Glib::build_filename (tmp.path, "worker.log")); char message[2049] = {};
   log.read (message, 2048); throw std::runtime_error ("Warp engine failed: " + std::string (message));
  }
 }
public:
 ClipWarpFilter (ARDOUR::Session& s, ARDOUR::TimeFXRequest& req, ClipWarpSettings const& options, std::string& error, std::function<bool()> cancel = {}, bool is_preview = false, std::function<void(std::shared_ptr<ARDOUR::Region>, ARDOUR::samplecnt_t)> update = {}, ClipWarpInputCache* cache = nullptr)
 : Filter (s), request (req), settings (options), failure (error), stop (cancel), preview (is_preview), input_cache (is_preview ? cache : nullptr), preview_update (update) {}
 int run (std::shared_ptr<ARDOUR::Region> r, PBD::Progress* progress) {
  using namespace ARDOUR;
  SourceList sources;
  try {
   auto region = std::dynamic_pointer_cast<AudioRegion> (r);
   if (!region || region->locked ()) throw std::runtime_error ("Select an unlocked audio clip");
   const unsigned channels = region->n_channels ();
   if (channels < 1 || channels > 2) throw std::runtime_error ("The local warp engines currently support mono and stereo clips");
   const double ratio = region->stretch () * request.time_fraction.to_double ();
   if (!std::isfinite (ratio) || ratio < .25 || ratio > 4.) throw std::runtime_error ("Warp duration must be between 0.25 and 4 times the original");
   const auto previous = ClipWarpSettings::read (*region);
   const samplecnt_t count = llround (region->length_samples () / region->stretch ());
   const samplecnt_t wanted = llround (region->length_samples () * request.time_fraction.to_double ());
   samplepos_t start = region->ancestral_start_sample () + llround (region->start_sample () / region->stretch ());
   if (previous.reverse) start = region->ancestral_start_sample () + region->ancestral_length_samples () - llround ((region->start_sample () + region->length_samples ()) / region->stretch ());
   if (count <= 0 || wanted <= 0 || start < 0) throw std::runtime_error ("Invalid source range for warp");
   const bool neutral = std::abs (ratio - 1.) < 1e-12 && (settings.mode == "repitch" || settings.pitch == 0.);
   if (!neutral && !settings.available ()) throw std::runtime_error ("The original " + settings.mode + " engine is not installed");
   auto configure = [&] (std::shared_ptr<ARDOUR::Region> result) {
    auto audio = std::dynamic_pointer_cast<AudioRegion> (result);
    audio->envelope ()->x_scale (request.time_fraction);
    audio->foreach_plugin ([&](std::weak_ptr<RegionFxPlugin> w) { auto fx = w.lock (); if (fx) fx->x_scale_automation (request.time_fraction); });
    result->set_ancestral_data (Temporal::timepos_t (start), Temporal::timecnt_t (count, Temporal::timepos_t (start)), double (wanted) / count, std::pow (1.0594631433486938f, float (settings.pitch)));
    result->set_master_sources (region->master_sources ());
    result->set_length_unchecked (Temporal::timecnt_t (wanted, region->position ()));
    /* Insert this exact region: whole-file regions are cloned by Playlist,
     * which would discard the per-clip extra XML and selection identity. */
    result->set_whole_file (false);
    settings.write (*result);
   };
   Scratch scratch;
   std::vector<float> mono (8192), interleaved (8192 * channels);
   if (input_cache) input_cache->ensure ();
   const auto input_path = input_cache ? input_cache->path : Glib::build_filename (scratch.path, "input.f32");
   const bool cached = input_cache && input_cache->valid && input_cache->region == region.get () && input_cache->start == start && input_cache->count == count && input_cache->channels == channels;
   if (!cached) {
    if (input_cache) input_cache->valid = false;
    std::ofstream input (input_path, std::ios::binary);
    if (!input) throw std::runtime_error ("Cannot write warp input");
    for (samplecnt_t pos = 0; pos < count; pos += 8192) {
     if (cancelled ()) throw std::runtime_error ("Warp cancelled");
     samplecnt_t n = std::min<samplecnt_t> (8192, count - pos);
     for (unsigned ch = 0; ch < channels; ++ch) {
      if (region->master_read_at (mono.data (), start + pos - region->start_sample () + region->position_sample (), n, ch) != n) throw std::runtime_error ("Cannot read original clip audio");
      for (samplecnt_t i = 0; i < n; ++i) { if (!std::isfinite (mono[i])) throw std::runtime_error ("Non-finite source audio"); interleaved[i * channels + ch] = mono[i]; }
     }
     input.write (reinterpret_cast<char*> (interleaved.data ()), n * channels * sizeof (float));
     if (!input) throw std::runtime_error ("Cannot write warp input");
     if (progress) progress->set_progress (.2 * double (pos + n) / count);
    }
    input.close ();
    if (!input) throw std::runtime_error ("Cannot finish warp input");
    if (input_cache) { input_cache->region=region.get ();input_cache->start=start;input_cache->count=count;input_cache->channels=channels;input_cache->valid=true; }
   }
   const auto output_path = neutral ? input_path : Glib::build_filename (scratch.path, "output.f32");
   std::shared_ptr<ClipWarpPreviewPCM> mapping;
   samplecnt_t last_available = -1;
   auto update_preview = [&] {
    if (!preview || cancelled ()) return;
    struct stat st;if (g_stat (output_path.c_str (), &st) || st.st_size <= 0) return;
    const size_t bytes = size_t(st.st_size);
    if (bytes > size_t(wanted)*channels*sizeof(float)) throw std::runtime_error ("Oversized preview output");
    const samplecnt_t available = bytes/(channels*sizeof(float));
    if (available == last_available) return;
    last_available = available;
    if (!mapping) {
     mapping = std::make_shared<ClipWarpPreviewPCM> (output_path, size_t(wanted)*channels*sizeof(float));
     for (unsigned ch=0;ch<channels;++ch) sources.push_back (std::make_shared<ClipWarpPreviewSource> (session, mapping, ch, channels, wanted, session.sample_rate (), settings.reverse));
     PBD::PropertyList props (region->derive_properties (true, true));
     props.add (Properties::start, Temporal::timepos_t (0));props.add (Properties::whole_file, false);
     auto result = RegionFactory::create (sources, props, false);
     std::dynamic_pointer_cast<AudioRegion> (result)->copy_plugin_state (region);
     configure (result);results.push_back (result);
    }
    mapping->available_bytes.store (size_t(available)*channels*sizeof(float), std::memory_order_release);
    if (preview_update) preview_update (results.front (), available);
   };
   if (!neutral) worker (scratch, input_path, session.sample_rate (), channels, double(wanted)/count, update_preview);
   std::ifstream output (output_path, std::ios::binary | std::ios::ate);
   if (!output || output.tellg () != std::streamoff (wanted * channels * sizeof(float))) throw std::runtime_error ("Warp engine returned an incomplete clip");
   if (cancelled ()) throw std::runtime_error ("Warp cancelled");
   if (preview) { update_preview ();return 0; }
   if (make_new_sources (region, sources, "@warp")) throw std::runtime_error ("Cannot create warped audio sources");
   for (samplecnt_t pos = 0; pos < wanted; pos += 8192) {
    if (cancelled ()) throw std::runtime_error ("Warp cancelled");
    samplecnt_t n = std::min<samplecnt_t> (8192, wanted - pos);
    samplecnt_t offset = settings.reverse ? wanted - pos - n : pos;
    output.seekg (offset * channels * sizeof (float)); output.read (reinterpret_cast<char*> (interleaved.data ()), n * channels * sizeof (float));
    if (!output) throw std::runtime_error ("Cannot read warped audio");
    for (unsigned ch = 0; ch < channels; ++ch) {
     for (samplecnt_t i = 0; i < n; ++i) { mono[i] = interleaved[(settings.reverse ? n - i - 1 : i) * channels + ch]; if (!std::isfinite (mono[i])) throw std::runtime_error ("Non-finite warped audio"); }
     auto source = std::dynamic_pointer_cast<AudioSource> (sources[ch]);
     if (!source || source->write (mono.data (), n) != n) throw std::runtime_error ("Cannot write warped audio");
    }
    if (progress) progress->set_progress (.75 + .25 * double (pos + n) / wanted);
   }
   if (finish (region, sources, region->name ())) throw std::runtime_error ("Cannot create warped clip");
   for (auto& result : results) configure (result);
   return 0;
  } catch (std::exception const& e) {
   failure = e.what (); if (failure != "Warp cancelled") PBD::error << failure << endmsg;
   for (auto& source : sources) source->mark_for_remove ();
   return -1;
  }
 }
};
