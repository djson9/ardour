/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "automation_clip_editor.h"
#include "ardour/audioregion.h"
#include "ardour/automation_list.h"
#include "ardour/session.h"
#include "gtkmm2ext/gui_thread.h"
#include "gtkmm2ext/keyboard.h"
#include "pbd/memento_command.h"
#include "temporal/tempo.h"
#include "pbd/i18n.h"
#include <ydkmm/general.h>
#include <ydkmm/cursor.h>

using namespace ARDOUR;
using namespace Temporal;
using CurvePoint = AutomationClipCurve::Point;

AutomationClipEditor::AutomationClipEditor (Session* s, std::shared_ptr<AudioRegion> r)
 : _session (s), _region (r)
{
 _help.set_text (_("Double-click to add a point · Drag points to move · Drag a line to bend · Delete removes a point · Esc cancels a drag"));
 _help.set_alignment (0, .5);
 pack_start (_help, false, false, 5);
 pack_start (_canvas, true, true);
 _canvas.set_name ("AutomationClipCanvas");
 _canvas.set_can_focus (true);
 _canvas.set_size_request (320, 180);
 _canvas.add_events (Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK | Gdk::KEY_PRESS_MASK);
 _canvas.signal_expose_event ().connect (sigc::mem_fun (*this, &AutomationClipEditor::expose));
 _canvas.signal_button_press_event ().connect (sigc::mem_fun (*this, &AutomationClipEditor::press));
 _canvas.signal_button_release_event ().connect (sigc::mem_fun (*this, &AutomationClipEditor::release));
 _canvas.signal_motion_notify_event ().connect (sigc::mem_fun (*this, &AutomationClipEditor::motion));
 _canvas.signal_key_press_event ().connect (sigc::mem_fun (*this, &AutomationClipEditor::key));
 _region->envelope ()->StateChanged.connect (_connections, invalidator (*this), std::bind (&AutomationClipEditor::redraw, this), gui_context ());
 _region->PropertyChanged.connect (_connections, invalidator (*this), [this] (PBD::PropertyChange const&) { redraw (); }, gui_context ());
 load ();
 show_all ();
}
void AutomationClipEditor::redraw () { _canvas.queue_draw (); }
void AutomationClipEditor::load () {
 _points.clear ();
 if (auto* state = _region->extra_xml ("AutomationClipCurve")) {
  int64_t source_start = -1, length = -1;
  /* Native trim/split/stretch operations transform the playback envelope.
   * Stale sparse handles must never replace that transformed automation. */
  const bool current = state->get_property ("source-start", source_start) && state->get_property ("length", length) &&
      source_start == _region->start ().samples () && length == _region->length ().samples ();
  if (current) for (auto* n : state->children ()) {
   CurvePoint p {0, 0, 0};
   if (n->name () == "Point" && n->get_property ("when", p.when) && n->get_property ("value", p.value) && n->get_property ("bend", p.bend) &&
       p.when >= 0 && std::isfinite (p.value) && std::isfinite (p.bend) && p.value >= 0 && p.value <= 1 && std::abs (p.bend) <= 1 &&
       (_points.empty () || p.when > _points.back ().when)) _points.push_back (p);
  }
 }
 if (_points.size () < 2) {
  _points.clear ();
  for (auto const& e : *_region->envelope ()) _points.push_back ({e->when.samples (), e->value, 0});
 }
 if (_points.empty ()) _points.push_back ({0, .5, 0});
 if (_points.size () == 1) _points.push_back ({std::max<int64_t> (1, _region->length ().samples ()), _points.front ().value, 0});
 if (_selected >= int (_points.size ())) _selected = -1;
}
double AutomationClipEditor::px (int64_t t) const { return 36 + double (t) / std::max<int64_t> (1, _region->length ().samples ()) * std::max (1, _canvas.get_width () - 52); }
double AutomationClipEditor::py (double v) const { return 30 + (1-v) * std::max (1, _canvas.get_height () - 46); }
int64_t AutomationClipEditor::when (double x) const { return std::llround (std::max (0.0, std::min (1.0, (x-36) / std::max (1, _canvas.get_width ()-52))) * _region->length ().samples ()); }
double AutomationClipEditor::value (double y) const { return std::max (0.0, std::min (1.0, 1 - (y-30) / std::max (1, _canvas.get_height ()-46))); }
int AutomationClipEditor::hit_point (double x, double y) const {
 for (size_t i=0; i<_points.size (); ++i) if (std::hypot (x-px (_points[i].when), y-py (_points[i].value)) <= 8) return i;
 return -1;
}
int AutomationClipEditor::hit_segment (double x, double y) const {
 const auto t = when (x);
 for (size_t i=1; i<_points.size (); ++i) if (t > _points[i-1].when && t < _points[i].when &&
   std::abs (y-py (AutomationClipCurve::evaluate (_points[i-1], _points[i], t))) <= 8) return i-1;
 return -1;
}
bool AutomationClipEditor::expose (GdkEventExpose*) {
 if (!_dragging) load ();
 auto c = _canvas.get_window ()->create_cairo_context ();
 const double w = _canvas.get_width (), h = _canvas.get_height ();
 c->set_source_rgb (.105, .125, .14); c->paint ();
 c->set_line_width (1);
 for (int v=0; v<=4; ++v) {
  const double y = py (v/4.0);
  c->set_source_rgba (.6,.65,.7,.12); c->move_to (36,y); c->line_to (w-16,y); c->stroke ();
  c->set_source_rgb (.6,.65,.7); c->set_font_size (10); c->move_to (2,y+4); c->show_text (std::to_string (v*25));
 }
 auto map = TempoMap::use ();
 const auto start = _region->position ().samples ();
 const auto length = _region->length ().samples ();
 const double first = std::ceil (_region->position ().beats ().to_ticks () / double (Temporal::ticks_per_beat));
 const double last = (_region->position () + _region->length ()).beats ().to_ticks () / double (Temporal::ticks_per_beat);
 const int step = std::max (1, int (std::ceil ((last-first) / std::max (1.0, (w-52)/45))));
 for (double beat=first; beat<=last; beat+=step) {
  const Beats b = Beats::from_double (beat);
  const auto t = map->sample_at (b) - start;
  if (t<0 || t>length) continue;
  const double x = px (t); const auto bbt = map->bbt_at (b);
  c->set_source_rgba (.6,.65,.7,bbt.beats==1 ? .32 : .12); c->move_to (x,24); c->line_to (x,h-16); c->stroke ();
  c->set_source_rgb (.7,.74,.78); c->move_to (x+3,16); c->show_text (std::to_string (bbt.bars) + "." + std::to_string (bbt.beats));
 }
 c->save (); c->rectangle (35,25,w-50,h-39); c->clip ();
 c->set_source_rgb (1,.65,.18); c->set_line_width (2);
 for (size_t i=1; i<_points.size (); ++i) {
  const auto& a = _points[i-1]; const auto& b = _points[i];
  c->move_to (px (a.when),py (a.value));
  const int n = std::max (2,std::min (2048,int (std::abs (px (b.when)-px (a.when)))));
  for (int j=1;j<=n;++j) { const double t=a.when+(b.when-a.when)*double(j)/n; c->line_to (px (std::llround (t)),py (AutomationClipCurve::evaluate (a,b,t))); }
  c->stroke ();
 }
 for (size_t i=0;i<_points.size ();++i) {
  c->arc (px (_points[i].when),py (_points[i].value),i==size_t (_selected)?5:4,0,2*M_PI);
  c->set_source_rgb (.105,.125,.14); c->fill_preserve (); c->set_source_rgb (1,.65,.18); c->stroke ();
 }
 c->restore ();
 return true;
}
bool AutomationClipEditor::press (GdkEventButton* e) {
 if (e->button != 1) return false;
 _canvas.grab_focus (); _dragging = false; load (); _selected = hit_point (e->x,e->y); _segment = -1;
 if (e->type == GDK_2BUTTON_PRESS && _selected < 0) {
  const int64_t t = when (e->x);
  auto i = std::lower_bound (_points.begin (), _points.end (), t, [] (CurvePoint const& p, int64_t x) { return p.when < x; });
  if (i != _points.end () && i->when == t) return true;
  _selected = i-_points.begin (); _points.insert (i, {t,value (e->y),0}); commit (); redraw (); return true;
 }
 if (_selected < 0) _segment = hit_segment (e->x,e->y);
 _dragging = _selected >= 0 || _segment >= 0;
 _before = _points; _start_y = e->y;
 if (_segment >= 0) _start_bend = _points[_segment].bend;
 redraw (); return true;
}
bool AutomationClipEditor::motion (GdkEventMotion* e) {
 if (!_dragging) {
  const bool over = hit_point (e->x,e->y)>=0 || hit_segment (e->x,e->y)>=0;
  _canvas.get_window ()->set_cursor (Gdk::Cursor (over ? Gdk::FLEUR : Gdk::LEFT_PTR));
  return true;
 }
 if (_segment >= 0) {
  const double sign = _points[_segment+1].value >= _points[_segment].value ? 1 : -1;
  _points[_segment].bend = std::max (-1.0,std::min (1.0,_start_bend + sign*(_start_y-e->y)/std::max (1,_canvas.get_height ()-46)*3));
 } else {
  auto& p = _points[_selected]; p.value = value (e->y);
  if (_selected > 0 && _selected+1 < int (_points.size ())) p.when = std::max (_points[_selected-1].when+1,std::min (_points[_selected+1].when-1,when (e->x)));
 }
 redraw (); return true;
}
bool AutomationClipEditor::release (GdkEventButton*) {
 if (_dragging) { _dragging=false; if (_points != _before) commit (); }
 redraw (); return true;
}
bool AutomationClipEditor::key (GdkEventKey* e) {
 if (e->keyval == GDK_Escape) { _dragging=false; load (); redraw (); return true; }
 if (e->keyval != GDK_Delete && e->keyval != GDK_BackSpace) return false;
 if (_selected > 0 && _selected+1 < int (_points.size ())) { _points.erase (_points.begin ()+_selected); _selected=-1; commit (); redraw (); }
 return true;
}
void AutomationClipEditor::commit () {
 auto* before = &_region->get_state ();
 if (!before->child ("Extra")) before->add_child ("Extra");
 auto* state = new XMLNode ("AutomationClipCurve"); state->set_property ("version",1);
 state->set_property ("source-start", _region->start ().samples ());
 state->set_property ("length", _region->length ().samples ());
 for (auto const& p : _points) { auto* n=state->add_child ("Point"); n->set_property ("when",p.when); n->set_property ("value",p.value); n->set_property ("bend",p.bend); }
 _session->begin_reversible_command (_("Edit automation clip curve"));
 _region->add_extra_xml (*state);
 auto env = _region->envelope (); env->freeze (); env->clear (); env->set_interpolation (Evoral::ControlList::Linear);
 for (auto const& p : AutomationClipCurve::compile (_points)) env->fast_simple_add (timepos_t (p.when),p.value);
 env->thaw ();
 _session->add_command (new MementoCommand<AudioRegion> (*_region,before,&_region->get_state ()));
 _session->commit_reversible_command (); _session->set_dirty ();
}
