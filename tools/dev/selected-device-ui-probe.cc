/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Inject only into a disposable Verification GUI, then create the trigger file. */
#include <fstream>
#include <ytkmm/container.h>
#include <ytkmm/label.h>
#include <ytkmm/window.h>
#include "ardour/audioengine.h"
#include "ardour/audio_rack.h"
#include "ardour/plugin_insert.h"
#include "ardour/plugin_manager.h"
#include "ardour/route.h"
#include "ardour/selection.h"
#include "ardour/session.h"
#include "../../gtk2_ardour/audio_rack_ui.h"
#include "../../gtk2_ardour/plugin_ui.h"
#include "../../gtk2_ardour/processor_box.h"
#include "../../gtk2_ardour/route_properties_box.h"

using namespace ARDOUR;
using List = Gtkmm2ext::DnDVBox<ProcessorEntry>;
template<class T> static T* find (Gtk::Widget* w) {
 if (auto* found = dynamic_cast<T*> (w)) return found;
 if (auto* c = dynamic_cast<Gtk::Container*> (w)) for (auto* child : c->get_children ()) if (auto* found = find<T> (child)) return found;
 return nullptr;
}
template<class T> static int count (Gtk::Widget* w) {
 int n = dynamic_cast<T*> (w) ? 1 : 0;
 if (auto* c = dynamic_cast<Gtk::Container*> (w)) for (auto* child : c->get_children ()) n += count<T> (child);
 return n;
}
static bool label (Gtk::Widget* w, std::string const& text) {
 if (auto* l = dynamic_cast<Gtk::Label*> (w)) if (l->get_text () == text) return true;
 if (auto* c = dynamic_cast<Gtk::Container*> (w)) for (auto* child : c->get_children ()) if (label (child, text)) return true;
 return false;
}
static ProcessorEntry* entry (List* list, std::shared_ptr<Processor> p) {
 for (auto* e : list->children ()) if (e->processor () == p) return e;
 return nullptr;
}
static void click (Gtk::Widget& w) {
 for (auto type : {GDK_BUTTON_PRESS, GDK_BUTTON_RELEASE}) {
  GdkEvent* e = gdk_event_new (type);
  e->button.window = GDK_WINDOW (g_object_ref (w.get_window ()->gobj ()));
  e->button.send_event = true; e->button.button = 1; e->button.x = 8; e->button.y = 8;
  gtk_widget_event (w.gobj (), e); gdk_event_free (e);
 }
}
static gboolean tick (gpointer) {
 const char* output = getenv ("ARDOUR_DEVICE_UI_PROBE"); if (!output) return FALSE;
 static int stage = 0;
 static gint64 started = 0;
 static std::shared_ptr<Route> route;
 static std::shared_ptr<PluginInsert> eq, delay;
 static std::shared_ptr<AudioRack> rack;
 if (!started && !g_file_test ((std::string (output) + ".trigger").c_str (), G_FILE_TEST_EXISTS)) return TRUE;
 if (!started) started = g_get_monotonic_time ();
 auto finish = [&] (const char* error) {
  std::ofstream (output) << "{\"passed\":" << (error ? "false" : "true") << ",\"stage\":" << stage
   << ",\"seconds\":" << (g_get_monotonic_time () - started) / 1e6 << ",\"error\":\"" << (error ? error : "") << "\"}";
  return FALSE;
 };
 if (g_get_monotonic_time () - started > 9000000) return finish ("UI deadline");
 try {
  auto* session = AudioEngine::instance ()->session (); if (!session) return TRUE;
  if (session->name ().find ("Verification") == std::string::npos) return finish ("Use a disposable Verification session");
  if (stage == 0) {
   for (auto const& r : *session->get_routes ()) if (r->name () == "Audio") route = r;
   if (!route) return finish ("Fixture needs an Audio track");
   auto plugin = [&] (std::string const& name, std::string const& instance) {
    for (auto const& info : PluginManager::instance ().lv2_plugin_info ()) if (info->name == name) {
     auto p = std::make_shared<PluginInsert> (*session, *route, info->load (*session)); p->set_name (instance); return p;
    }
    throw std::runtime_error ("Missing built-in plugin");
   };
   eq = plugin ("ACE EQ", "Detail EQ"); delay = plugin ("ACE Delay", "Detail Delay");
   rack = std::make_shared<AudioRack> (*session, *route); rack->set_name ("Detail Rack");
   if (route->add_processor (eq, PreFader) || route->add_processor (delay, PreFader) || route->add_processor (rack, PreFader)) return finish ("Fixture insertion");
   session->selection ().select_stripable_and_maybe_group (route, SelectionSet, false);
   stage = 1; return TRUE;
  }
  RoutePropertiesBox* props = nullptr;
  for (auto* w : Gtk::Window::list_toplevels ()) if (w->get_visible ()) if (auto* p = find<RoutePropertiesBox> (w)) if (p->get_mapped ()) props = p;
  if (!props) return TRUE;
  auto* list = find<List> (props); if (!list || (stage != 9 && !entry (list, eq))) return TRUE;
  static gint64 last_report = 0;
  if (g_get_monotonic_time () - last_report > 1000000) {
   g_printerr ("Device UI probe stage %d: generic=%d rack=%d selected=%s\n", stage, count<GenericPluginUI> (props), count<AudioRackUI> (props), list->selection ().empty () ? "none" : list->selection ().back ()->processor ()->name ().c_str ());
   last_report = g_get_monotonic_time ();
  }
  auto single = [&] (std::string const& name) { return count<GenericPluginUI> (props) == 1 && count<AudioRackUI> (props) == 0 && label (props, name); };
  if (stage == 1) { click (entry (list, eq)->action_widget ()); stage = 2; return TRUE; }
  if (stage == 2) {
   if (!single ("Detail EQ") || label (props, "Detail Delay") || label (props, "Detail Rack")) return TRUE;
   list->select (entry (list, delay)); stage = 3; return TRUE;
  }
  if (stage == 3) {
   if (!single ("Detail Delay") || label (props, "Detail EQ")) return TRUE;
   GdkEvent* e = gdk_event_new (GDK_LEAVE_NOTIFY); e->crossing.window = GDK_WINDOW (g_object_ref (list->get_window ()->gobj ()));
   e->crossing.mode = GDK_CROSSING_NORMAL; e->crossing.detail = GDK_NOTIFY_ANCESTOR;
   gtk_widget_event (GTK_WIDGET (list->gobj ()), e); gdk_event_free (e); stage = 4; return TRUE;
  }
  if (stage == 4) {
   if (list->selection ().size () != 1 || list->selection ().front ()->processor () != delay || !single ("Detail Delay")) return finish ("Pointer leave lost selection/details");
   list->select (entry (list, rack)); stage = 5; return TRUE;
  }
  if (stage == 5) {
   if (count<AudioRackUI> (props) != 1 || count<GenericPluginUI> (props) != 0) return TRUE;
   list->select (nullptr); stage = 6; return TRUE;
  }
  if (stage == 6) {
   if (count<AudioRackUI> (props) || count<GenericPluginUI> (props)) return TRUE;
   list->select (entry (list, rack)); stage = 7; return TRUE;
  }
  if (stage == 7) {
   if (count<AudioRackUI> (props) != 1) return TRUE;
   route->remove_processor (rack); stage = 8; return TRUE;
  }
  if (stage == 8) {
   if (!single ("Detail EQ") || count<AudioRackUI> (props)) return TRUE;
   session->selection ().select_stripable_and_maybe_group (session->master_out (), SelectionSet, false); stage = 9; return TRUE;
  }
  if (stage == 9) {
   if (count<GenericPluginUI> (props) || count<AudioRackUI> (props)) return TRUE;
   session->selection ().select_stripable_and_maybe_group (route, SelectionSet, false); stage = 10; return TRUE;
  }
  if (!single ("Detail EQ")) return TRUE;
  route->remove_processor (eq); route->remove_processor (delay);
  return finish (nullptr);
 } catch (std::exception const& e) { g_printerr ("Device UI probe: %s\n", e.what ()); return finish ("Exception"); }
}
__attribute__((constructor)) static void initialize () { if (getenv ("ARDOUR_DEVICE_UI_PROBE")) g_timeout_add (40, tick, nullptr); }
