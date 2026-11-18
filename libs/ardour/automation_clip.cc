/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <algorithm>
#include <cmath>
#include <set>
#include "ardour/automation_clip.h"
#include "ardour/automation_clip_source.h"
#include "ardour/audio_track.h"
#include "ardour/audioregion.h"
#include "ardour/automation_control.h"
#include "ardour/automation_list.h"
#include "ardour/io.h"
#include "ardour/playlist.h"
#include "ardour/region_factory.h"
#include "ardour/session.h"
#include "ardour/record_safe_control.h"
#include "ardour/processor.h"
#include "ardour/gain_control.h"
#include "ardour/audioengine.h"
#include "temporal/tempo.h"

using namespace ARDOUR;
using namespace Temporal;

AutomationClips::AutomationClips (Session& s) : _session (s) {
 if (XMLNode* n = s.extra_xml ("AutomationClips")) { set_state (*n, PBD::Stateful::current_state_version); }
 else { refresh(); }
 _session.register_with_memento_command_factory(id(), this);
}
AutomationClips::~AutomationClips () { _connections.drop_connections(); }
bool AutomationClips::is_track (std::shared_ptr<Route> const& r) { return r && r->extra_xml ("AutomationClipTrack"); }
bool AutomationClips::is_clip (std::shared_ptr<AudioRegion> const& r) {
 return r && r->n_channels() && std::dynamic_pointer_cast<AutomationClipSource> (r->source (0));
}
ParameterDescriptor AutomationClips::descriptor () {
 ParameterDescriptor d; d.lower = 0; d.upper = 1; d.normal = .5; d.label = "Amount"; return d;
}
std::shared_ptr<AudioTrack> AutomationClips::new_track () {
 auto tracks = _session.new_audio_track (1, 1, {}, 1, "Automation", PresentationInfo::max_order, Normal, false);
 if (tracks.empty()) { return {}; }
 auto t = tracks.front();
 t->add_extra_xml (*new XMLNode ("AutomationClipTrack"));
 t->input()->disconnect (); t->output()->disconnect ();
 t->rec_safe_control()->set_value (1, PBD::Controllable::NoGroup);
 refresh(); _session.set_dirty(); return t;
}
std::shared_ptr<AudioRegion> AutomationClips::add_clip (std::shared_ptr<AudioTrack> t, timepos_t const& p, timecnt_t const& length) {
 if (!is_track (t) || length.samples() <= 0) { return {}; }
 auto source = std::make_shared<AutomationClipSource> (_session);
 _session.add_source (source);
 PBD::PropertyList props;
 props.add (Properties::start, timepos_t (0));
 props.add (Properties::length, length);
 props.add (Properties::name, std::string ("Automation"));
 auto r = std::dynamic_pointer_cast<AudioRegion> (RegionFactory::create (source, props));
 r->set_envelope_active (true);
 r->envelope()->set_descriptor (descriptor());
 r->envelope()->set_interpolation (Evoral::ControlList::Linear);
 r->envelope()->clear();
 r->envelope()->fast_simple_add (timepos_t (0), 0);
 r->envelope()->fast_simple_add (timepos_t (length.samples()), 1);
 t->playlist()->add_region (r, p);
 refresh(); return r;
}
bool AutomationClips::map (std::shared_ptr<Route> track, std::shared_ptr<AutomationControl> target, double lo, double hi, bool inv, std::string& error) {
 if (!is_track (track) || !target || !target->alist() || (target->flags() & PBD::Controllable::NotAutomatable)) { error = "Select an automation track and an automatable target."; return false; }
 auto const& d = target->alist()->descriptor();
 if (!std::isfinite(lo) || !std::isfinite(hi) || lo > hi || lo < d.lower || hi > d.upper) { error = "Range must be within the target's limits, with minimum below maximum."; return false; }
 auto id = target->id().to_s();
 auto i = std::find_if (_mappings.begin(), _mappings.end(), [&] (Mapping const& m) { return m.target == id; });
 if (i != _mappings.end()) {
  if (i->track != track->id().to_s()) { error = "This target belongs to another automation track."; return false; }
  i->minimum = lo; i->maximum = hi; i->inverted = inv;
 } else {
  if (!target->alist()->empty() || target->automation_state() != Off) { error = "This target already has automation. Choose an unused target or clear its automation first."; return false; }
  Mapping m; m.track = track->id().to_s(); m.target = id; m.minimum = lo; m.maximum = hi; m.inverted = inv;
  m.baseline = target->get_value(); m.original.reset (&target->alist()->get_state()); _mappings.push_back (m);
 }
 refresh(); persist(); return true;
}
std::shared_ptr<AutomationControl> AutomationClips::resolve (std::string const& id) const {
 PBD::ID key(id);
 for (auto r : *_session.get_routes()) {
  if (r->gain_control()->id() == key) { return r->gain_control(); }
  auto c = r->automation_control(key); if (c) { return c; }
  for (auto p : {r->pan_azimuth_control(), r->pan_width_control()}) { if (p && p->id() == key) { return p; } }
  r->foreach_processor ([&] (std::weak_ptr<Processor> wp) {
   auto a = std::dynamic_pointer_cast<Automatable>(wp.lock());
   if (a && !c) { c = a->automation_control(key); }
  });
  if (c) { return c; }
 }
 return {};
}
void AutomationClips::restore (Mapping const& m) {
 auto c = resolve (m.target);
 if (c && c->alist() && m.original) {
  c->alist()->set_state (*m.original, PBD::Stateful::current_state_version);
  c->set_automation_state (c->alist()->automation_state());
  c->set_value (m.baseline, PBD::Controllable::NoGroup);
 }
}
void AutomationClips::unmap (std::string const& id) {
 {
 PBD::Mutex::Lock process_lock (_session.engine().process_lock());
 _session.retire_anticipation_for_edit();
 for (auto i = _mappings.begin(); i != _mappings.end();) {
  if (i->target == id) { restore (*i); i = _mappings.erase(i); } else { ++i; }
 }
 }
 refresh(); persist();
}
void AutomationClips::persist () {
 _session.add_extra_xml (get_state()); _session.set_dirty();
}
XMLNode& AutomationClips::get_state () const {
 auto n = new XMLNode ("AutomationClips"); n->set_property ("id", id().to_s()); n->set_property ("version", 1);
 for (auto const& m : _mappings) {
  auto c = n->add_child ("Mapping"); c->set_property ("track", m.track); c->set_property ("target", m.target);
  c->set_property ("minimum", m.minimum); c->set_property ("maximum", m.maximum); c->set_property ("inverted", m.inverted);
  c->set_property ("baseline", m.baseline); if (m.original) { c->add_child_copy (*m.original); }
 }
 return *n;
}
int AutomationClips::set_state (XMLNode const& state, int) {
 /* Copy first: persist() replaces the Session's node, which may be our argument. */
 XMLNode n (state);
 set_id(n);
 {
 PBD::Mutex::Lock process_lock (_session.engine().process_lock());
 if (!_mappings.empty()) { _session.retire_anticipation_for_edit(); }
 for (auto const& m : _mappings) { restore(m); }
 }
 _mappings.clear();
 for (auto c : n.children()) {
  Mapping m;
  if (c->name() != "Mapping" || !c->get_property ("track", m.track) || !c->get_property ("target", m.target)) { continue; }
  c->get_property ("minimum", m.minimum); c->get_property ("maximum", m.maximum); c->get_property ("inverted", m.inverted); c->get_property ("baseline", m.baseline);
  if (auto o = c->child ("AutomationList")) { m.original.reset (new XMLNode (*o)); }
  if (m.original && std::isfinite(m.minimum) && std::isfinite(m.maximum) && m.minimum <= m.maximum) { _mappings.push_back (m); }
 }
 refresh(); persist(); return 0;
}
void AutomationClips::refresh () {
 if (_refreshing || _session.deletion_in_progress()) { return; }
 _refreshing = true;
 _connections.drop_connections();
 /* All of these signals originate in non-realtime timeline edits. Replacing a
  * list takes its native writer lock; playback takes its existing read lock. */
 auto changed = [this] { refresh(); };
 TempoMap::MapChanged.connect_same_thread (_connections, changed);
 for (auto const& route : *_session.get_routes()) {
  if (!is_track (route)) { continue; }
  auto track = std::dynamic_pointer_cast<AudioTrack> (route);
  if (!track) { continue; }
  track->PlaylistChanged.connect_same_thread (_connections, changed);
  track->DropReferences.connect_same_thread (_connections, [this, track_id = route->id().to_s()] {
   for (auto const& m : _mappings) { if (m.track == track_id) { restore(m); } }
  });
  auto playlist = track->playlist();
  playlist->ContentsChanged.connect_same_thread (_connections, changed);
  playlist->LayeringChanged.connect_same_thread (_connections, changed);
  playlist->foreach_region ([&] (std::shared_ptr<Region> r) {
   auto a = std::dynamic_pointer_cast<AudioRegion> (r);
   if (!is_clip(a)) { return; }
   a->envelope()->set_descriptor (descriptor());
   a->envelope()->StateChanged.connect_same_thread (_connections, changed);
   a->PropertyChanged.connect_same_thread (_connections, [changed] (PBD::PropertyChange const&) { changed(); });
  });
 }
 PBD::Mutex::Lock process_lock (_session.engine().process_lock());
 if (!_mappings.empty()) { _session.retire_anticipation_for_edit(); }
 for (auto const& m : _mappings) {
  auto track = std::dynamic_pointer_cast<AudioTrack> (_session.route_by_id (PBD::ID(m.track)));
  auto target = resolve (m.target);
  if (!track || !target || !target->alist()) { continue; }
  std::vector<std::shared_ptr<AudioRegion>> clips;
  std::set<samplepos_t> knots {0};
  track->playlist()->foreach_region ([&] (std::shared_ptr<Region> r) {
   auto a = std::dynamic_pointer_cast<AudioRegion> (r);
   if (!is_clip(a) || a->muted()) { return; }
   clips.push_back(a);
   auto start = a->position().samples(), end = start + a->length().samples();
   knots.insert(start); if (start > 0) { knots.insert(start - 1); }
   knots.insert(end); if (end > 0) { knots.insert(end - 1); }
   for (auto const& e : *a->envelope()) {
    auto t = start + e->when.samples(); if (t >= start && t < end) { knots.insert(t); if (t > start && a->envelope()->interpolation() == Evoral::ControlList::Discrete) { knots.insert(t-1); } }
   }
  });
  auto dest = target->alist();
  AutomationList compiled (dest->parameter(), dest->descriptor(), *dest);
  compiled.set_interpolation (Evoral::ControlList::Linear);
  auto evaluate = [&] (samplepos_t t) {
   std::shared_ptr<AudioRegion> top;
   for (auto a : clips) {
    if (t >= a->position().samples() && t < a->position().samples() + a->length().samples() && (!top || a->layer() >= top->layer())) { top = a; }
   }
   double value = m.baseline;
   if (top) {
    double amount = std::max (0., std::min (1., top->envelope()->eval (timepos_t(t - top->position().samples()))));
    if (m.inverted) { amount = 1 - amount; }
    value = m.minimum + amount * (m.maximum - m.minimum);
   }
   if (target->desc().toggled) { value = value >= .5 ? 1 : 0; }
   else if (target->desc().integer_step) { value = std::round(value); }
   return value;
  };
  auto append = [&] (samplepos_t t, double value) {
   timepos_t when(t); when.set_time_domain(dest->time_domain());
   compiled.fast_simple_add(when,value);
  };
  /* Linear curves need only their knots. Subdivide nonlinear curves and
   * stepped parameters until the output agrees to 0.01% of the target range. */
  std::function<void(samplepos_t,double,samplepos_t,double,int)> segment;
  segment = [&] (samplepos_t a, double va, samplepos_t b, double vb, int depth) {
   double error = 0;
   for (int i = 1; i < 4; ++i) {
    samplepos_t x = a + (b-a)*i/4;
    if (x > a && x < b) { error = std::max(error,std::abs(evaluate(x) - (va+(vb-va)*double(x-a)/double(b-a)))); }
   }
   if (b-a > 1 && depth < 24 && error > std::max(1e-9,(m.maximum-m.minimum)*.0001)) {
    samplepos_t mid = a+(b-a)/2; double vm = evaluate(mid);
    segment(a,va,mid,vm,depth+1); segment(mid,vm,b,vb,depth+1);
   } else { append(b,vb); }
  };
  auto previous = *knots.begin(); double last = evaluate(previous); append(previous,last);
  for (auto t : knots) {
   if (t == previous) { continue; }
   double v = evaluate(t); segment(previous,last,t,v,0); previous=t; last=v;
  }
  compiled.set_automation_state (dest->automation_state());
  *dest = compiled;
  target->set_automation_state (Play);
 }
 _refreshing = false;
}
