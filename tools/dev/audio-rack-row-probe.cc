/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test-only bridge so the C rack probe can inspect the shared C++ rows. */
#include <ytkmm/widget.h>
#include "processor_box.h"
#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
extern "C" NSView* gdk_quartz_window_get_nsview (GdkWindow*);
#endif

static bool dragging = false;
extern "C" int rack_probe_dragging () { return dragging; }
using List = Gtkmm2ext::DnDVBox<ProcessorEntry>;
static List* list (GtkWidget* w) { return w ? dynamic_cast<List*> (Glib::wrap (w)) : nullptr; }
extern "C" int rack_probe_plugin_count (GtkWidget* w) {
 auto* p = list (w); return p ? p->children ().size () : -1;
}
extern "C" int rack_probe_plugin_name (GtkWidget* w, int index, char const* name) {
 auto* p = list (w); if (!p) return 0; auto rows = p->children ();
 return index >= 0 && index < int (rows.size ()) && (*std::next (rows.begin (), index))->processor ()->name () == name;
}
extern "C" GtkWidget* rack_probe_plugin_row (GtkWidget* w, int index) {
 auto* p = list (w); if (!p) return nullptr; auto rows = p->children ();
 if (index < 0 || index >= int (rows.size ())) return nullptr;
 auto& widget = (*std::next (rows.begin (), index))->action_widget ();
 g_printerr ("RACK PROBE row: %s (%dx%d)\n", (*std::next (rows.begin (), index))->processor ()->name ().c_str (), widget.get_width (), widget.get_height ());
 widget.signal_drag_begin ().connect ([] (Glib::RefPtr<Gdk::DragContext> const&) { dragging = true; g_printerr ("RACK PROBE drag begin\n"); });
 widget.signal_drag_end ().connect ([] (Glib::RefPtr<Gdk::DragContext> const&) { dragging = false; g_printerr ("RACK PROBE drag end\n"); });
 widget.signal_drag_data_get ().connect ([] (Glib::RefPtr<Gdk::DragContext> const&, Gtk::SelectionData& d, guint, guint) { g_printerr ("RACK PROBE data: %s\n", d.get_target ().c_str ()); });
 return GTK_WIDGET (widget.gobj ());
}

#ifdef __APPLE__
extern "C" int rack_probe_capture (GtkWidget* w, char const* path) {
 auto* top = gtk_widget_get_toplevel (w);
 NSView* view = gdk_quartz_window_get_nsview (gtk_widget_get_window (top));
 if (!view) return 0;
 gint x = 0, y = 0; GtkAllocation a;
 gtk_widget_translate_coordinates (w, top, 0, 0, &x, &y);
 gtk_widget_get_allocation (w, &a);
 NSRect rect = NSMakeRect (x, y, a.width, a.height);
 NSBitmapImageRep* pixels = [view bitmapImageRepForCachingDisplayInRect:rect];
 [view cacheDisplayInRect:rect toBitmapImageRep:pixels];
 NSData* png = [pixels representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
 return [png writeToFile:[NSString stringWithUTF8String:path] atomically:true];
}
#endif
