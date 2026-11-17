#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "ardour/audioplaylist.h"
#include "ardour/location.h"
#include "ardour/disk_reader.h"
#include "ardour/region_factory.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/session_event.h"
#include "clip_warp_filter.h"

/* A drag owns one background renderer and one isolated playlist. No render
 * changes the real playlist or adds an undo command; release commits once. */
class ClipWarpPreview {
 ARDOUR::Session& session;
 std::shared_ptr<ARDOUR::DiskReader> reader;
 std::shared_ptr<ARDOUR::AudioPlaylist> playlist;
 std::shared_ptr<ARDOUR::Region> original, replacement;
 unsigned channels;
 std::atomic<bool> stopping {false};
 std::atomic<uint64_t> sequence {0};
 std::mutex mutex;
 std::condition_variable changed;
 std::thread thread;
 ClipWarpSettings options;
 double fraction = 1.;
 bool fixed_end = false;
 void discard () {
  if (!replacement) return;
  playlist->remove_region (replacement);
  for (auto const& source : replacement->sources ()) source->mark_for_remove ();
  replacement.reset ();
 }
 void run () {
  using namespace ARDOUR;
  SessionEvent::create_per_thread_pool ("warp preview", 64);
  Temporal::TempoMap::fetch ();
  TimeFXRequest request; request.process_thread.get_buffers ();
  ClipWarpInputCache input_cache;
  uint64_t rendered = 0, reported = 0;
  auto render_started = std::chrono::steady_clock::now ();
  while (!stopping.load ()) {
   ClipWarpSettings settings;double ratio;bool anchor;uint64_t version;
   {
    std::unique_lock<std::mutex> lock (mutex);
    changed.wait_for (lock, std::chrono::milliseconds (100), [&] { return stopping.load () || sequence.load () != rendered; });
    if (stopping.load ()) break;
    version = sequence.load ();settings = options;ratio = fraction;anchor = fixed_end;
   }
   if (!version) continue;
   const auto cancelled = [&] { return stopping.load () || sequence.load () != version; };
   try {
    auto publish_window = [&] (ARDOUR::samplecnt_t available) {
     if (!replacement || cancelled ()) return;
     try {
    // Include existing neighbours/overlaps, gain, fades and region effects.
    // This window replaces track disk audio before track plugins/panning.
    ARDOUR::samplepos_t start = std::max<ARDOUR::samplepos_t> (0, session.transport_sample () - session.sample_rate ()/10);
    ARDOUR::samplepos_t loop_start=0,loop_end=0;
    if (session.get_play_loop ()) {
     if (auto* loop = session.locations ()->auto_loop_location ()) {
      loop_start=loop->start ().samples ();loop_end=loop->end ().samples ();
      if (session.transport_sample () >= loop_start && session.transport_sample () < loop_end) start=std::max (start,loop_start);
     }
    }
    const ARDOUR::samplecnt_t count = session.sample_rate ()*2;
    // Preflight the whole window before any playlist/region-FX state advances.
    auto source = std::dynamic_pointer_cast<ClipWarpPreviewSource> (replacement->sources ().front ());
    for (ARDOUR::samplecnt_t at=0;source && at<count;) {
     auto position=start+at;
     auto n=std::min<ARDOUR::samplecnt_t> (8192, count-at);
     if (loop_end > loop_start) {
      if (position >= loop_end) position=loop_start+(position-loop_start)%(loop_end-loop_start);
      n=std::min<ARDOUR::samplecnt_t> (n,loop_end-position);
     }
     const auto first=std::max (position,replacement->position_sample ());
     const auto last=std::min (position+n,replacement->position_sample ()+replacement->length_samples ());
     if (last>first && !source->ready (replacement->start_sample ()+first-replacement->position_sample (),last-first)) return;
     at+=n;
    }
    std::vector<std::vector<float>> pcm (channels, std::vector<float> (count));
    std::vector<float> mix (8192), gain (8192);
    for (unsigned ch=0;ch<channels && !cancelled ();++ch) {
     for (ARDOUR::samplecnt_t at=0;at<count && !cancelled ();) {
      auto position=start+at;
      auto n = std::min<ARDOUR::samplecnt_t> (8192, count-at);
      if (loop_end > loop_start) {
       if (position >= loop_end) position=loop_start+(position-loop_start)%(loop_end-loop_start);
       n=std::min<ARDOUR::samplecnt_t> (n,loop_end-position);
      }
      playlist->read (pcm[ch].data ()+at, mix.data (), gain.data (), Temporal::timepos_t (position), Temporal::timecnt_t (n), ch);
      at+=n;
     }
    }
    if (!cancelled () && reader->warp_preview ().publish (version, start, std::move (pcm), loop_start, loop_end) && reported != version) {
     const double elapsed = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now ()-render_started).count ();
     PBD::info << "Warp preview ready: generation=" << version << " ratio=" << ratio << " renderMs=" << elapsed << " availableFrames=" << available << " totalFrames=" << replacement->length_samples () << endmsg;
     reported = version;
    }
     } catch (ClipWarpPreviewNotReady const&) {
      // Keep the last complete window until this worker has enough PCM.
     }
    };
    if (rendered != version) {
     render_started = std::chrono::steady_clock::now ();
     request.time_fraction = Temporal::ratio_t (llround (ratio*1000000000.), 1000000000);
     std::string error;
     auto update = [&] (std::shared_ptr<ARDOUR::Region> result, ARDOUR::samplecnt_t available) {
      if (cancelled ()) return;
      if (replacement != result) {
       discard ();
       playlist->remove_region (original);
       replacement = result;
       const auto position = anchor ? original->end_position ().earlier (replacement->length ()) : original->position ();
       playlist->add_region (replacement, position);
       replacement->set_layer (original->layer ());replacement->set_layering_index (original->layering_index ());
      }
      publish_window (available);
     };
     ClipWarpFilter filter (session, request, settings, error, cancelled, true, update, &input_cache);
     if (filter.run (original, nullptr)) { discard ();if (!cancelled ()) reader->warp_preview ().cancel ();rendered = version;continue; }
     rendered = version;
    }
    if (replacement) publish_window (replacement->length_samples ());
   } catch (std::exception const& e) {
    PBD::error << "Warp preview: " << e.what () << endmsg;discard ();if (!cancelled ()) reader->warp_preview ().cancel ();rendered = version;
   }
  }
  discard ();reader->warp_preview ().cancel ();request.process_thread.drop_buffers ();
 }
public:
 ClipWarpPreview (ARDOUR::Session& s, std::shared_ptr<ARDOUR::Region> region, std::shared_ptr<ARDOUR::Route> route)
 : session (s), channels (std::max (1u, route->n_inputs ().n_audio ())) {
  route->foreach_processor ([&] (std::weak_ptr<ARDOUR::Processor> p) { if (auto r = std::dynamic_pointer_cast<ARDOUR::DiskReader> (p.lock ())) reader = r; });
  if (!reader || !region->playlist ()) return;
  playlist = std::make_shared<ARDOUR::AudioPlaylist> (session, "warp-drag-preview", true);
  region->playlist ()->foreach_region ([&] (std::shared_ptr<ARDOUR::Region> r) {
   auto copy = ARDOUR::RegionFactory::create (std::shared_ptr<const ARDOUR::Region> (r), false);
   copy->set_whole_file (false);playlist->add_region (copy, r->position ());copy->set_layer (r->layer ());copy->set_layering_index (r->layering_index ());
   if (r == region) original = copy;
  });
  if (original) thread = std::thread ([this] { run (); });
 }
 ~ClipWarpPreview () { stopping.store (true);changed.notify_one ();if (thread.joinable ()) thread.join (); }
 void update (ClipWarpSettings const& settings, double ratio, bool anchor) {
  if (!thread.joinable ()) return;
  std::lock_guard<std::mutex> lock (mutex);
  if (sequence.load () && ratio == fraction && anchor == fixed_end) return;
  options = settings; fraction = ratio;fixed_end = anchor;
  sequence.store (reader->warp_preview ().request ());changed.notify_one ();
 }
};
