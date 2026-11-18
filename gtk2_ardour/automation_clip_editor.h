/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <memory>
#include <ytkmm/box.h>
#include <ytkmm/drawingarea.h>
#include <ytkmm/label.h>
#include "pbd/signals.h"
#include "automation_clip_curve.h"
namespace ARDOUR { class AudioRegion; class Session; }
class XMLNode;

class AutomationClipEditor : public Gtk::VBox {
public:
 AutomationClipEditor (ARDOUR::Session*, std::shared_ptr<ARDOUR::AudioRegion>);
 std::shared_ptr<ARDOUR::AudioRegion> region () const { return _region; }
private:
 ARDOUR::Session* _session;
 std::shared_ptr<ARDOUR::AudioRegion> _region;
 Gtk::Label _help;
 Gtk::DrawingArea _canvas;
 std::vector<AutomationClipCurve::Point> _points, _before;
 PBD::ScopedConnectionList _connections;
 int _selected = -1, _segment = -1;
 bool _dragging = false;
 double _start_y = 0, _start_bend = 0;
 void load ();
 void commit ();
 void redraw ();
 double px (int64_t) const;
 double py (double) const;
 int64_t when (double) const;
 double value (double) const;
 int hit_point (double, double) const;
 int hit_segment (double, double) const;
 bool expose (GdkEventExpose*);
 bool press (GdkEventButton*);
 bool motion (GdkEventMotion*);
 bool release (GdkEventButton*);
 bool key (GdkEventKey*);
};
