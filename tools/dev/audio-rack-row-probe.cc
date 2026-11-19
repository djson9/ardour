/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test-only bridge so the C rack probe can inspect the shared C++ rows. */
#include <ytkmm/widget.h>
#include "processor_box.h"
#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
extern "C" NSView* gdk_quartz_window_get_nsview (GdkWindow*);
#endif

static GtkWidget* drag_source = nullptr;
extern "C" int rack_probe_dragging () { return drag_source != nullptr; }
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
 if (!widget.get_data ("rack-probe-drag-watched")) {
  widget.set_data ("rack-probe-drag-watched", &drag_source);
  // A move out can replace the source panel. Observe native drag completion
  // or source destruction; either releases this probe's source widget.
  g_signal_connect (widget.gobj (), "drag-begin", G_CALLBACK (+[] (GtkWidget* source, GdkDragContext*, gpointer) { drag_source = source; g_printerr ("RACK PROBE drag begin\n"); }), nullptr);
  g_signal_connect (widget.gobj (), "drag-end", G_CALLBACK (+[] (GtkWidget* source, GdkDragContext*, gpointer) { if (drag_source == source) drag_source = nullptr; g_printerr ("RACK PROBE drag end\n"); }), nullptr);
  g_signal_connect (widget.gobj (), "destroy", G_CALLBACK (+[] (GtkWidget* source, gpointer) { if (drag_source == source) drag_source = nullptr; }), nullptr);
 }
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
