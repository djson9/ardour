/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "browser_audio_preview.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <cairomm/context.h>
#include "ui_config.h"
#include "gtkmm2ext/colors.h"
#include "pbd/i18n.h"

struct BrowserAudioPreview::Worker {
 std::mutex mutex;
 std::condition_variable changed;
 std::atomic<uint64_t> generation {0};
 std::atomic<bool> stopping {false};
 std::string path, signature;
 uint64_t completed = 0;
 std::shared_ptr<BrowserAudio::Analysis const> result;
 std::thread thread;
 Worker () : thread ([this] { run (); }) {}
 ~Worker () { stopping = true; changed.notify_one (); thread.join (); }
 void run () {
  uint64_t seen = 0;
  std::deque<std::pair<std::string, std::shared_ptr<BrowserAudio::Analysis const>>> cache;
  while (!stopping) {
   std::string file, key;
   uint64_t version;
   {
    std::unique_lock<std::mutex> lock (mutex);
    changed.wait (lock, [&] { return stopping || generation != seen; });
    if (stopping) break;
    version = generation; seen = version; file = path; key = signature;
   }
   if (file.empty ()) continue;
   auto cancelled = [&] { return stopping || generation != version; };
   std::shared_ptr<BrowserAudio::Analysis const> data;
   for (auto const& item : cache) if (item.first == key) { data = item.second; break; }
   if (!data) {
    data = BrowserAudio::analyze (file, cancelled);
    if (data && data->error.empty ()) {
     cache.emplace_front (key, data);
     if (cache.size () > 4) cache.pop_back ();
    }
   }
   if (!data || cancelled ()) continue;
   std::lock_guard<std::mutex> lock (mutex);
   if (generation == version) { completed = version; result = data; }
  }
 }
};

BrowserAudioPreview::BrowserAudioPreview () : _worker (new Worker)
{
 set_name ("browser-audio-waveform");
 set_size_request (180, 92);
 add_events (Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK);
 _poll = Glib::signal_timeout ().connect (sigc::mem_fun (*this, &BrowserAudioPreview::poll), 30);
}
BrowserAudioPreview::~BrowserAudioPreview () { _poll.disconnect (); _worker.reset (); }

void BrowserAudioPreview::set_file (std::string const& path)
{
 struct stat info;
 std::string key = path;
 if (!path.empty () && !stat (path.c_str (), &info)) {
#ifdef __APPLE__
  auto modified = info.st_mtimespec;
#else
  auto modified = info.st_mtim;
#endif
  key += ":" + std::to_string (info.st_ino) + ":" + std::to_string (info.st_size) + ":" + std::to_string (modified.tv_sec) + ":" + std::to_string (modified.tv_nsec);
 }
 if (_path == path && _signature == key) return;
 _path = path; _signature = key; _analysis.reset (); _position = 0; _dragging = false;
 {
  std::lock_guard<std::mutex> lock (_worker->mutex);
  _worker->path = path; _worker->signature = key; _worker->result.reset (); ++_worker->generation;
 }
 _worker->changed.notify_one ();
 queue_draw ();
}
void BrowserAudioPreview::clear () { set_file (""); }
void BrowserAudioPreview::set_normalized (bool value) { _normalized = value; queue_draw (); }
void BrowserAudioPreview::set_position (double position) { if (!_dragging) { _position = std::max (0., std::min (1., position)); queue_draw (); } }
bool BrowserAudioPreview::poll ()
{
 std::shared_ptr<BrowserAudio::Analysis const> result;
 {
  std::lock_guard<std::mutex> lock (_worker->mutex);
  if (_worker->completed == _worker->generation) result = _worker->result;
 }
 if (result && result != _analysis) { _analysis = result; queue_draw (); Ready (); }
 return true;
}

bool BrowserAudioPreview::on_expose_event (GdkEventExpose*)
{
 auto cr = get_window ()->create_cairo_context ();
 const double width = get_allocation ().get_width (), height = get_allocation ().get_height ();
 auto color = [&] (char const* name, double alpha) {
  double r, g, b, a; Gtkmm2ext::color_to_rgba (UIConfiguration::instance ().color (name), r, g, b, a); cr->set_source_rgba (r, g, b, alpha);
 };
 color ("neutral:backgroundest", 1); cr->paint ();
 std::string label = _path.empty () ? _("Select an audio file") : Glib::path_get_basename (_path);
 if (!_path.empty () && !_analysis) label = _("Analyzing audio…");
 if (_analysis && !_analysis->error.empty ()) label = _("Audio preview unavailable");
 auto text = create_pango_layout (label); text->set_ellipsize (Pango::ELLIPSIZE_END); text->set_width ((width-12)*Pango::SCALE);
 color ("neutral:foreground", .85); cr->move_to (6, 3); text->show_in_cairo_context (cr);
 if (!_analysis || _analysis->peaks.empty ()) return true;
 const double middle = 22 + (height-40)*.5, scale = (height-42)*.5;
 const float gain = _normalized ? _analysis->gain : 1;
 cr->set_line_width (1);
 color ("neutral:foreground", .2); cr->move_to (4,middle); cr->line_to (width-4,middle); cr->stroke ();
 color ("selected waveform fill", .9);
 for (int x = 4; x < width-4; ++x) {
  size_t first = size_t ((x-4) * _analysis->peaks.size () / (width-8));
  size_t end = std::min (_analysis->peaks.size (), size_t ((x-3) * _analysis->peaks.size () / (width-8)) + 1);
  float low=0, high=0;
  for (size_t n=first; n<end; ++n) { low=std::min (low,_analysis->peaks[n].low); high=std::max (high,_analysis->peaks[n].high); }
  cr->move_to (x+.5, middle-std::min (1.f,high*gain)*scale);
  cr->line_to (x+.5, middle-std::max (-1.f,low*gain)*scale);
 }
 cr->stroke ();
 double playhead = 4 + _position*(width-8);
 color ("play head", 1); cr->set_line_width (2); cr->move_to (playhead,21); cr->line_to (playhead,height-18); cr->stroke ();
 char position[80]; snprintf (position,sizeof(position),"%.1f / %.1f s",_position*_analysis->seconds,_analysis->seconds);
 text = create_pango_layout (position); color ("neutral:foreground",.75); cr->move_to (6,height-17); text->show_in_cairo_context (cr);
 return true;
}
void BrowserAudioPreview::seek (double x)
{
 _position = std::max (0., std::min (.999999, (x-4)/std::max (1,get_allocation ().get_width ()-8)));
 queue_draw ();
}
bool BrowserAudioPreview::on_button_press_event (GdkEventButton* event)
{
 if (event->button != 1 || !_analysis || _analysis->peaks.empty ()) return false;
 _dragging = true; seek (event->x); return true;
}
bool BrowserAudioPreview::on_motion_notify_event (GdkEventMotion* event)
{
 if (!_dragging) return false;
 seek (event->x); return true;
}
bool BrowserAudioPreview::on_button_release_event (GdkEventButton* event)
{
 if (event->button != 1 || !_dragging) return false;
 seek (event->x); _dragging = false; Seek (_position); return true;
}
