/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mcp_http_ui.h"
#include "canvas/canvas.h"
#include "canvas/text.h"
#include <chrono>
#include <glibmm/main.h>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>
#include <deque>
#include "gtkmm2ext/gtk_ui.h"
#include "gtkmm2ext/actions.h"
#include "gtkmm2ext/bindings.h"
#include "gtkmm2ext/keyboard.h"
#include <ytkmm/label.h>
#include "pbd/event_loop.h"
#include <ytkmm/window.h>
#include <ytkmm/treeview.h>
#include <ytkmm/spinbutton.h>
#include <ytkmm/togglebutton.h>
#include <ytkmm/combobox.h>
#include <functional>
#include <cmath>

namespace {
std::string quote (std::string const& s) {
	std::ostringstream out;
	out << '"';
	for (unsigned char c : s) {
		switch (c) {
		case '"': out << "\\\""; break;
		case '\\': out << "\\\\"; break;
		case '\n': out << "\\n"; break;
		case '\r': out << "\\r"; break;
		case '\t': out << "\\t"; break;
		default:
			if (c < 32) { const char* hex = "0123456789abcdef"; out << "\\u00" << hex[c >> 4] << hex[c & 15]; }
			else out << c;
		}
	}
	out << '"';
	return out.str ();
}
/* Keep the response bounded even when the process log is very large. */
class LogTail : public std::streambuf {
public:
	std::deque<char> data;
	bool truncated = false;
	int_type overflow (int_type c) override {
		if (traits_type::eq_int_type (c, traits_type::eof ())) return traits_type::not_eof (c);
		if (data.size () == 65536) { data.pop_front (); truncated = true; }
		data.push_back (traits_type::to_char_type (c));
		return c;
	}
	std::string text () {
		/* Discard a partial first line, including any partial UTF-8 sequence. */
		if (truncated) {
			while (!data.empty () && data.front () != '\n') data.pop_front ();
			if (!data.empty ()) data.pop_front ();
		}
		return std::string (data.begin (), data.end ());
	}
};
void warp_controls (Gtk::Widget* widget, std::vector<Gtk::Widget*>& result) {
 if (!widget || !widget->get_visible ()) return;
 if (widget->get_name ().raw ().find ("warp-") == 0) result.push_back (widget);
 if (auto* container = dynamic_cast<Gtk::Container*> (widget)) for (auto* child : container->get_children ()) warp_controls (child, result);
}
void canvas_widgets (Gtk::Widget* widget, std::vector<Gtk::Widget*>& result) {
 if (!widget || !widget->get_visible ()) return;
 if (widget->get_name () == "AutomationClipCanvas" || widget->get_name () == "EditorMainCanvas" || widget->get_name () == "AudioClipCanvas" || widget->get_name () == "MidiCueCanvas") result.push_back (widget);
 if (auto* container = dynamic_cast<Gtk::Container*> (widget)) for (auto* child : container->get_children ()) canvas_widgets (child, result);
}
// Resolve a header by its full track name, independent of screen geometry.
Gtk::Widget* track_header (Gtk::Widget* widget, std::string const& name) {
 if (!widget || !widget->get_visible ()) return nullptr;
 if (auto* label = dynamic_cast<Gtk::Label*> (widget)) {
  if (label->get_text () == name) {
   for (auto* parent = label->get_parent (); parent; parent = parent->get_parent ()) {
    if (GTK_IS_EVENT_BOX (parent->gobj ()) && parent->get_name ().raw ().find ("ControlsBase") != std::string::npos) return parent;
   }
  }
 }
 if (auto* container = dynamic_cast<Gtk::Container*> (widget)) for (auto* child : container->get_children ()) {
  if (auto* found = track_header (child, name)) return found;
 }
 return nullptr;
}
Gtk::Widget* track_control (Gtk::Widget* widget, std::string const& role) {
 if (!widget || !widget->get_visible ()) return nullptr;
 const auto* name = static_cast<const char*> (widget->get_data ("compact-track-control"));
 if (name && role == name) return widget;
 if (auto* container = dynamic_cast<Gtk::Container*> (widget)) for (auto* child : container->get_children ()) {
  if (auto* found = track_control (child, role)) return found;
 }
 return nullptr;
}
void canvas_labels (ArdourCanvas::Item* item, std::ostringstream& out, unsigned& count, unsigned depth=0) {
 if (!item || !item->visible () || count >= 256 || depth > 64) return;
 if (auto* text = dynamic_cast<ArdourCanvas::Text*> (item)) {
  const auto box = item->item_to_window (item->bounding_box ());
  if (std::isfinite (box.x0) && std::isfinite (box.y0) && std::isfinite (box.x1) && std::isfinite (box.y1)) {
   if (count++) out << ',';
   out << "{\"text\":" << quote (text->text ()) << ",\"x0\":" << box.x0 << ",\"y0\":" << box.y0 << ",\"x1\":" << box.x1 << ",\"y1\":" << box.y1 << ",\"ancestors\":[";
   unsigned n=0;for (auto* parent=item->parent ();parent && n<3;parent=parent->parent (),++n) {
    const auto r=parent->item_to_window (parent->bounding_box ());if (n) out << ',';
    out << "{\"type\":" << quote (parent->whatami ()) << ",\"x0\":" << r.x0 << ",\"y0\":" << r.y0 << ",\"x1\":" << r.x1 << ",\"y1\":" << r.y1 << "}";
   }
   out << "]}";
  }
 }
 for (auto* child : item->items ()) canvas_labels (child, out, count, depth+1);
}
struct Reply {
	std::mutex mutex;
	std::condition_variable cv;
	bool done = false;
	bool cancelled = false;
	std::string json;
};
}

std::string
ArdourSurface::mcp_ui_call (PBD::EventLoop* loop, std::string const& tool,
                            std::string const& action_name, std::string const& group,
                            std::string const& key_name, std::string const& window_name)
{
	if (!loop) return "{\"error\":\"UI event loop unavailable\"}";
	auto reply = std::make_shared<Reply> ();
	bool queued = loop->call_slot (MISSING_INVALIDATOR, [reply, tool, action_name, group, key_name, window_name] {
		std::lock_guard<std::mutex> lock (reply->mutex);
		if (reply->cancelled) return;
		using namespace Gtkmm2ext;
		std::ostringstream out;
		auto action = action_name.empty () ? Glib::RefPtr<Gtk::Action> () : ActionManager::get_action (action_name, false);
		if (tool == "ui/get_logs") {
			LogTail tail;
			std::ostream log (&tail);
			UI::instance ()->dump_errors (log);
			out << "{\"source\":\"Ardour in-process log\",\"text\":" << quote (tail.text ())
			    << ",\"truncated\":" << (tail.truncated ? "true" : "false") << ",\"maxBytes\":65536}";
		} else if (tool == "ui/close_dialog") {
			bool found = false;
			for (Gtk::Window* w : Gtk::Window::list_toplevels ()) {
				if (!w->get_visible () || w->get_title () != window_name || (!GTK_IS_DIALOG (w->gobj ()) && !gtk_window_get_transient_for (w->gobj ()) && w->get_name () != "IOSelectorWindow2")) continue;
				GdkEvent* event = gdk_event_new (GDK_DELETE);
				event->any.window = GDK_WINDOW (g_object_ref (w->get_window ()->gobj ()));
				event->any.send_event = true;
				/* Let GTK apply normal window-manager delete handling when the
				 * window returns false (e.g. native I/O routing windows). */
				gdk_event_put (event);
				gdk_event_free (event);
				found = true;
				break;
			}
			out << (found ? "{\"closeRequested\":true}" : "{\"error\":\"Visible dialog not found\"}");
		} else if (tool == "ui/canvas_pointer") {
   Gtk::Widget* target = nullptr;
   for (auto* w : Gtk::Window::list_toplevels ()) {
    if (!w->get_visible () || w->get_title () != window_name) continue;
    std::vector<Gtk::Widget*> canvases; canvas_widgets (w, canvases);
    for (auto* c : canvases) if (c->get_name () == group) target = c;
    if (group.compare (0, 12, "TrackHeader:") == 0) target = track_header (w, group.substr (12));
    if (group.compare (0, 13, "TrackControl:") == 0) {
     const auto split = group.rfind ('|');
     if (split != std::string::npos && split > 13) target = track_control (track_header (w, group.substr (13, split - 13)), group.substr (split + 1));
    }
   }
   auto* canvas_target = dynamic_cast<ArdourCanvas::GtkCanvas*> (target);
   const bool grabbed = (canvas_target && canvas_target->have_grab ()) ||
    (target && group.compare (0, 12, "TrackHeader:") == 0 && gtk_grab_get_current () == GTK_WIDGET (target->gobj ()));
   double x=0,y=0;int button=1;bool shift=false,down=false;std::istringstream coordinates (key_name);coordinates.imbue (std::locale::classic ());
   if (!target || !target->get_window () || !(coordinates >> x >> y >> shift >> down) || (!grabbed && (x < 0 || y < 0 || x >= target->get_allocation ().get_width () || y >= target->get_allocation ().get_height ())) || (action_name != "move" && action_name != "press" && action_name != "release" && action_name != "pinch" && action_name != "doublePress" && action_name != "scroll")) {
    out << "{\"error\":\"Invalid canvas, event, or local coordinates\"}";
   } else {
    double magnification = 0; bool primary = false; double dx = 0, dy = 0; coordinates >> button >> magnification >> primary >> dx >> dy;
    if (button != 1 && button != 3) button = 1;
    const auto type = (action_name == "pinch" || action_name == "scroll") ? GDK_SCROLL : action_name == "move" ? GDK_MOTION_NOTIFY : action_name == "press" ? GDK_BUTTON_PRESS : action_name == "doublePress" ? GDK_2BUTTON_PRESS : GDK_BUTTON_RELEASE;
    /* GtkEntry handles clicks on its inner text window, not its outer frame. */
    GdkWindow* event_window = GTK_IS_ENTRY (target->gobj ()) ? GTK_ENTRY (target->gobj ())->text_area : target->get_window ()->gobj ();
    auto* event = gdk_event_new (type);event->any.window = GDK_WINDOW (g_object_ref (event_window));event->any.send_event = true;
    int ox=0,oy=0;target->get_window ()->get_origin (ox, oy);
    const guint state = (primary ? Gtkmm2ext::Keyboard::PrimaryModifier : 0) | (shift ? GDK_SHIFT_MASK : 0) | (down ? (button == 3 ? GDK_BUTTON3_MASK : GDK_BUTTON1_MASK) : 0);
    if (type == GDK_SCROLL) {
     event->scroll.time = guint32 (g_get_monotonic_time ()/1000);event->scroll.x=x;event->scroll.y=y;event->scroll.x_root=x+ox;event->scroll.y_root=y+oy;event->scroll.state=state;event->scroll.direction=action_name == "pinch" ? GDK_SCROLL_PINCH : GDK_SCROLL_DOWN;event->scroll.has_deltas=true;event->scroll.delta_x=dx;event->scroll.delta_y=action_name == "pinch" ? magnification : dy;
    } else if (type == GDK_MOTION_NOTIFY) {
     event->motion.time = guint32 (g_get_monotonic_time ()/1000);event->motion.x=x;event->motion.y=y;event->motion.x_root=x+ox;event->motion.y_root=y+oy;event->motion.state=state;event->motion.is_hint=false;
    } else {
     event->button.time = guint32 (g_get_monotonic_time ()/1000);event->button.x=x;event->button.y=y;event->button.x_root=x+ox;event->button.y_root=y+oy;event->button.state=state;event->button.button=button;
    }
    /* Native buttons require a pointer-enter before a press to establish
     * hover/grab state. Canvas items perform their own crossing dispatch. */
    if (type == GDK_BUTTON_PRESS && group.compare (0, 13, "TrackControl:") == 0) {
     GdkEvent* enter = gdk_event_new (GDK_ENTER_NOTIFY);
     enter->crossing.window = GDK_WINDOW (g_object_ref (target->get_window ()->gobj ()));
     enter->crossing.send_event = true;enter->crossing.x=x;enter->crossing.y=y;
     enter->crossing.x_root=x+ox;enter->crossing.y_root=y+oy;
     enter->crossing.mode=GDK_CROSSING_NORMAL;enter->crossing.detail=GDK_NOTIFY_NONLINEAR;
     gtk_widget_event (target->gobj (), enter);gdk_event_free (enter);
    }
    const bool handled = gtk_widget_event (target->gobj (), event);gdk_event_free (event);
    out << "{\"delivered\":true,\"handled\":" << (handled ? "true" : "false")
        << ",\"xRoot\":" << ox << ",\"yRoot\":" << oy << ",\"width\":" << target->get_allocation ().get_width () << ",\"height\":" << target->get_allocation ().get_height () << "}";
   }
  } else if (tool == "ui/set_warp_control") {
   Gtk::Widget* target = nullptr;
   for (Gtk::Window* w : Gtk::Window::list_toplevels ()) {
    if (!w->get_visible () || w->get_title () != window_name) continue;
    std::vector<Gtk::Widget*> controls; warp_controls (w, controls);
    for (auto* control : controls) if (control->get_name () == action_name) target = control;
   }
   try {
    if (!target || !gtk_widget_is_sensitive (target->gobj ())) throw std::runtime_error ("Visible sensitive warp control not found");
    if (auto* toggle = dynamic_cast<Gtk::ToggleButton*> (target)) {
     if (key_name != "true" && key_name != "false") throw std::runtime_error ("Toggle value must be true or false");
     toggle->set_active (key_name == "true");
    } else {
     size_t consumed = 0; double value = std::stod (key_name, &consumed);
     if (consumed != key_name.size () || !std::isfinite (value)) throw std::runtime_error ("Invalid numeric value");
     if (auto* spin = dynamic_cast<Gtk::SpinButton*> (target)) {
      double lo, hi; spin->get_range (lo, hi);
      if (value < lo || value > hi) throw std::runtime_error ("Value outside control range");
      spin->set_value (value);
     } else if (auto* combo = dynamic_cast<Gtk::ComboBox*> (target)) {
      if (value != std::floor (value) || value < 0 || value >= combo->get_model ()->children ().size ()) throw std::runtime_error ("Invalid choice index");
      combo->set_active (int (value));
     } else throw std::runtime_error ("Unsupported warp control");
    }
    out << "{\"changed\":" << quote (action_name) << "}";
   } catch (std::exception const& e) { out << "{\"error\":" << quote (e.what ()) << "}"; }
  } else if (tool == "ui/press_key") {
			KeyboardKey key;
			Gtk::Window* target = nullptr;
			for (Gtk::Window* w : Gtk::Window::list_toplevels ()) {
				if (w->get_visible () && w->get_title () == window_name) { target = w; break; }
			}
			if (!target || !KeyboardKey::make_key (key_name, key)) {
				out << "{\"error\":\"Invalid key or window title\"}";
			} else {
				GdkKeymapKey* keymap_keys = nullptr;
				gint n_keys = 0;
				gdk_keymap_get_entries_for_keyval (gdk_keymap_get_default (), key.key (), &keymap_keys, &n_keys);
				for (GdkEventType type : {GDK_KEY_PRESS, GDK_KEY_RELEASE}) {
					GdkEvent* event = gdk_event_new (type);
					event->key.window = GDK_WINDOW (g_object_ref (target->get_window ()->gobj ()));
					event->key.send_event = true;
					event->key.time = GDK_CURRENT_TIME;
					event->key.state = key.state ();
					event->key.keyval = key.key ();
					if (n_keys > 0) {
						event->key.hardware_keycode = keymap_keys[0].keycode;
						event->key.group = keymap_keys[0].group;
					}
					gchar text[7] = {};
					gunichar character = gdk_keyval_to_unicode (key.key ());
					event->key.length = character ? g_unichar_to_utf8 (character, text) : 0;
					event->key.string = g_strdup (text);
					/* Queue native dispatch so modal actions can still service the
					 * control-surface event source (e.g. Cancel Freeze). */
					gdk_event_put (event);
					gdk_event_free (event);
				}
				g_free (keymap_keys);
				out << "{\"delivered\":true}";
			}
		} else if (tool == "ui/activate_action") {
			/* Quitting from a synchronous HTTP request would join this server thread.
			 * Session lifecycle remains with the client/normal OS app launcher. */
			if (action_name == "Common/Quit" || action_name == "Main/Quit") {
				out << "{\"error\":\"Quit through the normal application lifecycle\"}";
			} else if (!action || !action->get_sensitive ()) {
				out << "{\"error\":\"Action missing or insensitive\"}";
			} else {
				/* A modal dialog runs a nested GTK loop. Leave the control-surface
				 * event source before activation so later UI requests can enter it. */
				Glib::signal_idle().connect_once ([action] { action->activate(); });
				out << "{\"activated\":" << quote (action_name) << "}";
			}
		} else {
			out << "{\"group\":" << quote (group) << ",\"action\":" << quote (action_name)
			    << ",\"actionExists\":" << (action ? "true" : "false")
			    << ",\"sensitive\":" << (action && action->get_sensitive () ? "true" : "false");
			Bindings* bindings = Bindings::get_bindings (group);
			Bindings::Operation op = Bindings::Press;
			out << ",\"key\":" << quote (bindings && action ? bindings->get_binding_for_action (action, op).name () : "");
			out << ",\"windows\":[";
			bool first = true;
			for (Gtk::Window* w : Gtk::Window::list_toplevels ()) {
				if (!w->get_visible ()) continue;
				if (!first) out << ',';
				first = false;
				out << "{\"title\":" << quote (w->get_title ())
				    << ",\"active\":" << (gtk_window_is_active (w->gobj ()) ? "true" : "false")
				    << ",\"textEntry\":" << (w->get_focus () && GTK_IS_ENTRY (w->get_focus ()->gobj ()) ? "true" : "false")
				    << ",\"modal\":" << (w->get_modal () ? "true" : "false")
				    << ",\"focusName\":" << quote (w->get_focus () ? w->get_focus ()->get_name () : "")
				    << ",\"focus\":" << quote (w->get_focus () ? G_OBJECT_TYPE_NAME (w->get_focus ()->gobj ()) : "");
				if (w->get_focus () && GTK_IS_ENTRY (w->get_focus ()->gobj ())) {
					out << ",\"focusText\":" << quote (gtk_entry_get_text (GTK_ENTRY (w->get_focus ()->gobj ())));
					gint start = 0, end = 0;
					gtk_editable_get_selection_bounds (GTK_EDITABLE (w->get_focus ()->gobj ()), &start, &end);
					out << ",\"selectionStart\":" << start << ",\"selectionEnd\":" << end;
				} else if (w->get_focus () && GTK_IS_BUTTON (w->get_focus ()->gobj ())) {
					const char* label = gtk_button_get_label (GTK_BUTTON (w->get_focus ()->gobj ()));
					out << ",\"focusText\":" << quote (label ? label : "");
				}
				if (auto* view = dynamic_cast<Gtk::TreeView*> (w->get_focus ())) {
					auto row = view->get_selection ()->get_selected ();
					if (row && gtk_tree_model_get_column_type (view->get_model ()->gobj (), 0) == G_TYPE_STRING) {
						std::string selected;
						row->get_value (0, selected);
						out << ",\"selectedChoice\":" << quote (selected);
					}
				}
				out << ",\"warpControls\":[";
    std::vector<Gtk::Widget*> controls; warp_controls (w, controls); bool first_control = true;
    for (auto* c : controls) {
     if (!first_control) out << ','; first_control = false;
     out << "{\"id\":" << quote (c->get_name ()) << ",\"sensitive\":" << (gtk_widget_is_sensitive (c->gobj ()) ? "true" : "false");
     if (auto* spin = dynamic_cast<Gtk::SpinButton*> (c)) out << ",\"value\":" << spin->get_value ();
     else if (auto* toggle = dynamic_cast<Gtk::ToggleButton*> (c)) out << ",\"value\":" << (toggle->get_active () ? "true" : "false");
     else if (auto* combo = dynamic_cast<Gtk::ComboBox*> (c)) out << ",\"value\":" << combo->get_active_row_number ();
     out << "}";
    }
    out << "],\"canvases\":[";
    std::vector<Gtk::Widget*> canvases;canvas_widgets (w, canvases);bool first_canvas=true;
    for (auto* canvas : canvases) {
     if (!first_canvas) out << ',';first_canvas=false;
     int ox = 0, oy = 0; canvas->get_window ()->get_origin (ox, oy);
     out << "{\"xRoot\":" << ox << ",\"yRoot\":" << oy << ",\"name\":" << quote (canvas->get_name ()) << ",\"width\":" << canvas->get_allocation ().get_width () << ",\"height\":" << canvas->get_allocation ().get_height () << ",\"labels\":[";
     unsigned labels=0;auto* c=dynamic_cast<ArdourCanvas::GtkCanvas*> (canvas);if (c) canvas_labels (c->root (), out, labels);
     out << "],\"grabbed\":" << (c && c->have_grab () ? "true" : "false") << "}";
    }
    out << "]}";
			}
			out << "]}";
		}
		reply->json = out.str ();
		reply->done = true;
		reply->cv.notify_one ();
	});
	if (!queued) return "{\"error\":\"Could not queue UI request\"}";
	if (tool != "ui/get_state" && tool != "ui/get_logs" && tool != "ui/set_warp_control" && tool != "ui/canvas_pointer") return "{\"queued\":true}";
	std::unique_lock<std::mutex> lock (reply->mutex);
	if (!reply->cv.wait_for (lock, std::chrono::seconds (2), [&] { return reply->done; })) {
		reply->cancelled = true;
		return "{\"error\":\"UI request timed out\"}";
	}
	return reply->json;
}
