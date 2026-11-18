/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <memory>
#include <string>
#include <ytkmm/drawingarea.h>
#include "browser_audio_analysis.h"

class BrowserAudioPreview : public Gtk::DrawingArea {
public:
 BrowserAudioPreview ();
 ~BrowserAudioPreview () override;
 void set_file (std::string const&);
 void clear ();
 void set_normalized (bool);
 void set_position (double);
 double position () const { return _position; }
 std::string const& path () const { return _path; }
 std::shared_ptr<BrowserAudio::Analysis const> analysis () const { return _analysis; }
 sigc::signal<void> Ready;
 sigc::signal<void, double> Seek;
protected:
 bool on_expose_event (GdkEventExpose*) override;
 bool on_button_press_event (GdkEventButton*) override;
 bool on_button_release_event (GdkEventButton*) override;
 bool on_motion_notify_event (GdkEventMotion*) override;
private:
 struct Worker;
 std::unique_ptr<Worker> _worker;
 std::shared_ptr<BrowserAudio::Analysis const> _analysis;
 std::string _path, _signature;
 double _position = 0;
 bool _normalized = true, _dragging = false;
 sigc::connection _poll;
 bool poll ();
 void seek (double x);
};
