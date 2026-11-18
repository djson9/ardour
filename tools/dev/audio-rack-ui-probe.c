/* SPDX-License-Identifier: GPL-2.0-or-later
 * Test-only GTK interaction/rendering, injected into a disposable GUI process.
 * Screenshots contain only the tested window, never the user's desktop. */
#include <ytk/ytk.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static GtkWidget *rack, *capturing, *content, *offscreen;
static const char *capture_name;
static int stage, ticks;
static GtkWidget* top (const char* title) {
 GList* list=gtk_window_list_toplevels(); GtkWidget* result=NULL;
 for(GList* p=list;p;p=p->next) {
  const char* t=gtk_window_get_title(GTK_WINDOW(p->data));
  if(t && !strcmp(t,title) && gtk_widget_get_visible(p->data)) {result=p->data;break;}
 }
 g_list_free(list); return result;
}
struct Search { const char* text; GtkWidget* result; };
static void find_button (GtkWidget* w,gpointer arg) {
 struct Search* s=arg; if(s->result)return;
 if(GTK_IS_BUTTON(w)) {const char* t=gtk_button_get_label(GTK_BUTTON(w)); if(t&&!strcmp(t,s->text)){s->result=w;return;}}
 if(GTK_IS_CONTAINER(w))gtk_container_foreach(GTK_CONTAINER(w),find_button,s);
}
static gboolean click_idle(gpointer p) {gtk_button_clicked(GTK_BUTTON(p));g_object_unref(p);return FALSE;}
static int click(GtkWidget* w,const char* text) {
 struct Search s={text,NULL};find_button(w,&s);if(!s.result)return 0;
 g_object_ref(s.result);g_idle_add(click_idle,s.result);return 1;
}
static void capture(GtkWidget* w,const char* name) {
 capturing=w; capture_name=name;content=gtk_bin_get_child(GTK_BIN(w));
 GtkAllocation a;gtk_widget_get_allocation(content,&a);g_object_ref(content);
 gtk_container_remove(GTK_CONTAINER(w),content);offscreen=gtk_offscreen_window_new();
 gtk_widget_set_size_request(offscreen,a.width,a.height);gtk_container_add(GTK_CONTAINER(offscreen),content);gtk_widget_show_all(offscreen);
}
static void finish_capture(void) {
 GdkPixbuf* image=gtk_offscreen_window_get_pixbuf(GTK_OFFSCREEN_WINDOW(offscreen));
 char path[4096];snprintf(path,sizeof(path),"%s/%s.png",getenv("ARDOUR_RACK_UI_PROBE"),capture_name);
 GError* err=NULL;if(image){gdk_pixbuf_save(image,path,"png",&err,NULL);g_object_unref(image);}
 if(err){g_printerr("RACK PROBE capture failed: %s\n",err->message);g_error_free(err);}
 gtk_container_remove(GTK_CONTAINER(offscreen),content);gtk_container_add(GTK_CONTAINER(capturing),content);g_object_unref(content);gtk_widget_destroy(offscreen);capturing=NULL;
}
static void find_named (GtkWidget* w,gpointer arg) {
 struct Search* s=arg;if(s->result)return;
 if(!strcmp(gtk_widget_get_name(w),s->text)){s->result=w;return;}
 if(GTK_IS_CONTAINER(w))gtk_container_foreach(GTK_CONTAINER(w),find_named,s);
}
static void find_menu (GtkWidget* w,gpointer arg) {
 struct Search* s=arg;if(s->result)return;
 if(GTK_IS_MENU_ITEM(w)){const char* label=gtk_menu_item_get_label(GTK_MENU_ITEM(w));if(label&&!strcmp(label,s->text)){s->result=w;return;}}
 if(GTK_IS_CONTAINER(w))gtk_container_foreach(GTK_CONTAINER(w),find_menu,s);
}
static int activate_menu(const char* label) {
 struct Search s={label,NULL};GList* list=gtk_window_list_toplevels();
 for(GList* p=list;p;p=p->next)find_menu(p->data,&s);g_list_free(list);
 if(!s.result)return 0;gtk_menu_item_activate(GTK_MENU_ITEM(s.result));return 1;
}
static int popup_control(GtkWidget* w) {
 struct Search s={"processor control knob",NULL};find_named(w,&s);if(!s.result)return 0;
 GdkEvent* ev=gdk_event_new(GDK_BUTTON_PRESS);ev->button.window=g_object_ref(gtk_widget_get_window(s.result));
 ev->button.send_event=TRUE;ev->button.button=3;ev->button.x=5;ev->button.y=5;ev->button.time=GDK_CURRENT_TIME;
 gtk_widget_event(s.result,ev);gdk_event_free(ev);return 1;
}
static int touch_parameter(GtkWidget* w) {
 struct Search s={"ProcessorControlSlider",NULL};find_named(w,&s);if(!s.result)return 0;
 while(GTK_IS_BIN(s.result) && gtk_bin_get_child(GTK_BIN(s.result)))s.result=gtk_bin_get_child(GTK_BIN(s.result));
 for(int release=0;release<2;++release) {
  GdkEvent* ev=gdk_event_new(release?GDK_BUTTON_RELEASE:GDK_BUTTON_PRESS);
  ev->button.window=g_object_ref(gtk_widget_get_window(s.result));ev->button.send_event=TRUE;
  ev->button.button=1;ev->button.x=10;ev->button.y=10;ev->button.time=GDK_CURRENT_TIME;
  gtk_widget_event(s.result,ev);gdk_event_free(ev);
 }
 return 1;
}
static gboolean probe(gpointer unused) {
 if(++ticks>100){g_printerr("RACK PROBE FAILED: timeout stage %d\n",stage);return FALSE;}
 if(stage==0) { GtkWidget* warning=top("Pre-Release Warning");if(warning){gtk_dialog_response(GTK_DIALOG(warning),GTK_RESPONSE_OK);return TRUE;} }
 if(ticks%5==0)g_printerr("RACK PROBE stage %d\n",stage);
 if(stage==0 && ticks%5==0) {
  GList* tops=gtk_window_list_toplevels();for(GList* p=tops;p;p=p->next)if(gtk_widget_get_visible(p->data))g_printerr("RACK PROBE waiting: %s\n",gtk_window_get_title(GTK_WINDOW(p->data)));g_list_free(tops);
 }
 if(capturing){finish_capture();return TRUE;}
 GtkWidget* w;
 switch(stage) {
 case 0: rack=top("Audio Rack");if(!rack)return TRUE;capture(rack,"rack");stage=1;break;
 case 1: if(!click(rack,"Add chain"))return TRUE;stage=2;break;
 case 2: capture(rack,"rack-two-chains");stage=3;break;
 case 3: if(!click(rack,"Map (1)…"))return TRUE;stage=4;break;
 case 4: w=top("Saved macro mappings");if(!w)return TRUE;capture(w,"mappings");stage=5;break;
 case 5: w=top("Saved macro mappings");if(!w)return TRUE;gtk_dialog_response(GTK_DIALOG(w),GTK_RESPONSE_CLOSE);stage=6;break;
 case 6: if(!click(rack,"Parameters"))return TRUE;stage=7;break;
 case 7: w=top("Rack test");if(!w||!click(w,"Learned controls…"))return TRUE;stage=8;break;
 case 8: w=top("Rack test — Learned controls");if(!w||!click(w,"Add parameter"))return TRUE;stage=9;break;
 case 9: w=top("Rack test — Learned controls");if(!w)return TRUE;capture(w,"learned-control");stage=10;break;
 case 10: w=top("Rack test — Learned controls");if(!w||!popup_control(w))return TRUE;stage=11;break;
 case 11: {
  struct Search s={"Remove learned control",NULL};GList* list=gtk_window_list_toplevels();
  for(GList* p=list;p;p=p->next)find_menu(p->data,&s);g_list_free(list);
  if(!s.result)return TRUE;gtk_menu_item_activate(GTK_MENU_ITEM(s.result));stage=12;break;
 }
 case 12: {
  w=top("Rack test — Learned controls");if(!w)return TRUE;
  struct Search s={"processor control knob",NULL};find_named(w,&s);
  if(s.result){g_printerr("RACK PROBE FAILED: learned control was not removed\n");return FALSE;}
  if(!click(w,"Learn"))return TRUE;stage=13;break;
 }
 case 13: w=top("Rack test");if(!w||!touch_parameter(w))return TRUE;stage=14;break;
 case 14: {
  w=top("Rack test — Learned controls");if(!w)return TRUE;
  struct Search s={"processor control knob",NULL};find_named(w,&s);if(!s.result)return TRUE;
  if(!popup_control(rack))return TRUE;stage=15;break;
 }
 case 15: if(!activate_menu("Control type: Choices"))return TRUE;stage=16;break;
 case 16: if(!popup_control(rack))return TRUE;stage=17;break;
 case 17: if(!activate_menu("Control type: Toggle"))return TRUE;stage=18;break;
 case 18: capture(rack,"macro-control-types");stage=19;break;
 case 19: g_printerr("RACK PROBE PASSED: rack, chain addition, mapping dialog, plugin parameters, learned addition, right-click removal, touch learning, and macro control types\n");return FALSE;
 }
 return TRUE;
}
__attribute__((constructor)) static void initialize(void) {
 if(getenv("ARDOUR_RACK_UI_PROBE"))g_timeout_add(600,probe,NULL);
}
