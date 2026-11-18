/* SPDX-License-Identifier: GPL-2.0-or-later
 * Test-only GTK interaction/rendering, injected into a disposable GUI process.
 * Screenshots contain only the tested window, never the user's desktop. */
#include <ytk/ytk.h>
#include <ydk/gdkkeysyms.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static GtkWidget *rack, *capturing, *content, *offscreen, *original_parent;
static const char *capture_name;
static int stage, ticks, dragged;
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
 capturing=w; capture_name=name;content=GTK_IS_WINDOW(w)?gtk_bin_get_child(GTK_BIN(w)):w;original_parent=gtk_widget_get_parent(content);
 GtkAllocation a;gtk_widget_get_allocation(content,&a);g_object_ref(content);
 gtk_container_remove(GTK_CONTAINER(original_parent),content);offscreen=gtk_offscreen_window_new();
 gtk_widget_set_size_request(offscreen,a.width,a.height);gtk_container_add(GTK_CONTAINER(offscreen),content);gtk_widget_show_all(offscreen);
}
static void finish_capture(void) {
 GdkPixbuf* image=gtk_offscreen_window_get_pixbuf(GTK_OFFSCREEN_WINDOW(offscreen));
 char path[4096];snprintf(path,sizeof(path),"%s/%s.png",getenv("ARDOUR_RACK_UI_PROBE"),capture_name);
 GError* err=NULL;if(image){gdk_pixbuf_save(image,path,"png",&err,NULL);g_object_unref(image);}
 if(err){g_printerr("RACK PROBE capture failed: %s\n",err->message);g_error_free(err);}
 gtk_container_remove(GTK_CONTAINER(offscreen),content);gtk_container_add(GTK_CONTAINER(original_parent),content);g_object_unref(content);gtk_widget_destroy(offscreen);capturing=NULL;
}
static void find_named (GtkWidget* w,gpointer arg) {
 struct Search* s=arg;if(s->result)return;
 if(gtk_widget_get_mapped(w)&&!strcmp(gtk_widget_get_name(w),s->text)){s->result=w;return;}
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
static GtkWidget* named(GtkWidget* w,const char* name) {
 if(!w)return NULL;
 struct Search s={name,NULL};find_named(w,&s);return s.result;
}
static int select_row(GtkWidget* view,int index) {
 if(!view)return 0;GtkTreePath* path=gtk_tree_path_new_from_indices(index,-1);
 gtk_tree_view_set_cursor(GTK_TREE_VIEW(view),path,NULL,FALSE);gtk_tree_path_free(path);return 1;
}
static int select_chain(int index) {return select_row(named(rack,"AudioRackChainList"),index);}
static int plugin_count(void) {
 GtkWidget* view=named(rack,"AudioRackPluginList");if(!view)return -1;
 return gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(view)),NULL);
}
static int plugin_name(int index,const char* expected) {
 GtkWidget* view=named(rack,"AudioRackPluginList");if(!view)return 0;
 GtkTreeModel* model=gtk_tree_view_get_model(GTK_TREE_VIEW(view));GtkTreeIter iter;
 if(!gtk_tree_model_iter_nth_child(model,&iter,NULL,index))return 0;
 char* name=NULL;gtk_tree_model_get(model,&iter,1,&name,-1);int result=name&&!strcmp(name,expected);g_free(name);return result;
}
static int add_to_second_chain(void) {if(!select_chain(1))return 0;return click(rack,"Add plugin…");}
static void return_key(GtkWidget* w) {
 GdkEvent* ev=gdk_event_new(GDK_KEY_PRESS);ev->key.window=g_object_ref(gtk_widget_get_window(w));
 ev->key.send_event=TRUE;ev->key.keyval=GDK_Return;gtk_widget_event(w,ev);gdk_event_free(ev);
}
static int search_effect(GtkWidget* w,const char* name) {
 struct Search s={"PluginSelectorSearch",NULL};find_named(w,&s);if(!s.result)return 0;
 gtk_entry_set_text(GTK_ENTRY(s.result),name);return_key(s.result);return 1;
}
static int select_delay(GtkWidget* w,int keyboard) {
 struct Search s={"PluginSelectorDisplay",NULL};find_named(w,&s);if(!s.result)return 0;
 GtkTreeModel* model=gtk_tree_view_get_model(GTK_TREE_VIEW(s.result));GtkTreeIter iter;
 if(!gtk_tree_model_get_iter_first(model,&iter))return 0;
 GtkTreePath* path=gtk_tree_model_get_path(model,&iter);
 gtk_tree_view_set_cursor(GTK_TREE_VIEW(s.result),path,NULL,FALSE);
 if(keyboard)return_key(s.result);
 else gtk_tree_view_row_activated(GTK_TREE_VIEW(s.result),path,gtk_tree_view_get_column(GTK_TREE_VIEW(s.result),1));
 gtk_tree_path_free(path);return 1;
}
static int drag_request(int id, GtkWidget* source, int source_row, GtkWidget* destination, int destination_row, int cancel) {
 if(!source||!destination||!gtk_widget_get_mapped(source)||!gtk_widget_get_mapped(destination))return 0;
 gtk_window_present(GTK_WINDOW(gtk_widget_get_toplevel(source)));
 GtkWidget* widgets[]={source,destination}; int rows[]={source_row,destination_row}; int x[2],y[2];
 for(int i=0;i<2;++i) {
  if(GTK_IS_TREE_VIEW(widgets[i])&&rows[i]>=0) {
   GtkTreePath* path=gtk_tree_path_new_from_indices(rows[i],-1);GdkRectangle rect;
   gtk_tree_view_get_cell_area(GTK_TREE_VIEW(widgets[i]),path,NULL,&rect);gtk_tree_path_free(path);
   gdk_window_get_origin(gtk_tree_view_get_bin_window(GTK_TREE_VIEW(widgets[i])),&x[i],&y[i]);
   x[i]+=30;y[i]+=rect.y+(i==1&&widgets[i]==named(rack,"AudioRackPluginList")?2:rect.height/2);
  } else {
   GtkWidget* top=gtk_widget_get_toplevel(widgets[i]);int dx,dy;GtkAllocation a;
   gdk_window_get_origin(gtk_widget_get_window(top),&x[i],&y[i]);
   gtk_widget_translate_coordinates(widgets[i],top,0,0,&dx,&dy);gtk_widget_get_allocation(widgets[i],&a);
   x[i]+=dx+25;y[i]+=dy+(rows[i]==-2?2:a.height/2);
  }
 }
 char path[4096],temp[4096];snprintf(path,sizeof(path),"%s/drag-request.json",getenv("ARDOUR_RACK_UI_PROBE"));
 snprintf(temp,sizeof(temp),"%s/drag-request.tmp",getenv("ARDOUR_RACK_UI_PROBE"));
 FILE* f=fopen(temp,"w");if(!f)return 0;fprintf(f,"{\"id\":%d,\"from\":[%d,%d],\"to\":[%d,%d],\"cancel\":%d}",id,x[0],y[0],x[1],y[1],cancel);fclose(f);rename(temp,path);return 1;
}
static int drag_done(int id) {
 char path[4096];snprintf(path,sizeof(path),"%s/drag-done",getenv("ARDOUR_RACK_UI_PROBE"));
 FILE* f=fopen(path,"r");int value=0;if(f){fscanf(f,"%d",&value);fclose(f);}return value==id;
}
static GtkWidget* editor_window(void) {GtkWidget* w=top("*Rack Verification - Ardour");return w?w:top("Rack Verification - Ardour");}
static gboolean probe(gpointer unused) {
 if(++ticks>210){g_printerr("RACK PROBE FAILED: timeout stage %d\n",stage);return FALSE;}
 if(stage==0) { GtkWidget* warning=top("Pre-Release Warning");if(warning){gtk_dialog_response(GTK_DIALOG(warning),GTK_RESPONSE_OK);return TRUE;} }
 if(ticks%5==0)g_printerr("RACK PROBE stage %d\n",stage);
 if(stage==0 && ticks%5==0) {
  GList* tops=gtk_window_list_toplevels();for(GList* p=tops;p;p=p->next)if(gtk_widget_get_visible(p->data))g_printerr("RACK PROBE waiting: %s\n",gtk_window_get_title(GTK_WINDOW(p->data)));g_list_free(tops);
 }
 if(capturing){finish_capture();return TRUE;}
 GtkWidget* discovery=top("Discover VST/AU Plugins?");
 if(discovery){gtk_dialog_response(GTK_DIALOG(discovery),GTK_RESPONSE_NO);return TRUE;}
 GtkWidget* w;
 switch(stage) {
 case 0:
  if(getenv("ARDOUR_RACK_UI_EMBEDDED")) {
   w=top("Rack Verification - Ardour");if(!w)w=top("*Rack Verification - Ardour");
   if(!w)return TRUE;rack=named(w,"AudioRackControls");if(!rack||!gtk_widget_get_mapped(rack))return TRUE;
   if(top("Audio Rack")){g_printerr("RACK PROBE FAILED: unexpected floating rack window\n");return FALSE;}
  } else rack=top("Audio Rack");
  if(!rack)return TRUE;capture(rack,"rack");stage=1;break;
 case 1: if(!click(rack,"Add chain"))return TRUE;stage=2;break;
 case 2:
  if(plugin_count()!=0){g_printerr("RACK PROBE FAILED: new chain should have an empty plugin list\n");return FALSE;}
  capture(rack,"rack-two-chains");select_chain(0);stage=3;break;
 case 3: if(!select_chain(0)||!click(rack,"Map (1)…"))return TRUE;stage=4;break;
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
 case 19:
  w=top("Rack test — Learned controls");if(w)gtk_widget_hide(w);
  w=top("Rack test");if(w)gtk_widget_hide(w);
  if(!add_to_second_chain())return TRUE;stage=20;break;
 case 20: w=top("Plugin Selector");if(!w||!search_effect(w,"ACE Delay"))return TRUE;stage=21;break;
 case 21: w=top("Plugin Selector");if(!w||!select_delay(w,0))return TRUE;stage=22;break;
 case 22: w=top("Plugin Selector");if(!w||!click(w,"Insert Plugin(s)"))return TRUE;stage=23;break;
 case 23: {
  int count=plugin_count();
  if(top("Plugin Selector")||count!=1||!plugin_name(0,"ACE Delay")){g_printerr("RACK PROBE FAILED: Insert Plugin(s) did not close the chooser and add ACE Delay (count=%d)\n",count);return FALSE;}
  if(!add_to_second_chain())return TRUE;stage=24;break;
 }
 case 24: w=top("Plugin Selector");if(!w||!search_effect(w,"ACE EQ"))return TRUE;stage=25;break;
 case 25: w=top("Plugin Selector");if(!w||!select_delay(w,1))return TRUE;stage=26;break;
 case 26: {
  int count=plugin_count();
  if(top("Plugin Selector")||count!=2||!plugin_name(1,"ACE EQ")){g_printerr("RACK PROBE FAILED: keyboard insertion or chooser queue reset (count=%d)\n",count);return FALSE;}
  if(!add_to_second_chain())return TRUE;stage=27;break;
 }
 case 27: w=top("Plugin Selector");if(!w)return TRUE;gtk_dialog_response(GTK_DIALOG(w),GTK_RESPONSE_CANCEL);stage=28;break;
 case 28: {
  int count=plugin_count();
  if(top("Plugin Selector")||count!=2){g_printerr("RACK PROBE FAILED: cancel changed the chain or left the chooser open\n");return FALSE;}
  capture(rack,"plugins-in-chain");stage=29;break;
 }
 case 29: if(!click(rack,"Down"))return TRUE;stage=30;break;
 case 30:
  if(!plugin_name(0,"ACE EQ")||!plugin_name(1,"ACE Delay")){g_printerr("RACK PROBE FAILED: reorder selected plugin down\n");return FALSE;}
  if(!click(rack,"Up"))return TRUE;stage=31;break;
 case 31:
  if(!plugin_name(0,"ACE Delay")||!plugin_name(1,"ACE EQ")){g_printerr("RACK PROBE FAILED: reorder selected plugin up\n");return FALSE;}
  if(!click(rack,"Remove"))return TRUE;stage=32;break;
 case 32:
  if(plugin_count()!=1||!plugin_name(0,"ACE EQ")){g_printerr("RACK PROBE FAILED: remove selected plugin\n");return FALSE;}
  select_chain(0);stage=33;break;
 case 33:
  if(plugin_count()!=1||!plugin_name(0,"Rack test")){g_printerr("RACK PROBE FAILED: first chain list\n");return FALSE;}
  select_chain(1);stage=34;break;
 case 34:
  if(plugin_count()!=1||!plugin_name(0,"ACE EQ")){g_printerr("RACK PROBE FAILED: second chain list\n");return FALSE;}
  if(getenv("ARDOUR_RACK_UI_EMBEDDED")) {
   w=top("*Rack Verification - Ardour");if(!w)w=top("Rack Verification - Ardour");capture(w,"rack-bottom-panel");
  } else capture(rack,"rack-selected-chain");
  stage=35;break;
 case 35:
  if(getenv("ARDOUR_RACK_UI_DRAG")&&!dragged){select_chain(0);stage=40;break;}
  if(getenv("ARDOUR_RACK_UI_EMBEDDED")){g_printerr("RACK PROBE READY: select empty track\n");stage=36;break;}
  g_printerr("RACK PROBE PASSED: floating rack, macros, learned controls, chain selection, insertion, reorder and removal\n");return FALSE;
 case 40:
  if(!drag_request(1,named(rack,"AudioRackPluginList"),0,named(rack,"AudioRackChainList"),1,0))return TRUE;
  stage=41;break;
 case 41:
  if(!drag_done(1))return TRUE;
  if(plugin_count()!=2||!plugin_name(0,"ACE EQ")||!plugin_name(1,"Rack test")){g_printerr("RACK PROBE FAILED: mouse drag between chains (count=%d, first_eq=%d, second_rack=%d)\n",plugin_count(),plugin_name(0,"ACE EQ"),plugin_name(1,"Rack test"));return FALSE;}
  if(!drag_request(2,named(rack,"AudioRackPluginList"),1,named(rack,"AudioRackPluginList"),0,0))return TRUE;
  stage=42;break;
 case 42:
  if(!drag_done(2))return TRUE;
  if(!plugin_name(0,"Rack test")||!plugin_name(1,"ACE EQ")){g_printerr("RACK PROBE FAILED: mouse reorder\n");return FALSE;}
  if(!drag_request(3,named(rack,"AudioRackPluginList"),0,named(editor_window(),"ProcessorList"),-2,0))return TRUE;
  stage=43;break;
 case 43:
  if(!drag_done(3))return TRUE;
  rack=named(editor_window(),"AudioRackControls");if(!rack)return TRUE;
  select_chain(1);
  if(plugin_count()!=1||!plugin_name(0,"ACE EQ")){g_printerr("RACK PROBE FAILED: mouse move out of rack\n");return FALSE;}
  select_chain(0);
  if(plugin_count()!=0){g_printerr("RACK PROBE FAILED: source chain after move\n");return FALSE;}
  if(!drag_request(4,named(named(editor_window(),"ProcessorList"),"processor prefader"),-1,named(rack,"AudioRackPluginList"),-1,0))return TRUE;
  stage=44;break;
 case 44:
  if(!drag_done(4))return TRUE;
  rack=named(editor_window(),"AudioRackControls");if(!rack)return TRUE;select_chain(0);
  if(plugin_count()!=1||!plugin_name(0,"Rack test")){g_printerr("RACK PROBE FAILED: mouse move into empty chain\n");return FALSE;}
  if(!drag_request(5,named(rack,"AudioRackPluginList"),0,named(rack,"AudioRackChainList"),1,1))return TRUE;
  stage=45;break;
 case 45:
  if(!drag_done(5))return TRUE;
  if(plugin_count()!=1||!plugin_name(0,"Rack test")){g_printerr("RACK PROBE FAILED: cancelled drag changed chain\n");return FALSE;}
  dragged=1;g_printerr("RACK PROBE PASSED: native mouse drags between chains, reorder, out to track, into empty chain, cancel\n");stage=35;break;
 case 36:
  w=top("*Rack Verification - Ardour");if(!w)w=top("Rack Verification - Ardour");
  if(!w||named(w,"AudioRackControls"))return TRUE;
  g_printerr("RACK PROBE READY: return to rack track\n");stage=37;break;
 case 37:
  w=top("*Rack Verification - Ardour");if(!w)w=top("Rack Verification - Ardour");
  if(!w)return TRUE;rack=named(w,"AudioRackControls");if(!rack||!gtk_widget_get_mapped(rack))return TRUE;
  if(plugin_count()!=1||!plugin_name(0,"Rack test")){g_printerr("RACK PROBE FAILED: rack state after track switch\n");return FALSE;}
  select_chain(1);
  if(plugin_count()!=1||!plugin_name(0,"ACE EQ")){g_printerr("RACK PROBE FAILED: second chain after track switch\n");return FALSE;}
  g_printerr("RACK PROBE PASSED: bottom panel, macros, learned controls, chain selection, insertion, reorder, removal and track-switch lifecycle\n");return FALSE;
 }
 return TRUE;
}
__attribute__((constructor)) static void initialize(void) {
 if(getenv("ARDOUR_RACK_UI_PROBE"))g_timeout_add(600,probe,NULL);
}
