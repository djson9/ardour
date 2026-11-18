/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Inject only into a disposable Verification GUI, then create the trigger file. */
#include <fstream>
#include <ytkmm/container.h>
#include <ytkmm/label.h>
#include <ytkmm/window.h>
#include "widgets/ardour_knob.h"
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
static void check_plugin_knob_scroll (GenericPluginUI* ui) {
 auto* knob = find<ArdourWidgets::ArdourKnob> (ui);
 auto* scroller = find<Gtk::ScrolledWindow> (ui);
 if (!knob || !scroller) throw std::runtime_error ("Missing compact plugin knob or scroller");
 auto* adj = scroller->get_vadjustment ();
 const bool overflows = adj->get_upper () - adj->get_page_size () > adj->get_lower ();

 const double original_scroll = adj->get_value ();
 const auto control = knob->get_controllable ();
 const double original_value = control->get_value ();
 adj->set_value (adj->get_lower ());
 auto scroll = [&] (GdkScrollDirection direction, bool smooth) {
  GdkEvent* event = gdk_event_new (GDK_SCROLL);
  event->scroll.window = GDK_WINDOW (g_object_ref (knob->get_window ()->gobj ()));
  event->scroll.send_event = true;
  event->scroll.direction = direction;
  event->scroll.has_deltas = smooth;
  event->scroll.delta_y = direction == GDK_SCROLL_UP ? -5 : 5;
  gtk_propagate_event (GTK_WIDGET (knob->gobj ()), event);
  gdk_event_free (event);
  if (control->get_value () != original_value) throw std::runtime_error ("Scrolling edited a compact plugin parameter");
 };
 scroll (GDK_SCROLL_UP, false); // At the top edge.
 scroll (GDK_SCROLL_DOWN, false);
 const double after_wheel = adj->get_value ();
 if (overflows && after_wheel <= adj->get_lower ()) throw std::runtime_error ("Wheel did not scroll the plugin list");
 scroll (GDK_SCROLL_UP, true);
 if (overflows && adj->get_value () >= after_wheel) throw std::runtime_error ("Smooth scroll did not scroll the plugin list");
 adj->set_value (original_scroll);

 const double step = control->get_interface (true) < 0.5 ? -60 : 60;
 double y = 8;
 for (auto type : {GDK_BUTTON_PRESS, GDK_MOTION_NOTIFY, GDK_MOTION_NOTIFY, GDK_BUTTON_RELEASE}) {
  GdkEvent* event = gdk_event_new (type);
  event->any.window = GDK_WINDOW (g_object_ref (knob->get_window ()->gobj ()));
  event->any.send_event = true;
  if (type == GDK_MOTION_NOTIFY) {
   y += step;
   event->motion.state = GDK_BUTTON1_MASK; event->motion.x = 8; event->motion.y = y;
  } else {
   event->button.button = 1; event->button.x = 8; event->button.y = y;
  }
  gtk_widget_event (GTK_WIDGET (knob->gobj ()), event);
  gdk_event_free (event);
 }
 if (control->get_value () == original_value) throw std::runtime_error ("Dragging did not edit the compact plugin parameter");
 control->set_value (original_value, PBD::Controllable::NoGroup);
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
   if (route->add_processor (eq, PreFader) || route->add_processor (delay, PostFader) || route->add_processor (rack, PostFader)) return finish ("Fixture insertion");
   session->selection ().select_stripable_and_maybe_group (route, SelectionSet, false);
   stage = 1; return TRUE;
  }
  RoutePropertiesBox* props = nullptr;
  for (auto* w : Gtk::Window::list_toplevels ()) if (w->get_visible ()) if (auto* p = find<RoutePropertiesBox> (w)) if (p->get_mapped ()) props = p;
  if (!props) return TRUE;
  auto* list = find<List> (props); if (!list || (stage != 9 && (stage < 20 || stage > 23) && !entry (list, eq))) return TRUE;
  static gint64 last_report = 0;
  if (g_get_monotonic_time () - last_report > 1000000) {
   g_printerr ("Device UI probe stage %d: generic=%d rack=%d selected=%s\n", stage, count<GenericPluginUI> (props), count<AudioRackUI> (props), list->selection ().empty () ? "none" : list->selection ().back ()->processor ()->name ().c_str ());
   last_report = g_get_monotonic_time ();
  }
  auto single = [&] (std::string const& name) { return count<GenericPluginUI> (props) == 1 && count<AudioRackUI> (props) == 0 && label (props, name); };
  auto selected = [&] (std::shared_ptr<Processor> p) { return list->selection ().size () == 1 && list->selection ().front ()->processor () == p; };
  auto* view = PublicEditor::instance ().time_axis_view_from_stripable (route);
  if (stage == 1) {
   if (!single ("Detail EQ")) return TRUE;
   check_plugin_knob_scroll (find<GenericPluginUI> (props));
   view->focus_track_header (); key (*props, GDK_Tab); stage = 11; return TRUE;
  }
  if (stage == 11) {
   if (!list->is_focus () || !selected (eq) || !single ("Detail EQ")) return finish ("Tab did not focus the selected device");
   key (*props, GDK_Right); stage = 12; return TRUE;
  }
  if (stage == 12) {
   if (!list->is_focus () || !selected (delay) || !single ("Detail Delay")) return finish ("Right did not skip the fader/select Delay");
   key (*props, GDK_Right); stage = 13; return TRUE;
  }
  if (stage == 13) {
   if (!selected (rack) || count<AudioRackUI> (props) != 1 || count<GenericPluginUI> (props)) return finish ("Right did not select the rack");
   auto* text = find<Gtk::Entry> (find<AudioRackUI> (props));
   if (!text) return finish ("Missing rack name entry");
   text->grab_focus (); text->select_region (2, 2); text->set_position (2); key (*props, GDK_Left);
   if (text->get_position () != 1 || !selected (rack)) return finish ("Entry arrow was intercepted");
   key (*props, GDK_Tab);
   if (view->track_header_has_focus () || list->is_focus () || !selected (rack)) return finish ("Entry Tab was intercepted");
   if (!props->focus_processors ()) return finish ("Could not refocus the device list after entry Tab");
   key (*props, GDK_Right); if (!selected (rack)) return finish ("Device navigation wrapped at the end");
   key (*props, GDK_KP_Left); stage = 14; return TRUE;
  }
  if (stage == 14) {
   if (!selected (delay) || !single ("Detail Delay")) return finish ("Keypad Left did not select Delay");
   key (*props, GDK_Left); stage = 15; return TRUE;
  }
  if (stage == 15) {
   if (!selected (eq) || !single ("Detail EQ")) return finish ("Left did not select EQ");
   key (*props, GDK_Left); if (!selected (eq)) return finish ("Device navigation wrapped at the start");
   key (*props, GDK_Tab); stage = 16; return TRUE;
  }
  if (stage == 16) {
   if (!view->track_header_has_focus ()) return finish ("Tab did not return to the track");
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
   if (!list->is_focus ()) return finish ("Tab did not focus an empty device list");
   key (*props, GDK_Right); key (*props, GDK_Left);
   if (!list->is_focus () || !list->selection ().empty ()) return finish ("Empty device navigation changed focus/selection");
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
  return finish (nullptr);
 } catch (std::exception const& e) { g_printerr ("Device UI probe: %s\n", e.what ()); return finish ("Exception"); }
}
__attribute__((constructor)) static void initialize () { if (getenv ("ARDOUR_DEVICE_UI_PROBE")) g_timeout_add (40, tick, nullptr); }
