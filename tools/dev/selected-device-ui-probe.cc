/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Inject only into a disposable Verification GUI, then create the trigger file. */
#include <fstream>
#include <type_traits>
#include <execinfo.h>
#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#endif
#include <ytkmm/container.h>
#include <ytkmm/label.h>
#include <ytkmm/window.h>
#include <ytkmm/checkmenuitem.h>
#include <ytkmm/menu.h>
#ifdef __APPLE__
extern "C" NSView* gdk_quartz_window_get_nsview (GdkWindow*);
#endif
#include "ardour/amp.h"
#include "ardour/automation_clip.h"
#include "ardour/audio_track.h"
#include "ardour/gain_control.h"
#include "ardour/midi_track.h"
#include "ardour/midi_source.h"
#include "ardour/playlist.h"
#include "ardour/region.h"
#include "ardour/region_factory.h"
#include "ardour/audioengine.h"
#include "ardour/audio_rack.h"
#include "ardour/plugin_insert.h"
#include "ardour/plugin_manager.h"
#include "ardour/route.h"
#include "ardour/selection.h"
#include "ardour/session.h"
#include "../../gtk2_ardour/audio_rack_ui.h"
#include "../../gtk2_ardour/ardour_ui.h"
#include "../../gtk2_ardour/actions.h"
#include "../../gtk2_ardour/plugin_ui.h"
#include "../../gtk2_ardour/gain_meter.h"
#include "../../gtk2_ardour/pianoroll.h"
#include "../../gtk2_ardour/automation_clip_editor.h"
#include "../../gtk2_ardour/region_view.h"
#include "../../gtk2_ardour/selection.h"
#include "../../gtk2_ardour/processor_box.h"
#include "../../gtk2_ardour/route_properties_box.h"
#include "../../gtk2_ardour/public_editor.h"
#include "../../gtk2_ardour/time_axis_view.h"

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
static Gtk::Widget* named (Gtk::Widget* w, std::string const& name) {
 if (w->get_name () == name) return w;
 if (auto* c = dynamic_cast<Gtk::Container*> (w)) for (auto* child : c->get_children ()) if (auto* found = named (child, name)) return found;
 return nullptr;
}
static Gtk::Widget* piano_canvas () {
 for (auto* w : Gtk::Window::list_toplevels ()) if (w->get_visible ()) if (auto* p = named (w, "MidiCueCanvas")) if (p->get_mapped ()) return p;
 return nullptr;
}
static ProcessorEntry* entry (List* list, std::shared_ptr<Processor> p) {
 for (auto* e : list->children ()) if (e->processor () == p) return e;
 return nullptr;
}
static void click (Gtk::Widget& w, guint button = 1) {
 for (auto type : {GDK_BUTTON_PRESS, GDK_BUTTON_RELEASE}) {
  GdkEvent* e = gdk_event_new (type);
  e->button.window = GDK_WINDOW (g_object_ref (w.get_window ()->gobj ()));
  e->button.send_event = true; e->button.button = button; e->button.x = 8; e->button.y = 8;
  gtk_widget_event (w.gobj (), e); gdk_event_free (e);
 }
}
static Gtk::Widget* header (Gtk::Widget* w, TimeAxisView* view) {
 if (w->get_data ("ardour-track-header") == view) return w;
 if (auto* c = dynamic_cast<Gtk::Container*> (w)) for (auto* child : c->get_children ()) if (auto* h = header (child, view)) return h;
 return nullptr;
}
static double title_brightness (Gtk::Widget* w, std::string const& title) {
 if (auto* l = dynamic_cast<Gtk::Label*> (w)) if (l->get_text () == title) {
  Gtk::Widget* box = l->get_parent ();
  while (box && !dynamic_cast<Gtk::EventBox*> (box)) box = box->get_parent ();
  if (!box) return -1;
  auto c = box->get_style ()->get_bg (Gtk::STATE_NORMAL);
  return c.get_red_p () + c.get_green_p () + c.get_blue_p ();
 }
 if (auto* container = dynamic_cast<Gtk::Container*> (w)) for (auto* child : container->get_children ()) {
  double value = title_brightness (child, title); if (value >= 0) return value;
 }
 return -1;
}
static void snapshot (Gtk::Widget* w, std::string const& suffix) {
 if (!w) throw std::runtime_error ("Missing snapshot widget");
#ifdef __APPLE__
 /* GTK2's pixmap snapshot has no Quartz backing store. Cache the native view
  * instead, which draws the real widget tree without screen-recording access. */
 auto* toplevel = w->get_toplevel ();
 NSView* view = gdk_quartz_window_get_nsview (toplevel->get_window ()->gobj ());
 gint x = 0, y = 0;
 gtk_widget_translate_coordinates (w->gobj (), GTK_WIDGET (toplevel->gobj ()), 0, 0, &x, &y);
 NSRect rect = NSMakeRect (x, y, w->get_width (), w->get_height ());
 NSBitmapImageRep* pixels = [view bitmapImageRepForCachingDisplayInRect:rect];
 [view cacheDisplayInRect:rect toBitmapImageRep:pixels];
 NSData* png = [pixels representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
 std::string path = std::string (getenv ("ARDOUR_DEVICE_UI_PROBE")) + suffix + ".png";
 if (![png writeToFile:[NSString stringWithUTF8String:path.c_str ()] atomically:true]) throw std::runtime_error ("Native snapshot save failed");
#else
 GdkPixmap* pixmap = gtk_widget_get_snapshot (w->gobj (), nullptr);
 if (!pixmap) throw std::runtime_error ("Widget snapshot failed");
 gint width, height; gdk_drawable_get_size (pixmap, &width, &height);
 auto* pixels = gdk_pixbuf_get_from_drawable (nullptr, pixmap, gdk_drawable_get_colormap (pixmap), 0, 0, 0, 0, width, height);
 bool saved = pixels && gdk_pixbuf_save (pixels, (std::string (getenv ("ARDOUR_DEVICE_UI_PROBE")) + suffix + ".png").c_str (), "png", nullptr, nullptr);
 if (pixels) g_object_unref (pixels);
 g_object_unref (pixmap);
 if (!saved) throw std::runtime_error ("Snapshot save failed");
#endif
}
static std::vector<std::shared_ptr<Processor>> visible_order (std::shared_ptr<Route> route) {
 std::vector<std::shared_ptr<Processor>> order;
 route->foreach_processor ([&] (std::weak_ptr<Processor> w) { auto p = w.lock (); if (p && p->display_to_user ()) order.push_back (p); });
 return order;
}
static void key (Gtk::Widget& widget, guint keyval, guint state = 0) {
 auto* window = dynamic_cast<Gtk::Window*> (widget.get_toplevel ());
 GdkEvent* event = gdk_event_new (GDK_KEY_PRESS);
 event->key.window = GDK_WINDOW (g_object_ref (window->get_window ()->gobj ()));
 event->key.send_event = true; event->key.time = GDK_CURRENT_TIME;
 event->key.keyval = keyval; event->key.state = state;
 GdkKeymapKey* keys = nullptr; gint n_keys = 0;
 gdk_keymap_get_entries_for_keyval (gdk_keymap_get_default (), keyval, &keys, &n_keys);
 if (n_keys) { event->key.hardware_keycode = keys[0].keycode; event->key.group = keys[0].group; }
 g_free (keys);
 gchar text[7] = {}; gunichar character = gdk_keyval_to_unicode (keyval);
 event->key.length = character ? g_unichar_to_utf8 (character, text) : 0;
 event->key.string = g_strdup (text);
 gtk_widget_event (GTK_WIDGET (window->gobj ()), event);
 gdk_event_free (event);
}
static gboolean tick (gpointer) {
 const char* output = getenv ("ARDOUR_DEVICE_UI_PROBE"); if (!output) return FALSE;
 static int stage = 0;
 static gint64 started = 0;
 static std::shared_ptr<Route> route;
 static std::shared_ptr<Route> group;
 static std::shared_ptr<PluginInsert> eq, delay;
 static std::shared_ptr<AudioRack> rack;
 static std::shared_ptr<MidiTrack> midi_track;
 static std::shared_ptr<Region> midi_a, midi_b;
 static std::shared_ptr<AudioTrack> automation_track;
 static std::shared_ptr<Region> automation_region;
 static double active_title_brightness = 0;
 if (!started && !g_file_test ((std::string (output) + ".trigger").c_str (), G_FILE_TEST_EXISTS)) return TRUE;
 if (!started) started = g_get_monotonic_time ();
 auto finish = [&] (const char* error) {
  std::ofstream (output) << "{\"passed\":" << (error ? "false" : "true") << ",\"stage\":" << stage
   << ",\"seconds\":" << (g_get_monotonic_time () - started) / 1e6 << ",\"error\":\"" << (error ? error : "") << "\"}";
  return FALSE;
 };
 if (g_get_monotonic_time () - started > 15000000) return finish ("UI deadline");
 try {
  auto* session = AudioEngine::instance ()->session (); if (!session) return TRUE;
  if (session->name ().find ("Verification") == std::string::npos) return finish ("Use a disposable Verification session");
  if (stage >= 30) {
   auto& editor = PublicEditor::instance ();
   auto range = [&] (std::shared_ptr<Route> track, timepos_t at) {
    session->selection ().select_stripable_and_maybe_group (track, SelectionSet, false);
    editor.get_selection ().set (at, at + timecnt_t (Temporal::Beats (1, 0), at));
    auto* view = editor.time_axis_view_from_stripable (track);
    if (!view) throw std::runtime_error ("Missing clip track view");
    view->focus_track_header ();
   };
   auto header_key = [&] (std::shared_ptr<Route> track, guint keyval) {
    auto* view = editor.time_axis_view_from_stripable (track);
    for (auto* window : Gtk::Window::list_toplevels ()) if (auto* h = header (window, view)) { key (*h, keyval); return; }
    throw std::runtime_error ("Missing header for Enter");
   };
   auto panel = [&] (auto type) {
    using T = decltype (type); T result = nullptr;
    for (auto* window : Gtk::Window::list_toplevels ()) if (window->get_visible ()) if (auto* widget = find<typename std::remove_pointer<T>::type> (window)) if (widget->get_mapped ()) result = widget;
    return result;
   };
   if (stage == 30) {
    for (auto const& r : *session->get_routes ()) if ((midi_track = std::dynamic_pointer_cast<MidiTrack> (r))) break;
    if (!midi_track) return finish ("Fixture needs a MIDI track");
    auto source = session->create_midi_source_for_session ("UI Enter MIDI");
    PBD::PropertyList properties;
    properties.add (Properties::start, timepos_t (Temporal::Beats (0, 0)));
    properties.add (Properties::length, timecnt_t (Temporal::Beats (4, 0)));
    midi_a = RegionFactory::create (source, properties); midi_b = RegionFactory::create (source, properties);
    midi_track->playlist ()->add_region (midi_a, timepos_t (Temporal::Beats (4, 0)));
    midi_track->playlist ()->add_region (midi_b, timepos_t (Temporal::Beats (8, 0)));
    stage = 31; return TRUE;
   }
   if (stage == 31) {
    range (midi_track, timepos_t (Temporal::Beats (9, 0)));
    header_key (midi_track, GDK_Return);
    stage = 32; return TRUE;
   }
   if (stage == 32 || stage == 33 || stage == 34) {
    auto* piano = piano_canvas ();
    if (!piano || editor.get_selection ().regions.size () != 1 || editor.get_selection ().regions.front ()->region () != midi_b) return finish ("Enter did not open the MIDI clip at the selection marker");
    if (stage == 32) {
     snapshot (piano, ".midi");
     auto* view = editor.time_axis_view_from_stripable (midi_track); view->focus_track_header ();
     key (*piano, GDK_KP_Enter); stage = 33; return TRUE;
    }
    range (midi_track, timepos_t (Temporal::Beats (stage == 33 ? 8 : 13, 0)));
    header_key (midi_track, GDK_Return);
    stage = stage == 33 ? 34 : 35; return TRUE;
   }
   if (stage == 35) {
    if (!editor.get_selection ().regions.empty () || piano_canvas ()) return finish ("Enter in a gap selected a clip elsewhere");
    ActionManager::get_action ("Editor", "add-automation-track")->activate (); stage = 36; return TRUE;
   }
   if (stage == 36) {
    for (auto const& r : *session->get_routes ()) if (AutomationClips::is_track (r)) automation_track = std::dynamic_pointer_cast<AudioTrack> (r);
    if (!automation_track || !editor.time_axis_view_from_stripable (automation_track)) return TRUE;
    Gtk::Widget* h = nullptr;
    for (auto* w : Gtk::Window::list_toplevels ()) if ((h = header (w, editor.time_axis_view_from_stripable (automation_track)))) break;
    if (!h || !h->get_mapped ()) return TRUE;
    automation_track->playlist ()->foreach_region ([&] (std::shared_ptr<Region> r) { automation_region = r; });
    if (!automation_region) return finish ("Missing automation clip fixture");
    auto at = automation_region->position ();
    range (automation_track, at + timecnt_t (Temporal::Beats (1, 0), at));
    header_key (automation_track, GDK_Return);
    stage = 37; return TRUE;
   }
   if (stage == 37) {
    auto* curve = panel (static_cast<AutomationClipEditor*> (nullptr));
    if (!curve || curve->region () != automation_region || editor.get_selection ().regions.size () != 1 || editor.get_selection ().regions.front ()->region () != automation_region) return finish ("Enter did not open the automation clip curve");
    snapshot (curve, ".automation");
    midi_track->playlist ()->remove_region (midi_a); midi_track->playlist ()->remove_region (midi_b);
    session->remove_route (automation_track);
    return finish (nullptr);
   }
  }
  if (stage == 0) {
   for (auto const& r : *session->get_routes ()) if (r->name () == "Audio") route = r;
   if (!route) return finish ("Fixture needs an Audio track");
   if (!route->fader_last ()) return finish ("Existing session did not default to fader last");
   if (!session->master_out ()->set_fader_last (false)) return finish ("Per-route option fixture");
   auto plugin = [&] (std::string const& name, std::string const& instance) {
    for (auto const& info : PluginManager::instance ().lv2_plugin_info ()) if (info->name == name) {
     auto p = std::make_shared<PluginInsert> (*session, *route, info->load (*session)); p->set_name (instance); return p;
    }
    throw std::runtime_error ("Missing built-in plugin");
   };
   eq = plugin ("ACE EQ", "Detail EQ"); delay = plugin ("ACE Delay", "Detail Delay");
   rack = std::make_shared<AudioRack> (*session, *route); rack->set_name ("Detail Rack");
   if (route->add_processor (eq, PreFader) || !route->set_fader_last (false) || route->add_processor (delay, PostFader)) return finish ("Fixture insertion");
   auto order = visible_order (route);
   if (std::find (order.begin (), order.end (), route->amp ()) > std::find (order.begin (), order.end (), delay)) return finish ("Disabled option did not allow a post-fader plugin");
   XMLNode& state = route->get_state (); bool pinned = true;
   bool persisted = state.get_property ("fader-last", pinned) && !pinned; delete &state;
   if (!persisted || !route->set_fader_last (true)) return finish ("Fader option state/toggle");
   auto batch_plugin = plugin ("ACE EQ", "Temporary Fader Batch");
   Route::ProcessorList batch {rack, batch_plugin};
   if (route->add_processors (batch, route->amp ())) return finish ("Batch insertion");
   order = visible_order (route);
   if (order != std::vector<std::shared_ptr<Processor>> {eq, delay, rack, batch_plugin, route->amp ()}) return finish ("Batch insertion changed device order");
   route->remove_processor (batch_plugin);
   order = visible_order (route);
   if (order != std::vector<std::shared_ptr<Processor>> {eq, delay, rack, route->amp ()}) return finish ("Pinned fader did not follow devices in signal order");
   Route::ProcessorList requested {eq, route->amp (), delay, rack};
   if (route->reorder_processors (requested, nullptr)) return finish ("Reorder fixture");
   session->selection ().select_stripable_and_maybe_group (route, SelectionSet, false);
   stage = 1; return TRUE;
  }
  RoutePropertiesBox* props = nullptr;
  for (auto* w : Gtk::Window::list_toplevels ()) if (w->get_visible ()) if (auto* p = find<RoutePropertiesBox> (w)) if (p->get_mapped ()) props = p;
  if (!props) return TRUE;
  auto* list = find<List> (props); if (!list || (stage != 9 && (stage < 20 || stage > 23) && !entry (list, eq))) return TRUE;
  static gint64 last_report = 0;
  static int reported_stage = -1;
  if (reported_stage != stage || g_get_monotonic_time () - last_report > 1000000) {
   g_printerr ("Device UI probe stage %d: focus=%d generic=%d rack=%d selected=%s\n", stage, list->is_focus (), count<GenericPluginUI> (props), count<AudioRackUI> (props), list->selection ().empty () ? "none" : list->selection ().back ()->processor ()->name ().c_str ());
   last_report = g_get_monotonic_time (); reported_stage = stage;
  }
  auto single = [&] (std::string const& name) { return count<GenericPluginUI> (props) == 1 && count<AudioRackUI> (props) == 0 && label (props, name); };
  auto selected = [&] (std::shared_ptr<Processor> p) { return list->selection ().size () == 1 && list->selection ().front ()->processor () == p; };
  auto* view = PublicEditor::instance ().time_axis_view_from_stripable (route);
  if (stage == 1) {
   if (!single ("Detail EQ")) return TRUE;
   if (visible_order (route) != std::vector<std::shared_ptr<Processor>> {eq, delay, rack, route->amp ()}) return finish ("Reorder moved the pinned fader");
   click (entry (list, eq)->action_widget (), 3);
   auto* menu = dynamic_cast<Gtk::Menu*> (ActionManager::get_widget ("/ProcessorMenu"));
   auto* item = menu ? static_cast<Gtk::CheckMenuItem*> (g_object_get_data (G_OBJECT (menu->gobj ()), "fader-last-item")) : nullptr;
   if (!item || !item->get_active ()) return finish ("Right-click fader option missing/default unchecked");
   item->set_active (false); if (route->fader_last ()) return finish ("Menu could not unpin the fader");
   item->set_active (true); if (!route->fader_last ()) return finish ("Menu could not pin the fader");
   menu->popdown ();
   view->focus_track_header (); stage = 24; return TRUE;
  }
  if (stage == 24) {
   active_title_brightness = title_brightness (header (props->get_toplevel (), view), route->name ());
   if (active_title_brightness <= 0) return finish ("Missing active track title color");
   snapshot (header (props->get_toplevel (), view), ".track-active");
   key (*props, GDK_Tab); stage = 11; return TRUE;
  }
  if (stage == 11) {
   if (!list->is_focus () || !selected (eq) || !single ("Detail EQ")) return finish ("Tab did not focus the selected device");
   auto* h = header (props->get_toplevel (), view);
   if (!h || title_brightness (h, route->name ()) >= active_title_brightness * .75) return finish ("Track did not dim after Tab");
   snapshot (h, ".track-inactive"); snapshot (&entry (list, eq)->widget (), ".device-active");
   key (*props, GDK_Right); stage = 12; return TRUE;
  }
  if (stage == 12) {
   if (!list->is_focus () || !selected (delay)) return finish ("Right did not select Delay");
   if (!single ("Detail Delay")) return TRUE;
   key (*props, GDK_Right); stage = 13; return TRUE;
  }
  if (stage == 13) {
   if (!selected (rack)) return finish ("Right did not select the rack");
   if (count<AudioRackUI> (props) != 1 || count<GenericPluginUI> (props)) return TRUE;
   auto* text = find<Gtk::Entry> (find<AudioRackUI> (props));
   if (!text) return finish ("Missing rack name entry");
   text->grab_focus (); text->select_region (2, 2); text->set_position (2); key (*props, GDK_Left);
   if (text->get_position () != 1 || !selected (rack)) return finish ("Entry arrow was intercepted");
   key (*props, GDK_Tab);
   if (view->track_header_has_focus () || list->is_focus () || !selected (rack)) return finish ("Entry Tab was intercepted");
   if (!props->focus_processors ()) return finish ("Could not refocus the device list after entry Tab");
   key (*props, GDK_Right); stage = 25; return TRUE;
  }
  if (stage == 25) {
   if (!selected (route->amp ())) return finish ("Arrow did not select Fader");
   if (count<GainMeter> (props) != 1 || count<GenericPluginUI> (props) || count<AudioRackUI> (props)) return TRUE;
   auto* gain = find<GainMeter> (props);
   if (!gain->get_mapped () || gain->get_width () < 30 || gain->get_height () < 80) return TRUE;
   if (gain->get_controllable () != route->gain_control ()) return finish ("Fader is not bound to track gain");
   snapshot (props, ".fader");
   key (*props, GDK_Right); if (!selected (route->amp ())) return finish ("Device navigation wrapped at the end");
   key (*props, GDK_KP_Left); stage = 26; return TRUE;
  }
  if (stage == 26) {
   if (!selected (rack)) return finish ("Keypad Left did not return from Fader to Rack");
   if (count<AudioRackUI> (props) != 1) return TRUE;
   key (*props, GDK_Left); stage = 14; return TRUE;
  }
  if (stage == 14) {
   if (!selected (delay)) return finish ("Left did not select Delay");
   if (!single ("Detail Delay")) return TRUE;
   key (*props, GDK_Left); stage = 15; return TRUE;
  }
  if (stage == 15) {
   if (!selected (eq)) return finish ("Left did not select EQ");
   if (!single ("Detail EQ")) return TRUE;
   key (*props, GDK_Left); if (!selected (eq)) return finish ("Device navigation wrapped at the start");
   key (*props, GDK_Tab); stage = 16; return TRUE;
  }
  if (stage == 16) {
   if (!view->track_header_has_focus ()) return finish ("Tab did not return to the track");
   if (std::abs (title_brightness (header (props->get_toplevel (), view), route->name ()) - active_title_brightness) > .01) return finish ("Track did not brighten on return");
   snapshot (&entry (list, eq)->widget (), ".device-inactive");
   key (*props, GDK_Tab); stage = 17; return TRUE;
  }
  if (stage == 17) {
   if (!list->is_focus () || !selected (eq)) return finish ("Tab did not preserve device selection");
   key (*props, GDK_ISO_Left_Tab, GDK_SHIFT_MASK); stage = 18; return TRUE;
  }
  if (stage == 18) {
   if (!view->track_header_has_focus ()) return finish ("Shift+Tab did not return to the track");
   ActionManager::get_toggle_action ("Editor", "show-editor-props")->set_active (false);
   view->focus_track_header (); key (*props, GDK_Tab); stage = 19; return TRUE;
  }
  if (stage == 19) {
   if (!list->get_mapped () || !list->is_focus ()) return finish ("Tab did not reopen/focus the bottom panel");
   click (entry (list, eq)->action_widget ()); stage = 2; return TRUE;
  }
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
   PublicEditor::instance ().time_axis_view_from_stripable (session->master_out ())->focus_track_header ();
   key (*props, GDK_Tab); stage = 20; return TRUE;
  }
  if (stage == 20) {
   if (!list->is_focus ()) return finish ("Tab did not focus a fader-only track");
   key (*props, GDK_Right); key (*props, GDK_Left);
   if (!list->is_focus () || !selected (session->master_out ()->amp ()) || count<GainMeter> (props) != 1) return finish ("Fader-only navigation changed focus/selection");
   click (entry (list, session->master_out ()->amp ())->action_widget (), 3);
   auto* menu = dynamic_cast<Gtk::Menu*> (ActionManager::get_widget ("/ProcessorMenu"));
   auto* item = static_cast<Gtk::CheckMenuItem*> (g_object_get_data (G_OBJECT (menu->gobj ()), "fader-last-item"));
   if (!item || item->get_active () || !route->fader_last ()) return finish ("Right-click option was not synced per route");
   item->set_active (true); menu->popdown ();
   if (!session->master_out ()->fader_last () || !route->fader_last ()) return finish ("Menu changed another route");
   props->focus_processors ();
   key (*props, GDK_Tab); stage = 21; return TRUE;
  }
  if (stage == 21) {
   if (!PublicEditor::instance ().time_axis_view_from_stripable (session->master_out ())->track_header_has_focus ()) return finish ("Empty device list did not return to Master");
   for (auto const& r : *session->get_routes ()) if (r->name () == "Group") group = r;
   if (group) {
    session->selection ().select_stripable_and_maybe_group (group, SelectionSet, false); stage = 22; return TRUE;
   }
   session->selection ().select_stripable_and_maybe_group (route, SelectionSet, false); stage = 10; return TRUE;
  }
  if (stage == 22) {
   PublicEditor::instance ().time_axis_view_from_stripable (group)->focus_track_header ();
   key (*props, GDK_Tab); stage = 23; return TRUE;
  }
  if (stage == 23) {
   if (!list->is_focus ()) return finish ("Group header Tab did not focus its devices");
   key (*props, GDK_Right); key (*props, GDK_ISO_Left_Tab, GDK_SHIFT_MASK);
   if (!PublicEditor::instance ().time_axis_view_from_stripable (group)->track_header_has_focus ()) return finish ("Device list did not return to the group");
   session->selection ().select_stripable_and_maybe_group (route, SelectionSet, false); stage = 10; return TRUE;
  }
  if (!single ("Detail EQ")) return TRUE;
  route->remove_processor (eq); route->remove_processor (delay);
  stage = 30; return TRUE;
 } catch (std::exception const& e) { g_printerr ("Device UI probe: %s\n", e.what ()); return finish ("Exception"); }
}
__attribute__((constructor)) static void initialize () {
 if (!getenv ("ARDOUR_DEVICE_UI_PROBE")) return;
 g_log_set_handler ("Gtk", G_LOG_LEVEL_CRITICAL, +[] (const gchar* domain, GLogLevelFlags level, const gchar* message, gpointer data) {
  g_log_default_handler (domain, level, message, data);
  void* frames[32]; int n = backtrace (frames, 32); backtrace_symbols_fd (frames, n, 2);
 }, nullptr);
 g_timeout_add (40, tick, nullptr);
}
