#pragma once

#include <cmath>
#include <string>
#include <glibmm/miscutils.h>
#include <glibmm/fileutils.h>
#include "pbd/xml++.h"
#include "ardour/region.h"

/* Per-region settings. Rendered audio retains original master sources, so
 * changing these values never processes an already processed waveform. */
struct ClipWarpSettings {
 bool enabled = false;
 bool reverse = false;
 std::string mode = "complex-pro";
 double bpm = 120.;
 double pitch = 0.;
 double formants = 100.;
 int envelope = 128;
 double grain = 65.;
 double flux = 25.;
 int preserve = 0;
 int loop = 1;
 double transient_envelope = 100.;

 static ClipWarpSettings read (ARDOUR::Region& r, double tempo = 120.) {
  ClipWarpSettings s;
  s.bpm = tempo * r.stretch ();
  s.pitch = 12. * std::log2 (r.shift ());
  XMLNode* n = r.extra_xml ("ClipWarp");
  if (!n) return s;
  n->get_property ("enabled", s.enabled); n->get_property ("reverse", s.reverse);
  n->get_property ("mode", s.mode); n->get_property ("bpm", s.bpm); n->get_property ("pitch", s.pitch);
  n->get_property ("formants", s.formants); n->get_property ("envelope", s.envelope);
  n->get_property ("grain", s.grain); n->get_property ("flux", s.flux);
  n->get_property ("preserve", s.preserve); n->get_property ("loop", s.loop);
  n->get_property ("transient-envelope", s.transient_envelope);
  return s;
 }
 void write (ARDOUR::Region& r) const {
  XMLNode* n = new XMLNode ("ClipWarp");
  n->set_property ("version", 1); n->set_property ("enabled", enabled); n->set_property ("reverse", reverse);
  n->set_property ("mode", mode); n->set_property ("bpm", bpm); n->set_property ("pitch", pitch);
  n->set_property ("formants", formants); n->set_property ("envelope", envelope);
  n->set_property ("grain", grain); n->set_property ("flux", flux);
  n->set_property ("preserve", preserve); n->set_property ("loop", loop);
  n->set_property ("transient-envelope", transient_envelope);
  r.add_extra_xml (*n);
 }
 static std::string engine_directory () {
  const char* p = g_getenv ("ARDOUR_WARP_ENGINE_DIR");
  if (p && *p) return p;
#ifdef __APPLE__
  return Glib::build_filename (Glib::get_home_dir (), "Library/Application Support/Ardour9/WarpEngines");
#else
  return Glib::build_filename (Glib::get_user_data_dir (), "ardour9/warp-engines");
#endif
 }
 bool available () const {
  return Glib::file_test (Glib::build_filename (engine_directory (), mode + ".ready"), Glib::FILE_TEST_IS_REGULAR);
 }
};
