/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mcp_http_ui.h"
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>
#include "gtkmm2ext/actions.h"
#include "gtkmm2ext/bindings.h"
#include "pbd/event_loop.h"
#include <ytkmm/window.h>

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
		if (tool == "ui/close_dialog") {
			bool found = false;
			for (Gtk::Window* w : Gtk::Window::list_toplevels ()) {
				if (!w->get_visible () || w->get_title () != window_name || !GTK_IS_DIALOG (w->gobj ())) continue;
				GdkEvent* event = gdk_event_new (GDK_DELETE);
				event->any.window = GDK_WINDOW (g_object_ref (w->get_window ()->gobj ()));
				event->any.send_event = true;
				gtk_widget_event (GTK_WIDGET (w->gobj ()), event);
				gdk_event_free (event);
				found = true;
				break;
			}
			out << (found ? "{\"closeRequested\":true}" : "{\"error\":\"Visible dialog not found\"}");
		} else if (tool == "ui/press_key") {
			KeyboardKey key;
			Gtk::Window* target = nullptr;
			for (Gtk::Window* w : Gtk::Window::list_toplevels ()) {
				if (w->get_visible () && w->get_title () == window_name) { target = w; break; }
			}
			if (!target || !KeyboardKey::make_key (key_name, key)) {
				out << "{\"error\":\"Invalid key or window title\"}";
			} else {
				for (GdkEventType type : {GDK_KEY_PRESS, GDK_KEY_RELEASE}) {
					GdkEvent* event = gdk_event_new (type);
					event->key.window = GDK_WINDOW (g_object_ref (target->get_window ()->gobj ()));
					event->key.send_event = true;
					event->key.time = GDK_CURRENT_TIME;
					event->key.state = key.state ();
					event->key.keyval = key.key ();
					gtk_main_do_event (event);
					gdk_event_free (event);
				}
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
				action->activate ();
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
				    << ",\"focus\":" << quote (w->get_focus () ? G_OBJECT_TYPE_NAME (w->get_focus ()->gobj ()) : "") << "}";
			}
			out << "]}";
		}
		reply->json = out.str ();
		reply->done = true;
		reply->cv.notify_one ();
	});
	if (!queued) return "{\"error\":\"Could not queue UI request\"}";
	if (tool != "ui/get_state") return "{\"queued\":true}";
	std::unique_lock<std::mutex> lock (reply->mutex);
	if (!reply->cv.wait_for (lock, std::chrono::seconds (2), [&] { return reply->done; })) {
		reply->cancelled = true;
		return "{\"error\":\"UI request timed out\"}";
	}
	return reply->json;
}
