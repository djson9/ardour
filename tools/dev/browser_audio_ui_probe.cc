/* SPDX-License-Identifier: GPL-2.0-or-later
 * Inject only into a disposable Verification GUI. Tests begin after startup. */
#include <fstream>
#include <cmath>
#include <cstring>
#include <ytk/ytk.h>
#include <ytkmm/widget.h>
#include "ardour/audioengine.h"
#include "ardour/session.h"
#include "ardour/auditioner.h"
#include "ardour/playlist.h"
#include "ardour/audioregion.h"
#include "../../gtk2_ardour/browser_audio_preview.h"
static GtkWidget* browser;
static GtkWidget* find(GtkWidget* w,const char* name) {
 if(!strcmp(gtk_widget_get_name(w),name))return w;
 if(GTK_IS_CONTAINER(w)) {
  GList* children=gtk_container_get_children(GTK_CONTAINER(w));GtkWidget* result=nullptr;
  for(GList* p=children;p&&!result;p=p->next)result=find(GTK_WIDGET(p->data),name);
  g_list_free(children);return result;
 }return nullptr;
}
static GtkWidget* top(const char* title) {
 GList* list=gtk_window_list_toplevels();GtkWidget* result=nullptr;
 for(GList* p=list;p;p=p->next)if(gtk_widget_get_visible(GTK_WIDGET(p->data))&&!g_strcmp0(gtk_window_get_title(GTK_WINDOW(p->data)),title)){result=GTK_WIDGET(p->data);break;}
 g_list_free(list);return result;
}
static void click_at(GtkWidget* w,double fraction=.05) {
 GdkEvent* enter=gdk_event_new(GDK_ENTER_NOTIFY);enter->crossing.window=GDK_WINDOW(g_object_ref(gtk_widget_get_window(w)));enter->crossing.mode=GDK_CROSSING_NORMAL;enter->crossing.detail=GDK_NOTIFY_NONLINEAR;gtk_widget_event(w,enter);gdk_event_free(enter);
 for(auto type:{GDK_BUTTON_PRESS,GDK_BUTTON_RELEASE}) {
  GdkEvent* e=gdk_event_new(type);e->button.window=GDK_WINDOW(g_object_ref(gtk_widget_get_window(w)));e->button.send_event=true;e->button.button=1;e->button.x=4+fraction*(w->allocation.width-8);e->button.y=35;
  gtk_widget_event(w,e);gdk_event_free(e);
 }
}
static gboolean click_idle(gpointer w) {click_at(GTK_WIDGET(w));g_object_unref(w);return FALSE;}
static void click(GtkWidget* w) {g_idle_add(click_idle,g_object_ref(w));}
static GtkWidget* control(int column) {
 GtkWidget* table=gtk_widget_get_parent(find(browser,"browser-normalize"));
 GList* children=gtk_container_get_children(GTK_CONTAINER(table));GtkWidget* result=nullptr;
 for(GList* p=children;p;p=p->next) {guint left,top;gtk_container_child_get(GTK_CONTAINER(table),GTK_WIDGET(p->data),"left-attach",&left,"top-attach",&top,NULL);if(left==column&&top==0)result=GTK_WIDGET(p->data);}
 g_list_free(children);return result;
}
static bool select(const char* name) {
 auto* view=GTK_TREE_VIEW(find(browser,"browser-files"));auto* model=gtk_tree_view_get_model(view);GtkTreeIter iter;
 if(!gtk_tree_model_get_iter_first(model,&iter))return false;
 do {char* text=nullptr;gtk_tree_model_get(model,&iter,0,&text,-1);bool match=text&&!strcmp(text,name);g_free(text);if(match){auto* path=gtk_tree_model_get_path(model,&iter);gtk_tree_view_set_cursor(view,path,NULL,FALSE);gtk_tree_path_free(path);return true;}}while(gtk_tree_model_iter_next(model,&iter));return false;
}
static gboolean tick(gpointer) {
 const char* base=getenv("ARDOUR_BROWSER_AUDIO_UI_PROBE");if(!base)return FALSE;
 std::string root=base;
 static int stage=0;static gint64 started=0,stopped=0;static bool restore=false;static float gain=1;static const char* waiting="GUI readiness";
 if(!started) {std::ifstream trigger(root+"/trigger");if(!trigger)return TRUE;std::string mode;trigger>>mode;restore=mode=="restore";remove((root+"/trigger").c_str());started=g_get_monotonic_time();}
 auto finish=[&](const char* error) {std::ofstream(root+"/result.json")<<"{\"passed\":"<<(error?"false":"true")<<",\"stage\":"<<stage<<",\"elapsed\":"<<(g_get_monotonic_time()-started)/1e6<<",\"error\":\""<<(error?error:"")<<"\"}";return FALSE;};
 if(g_get_monotonic_time()-started>8000000)return finish(waiting);
 auto* session=ARDOUR::AudioEngine::instance()->session();if(!session)return TRUE;
 if(session->name().find("Verification")==std::string::npos)return finish("Disposable Verification session required");
 if(stage==0) {
  GList* list=gtk_window_list_toplevels();for(GList* p=list;p;p=p->next) {const char* title=gtk_window_get_title(GTK_WINDOW(p->data));if(title&&strstr(title,"Verification - Ardour"))browser=find(GTK_WIDGET(p->data),"file-browser");if(browser)break;}g_list_free(list);if(!browser)return TRUE;
  gtk_window_present(GTK_WINDOW(gtk_widget_get_toplevel(browser)));
  for(GtkWidget* child=browser,*parent; (parent=gtk_widget_get_parent(child));child=parent)if(GTK_IS_NOTEBOOK(parent))gtk_notebook_set_current_page(GTK_NOTEBOOK(parent),gtk_notebook_page_num(GTK_NOTEBOOK(parent),child));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(3)),FALSE);
  if(restore)return finish(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(browser,"browser-normalize")))?"Normalize preference not restored":nullptr);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(browser,"browser-normalize")),TRUE);
  click(find(browser,"browser-open-folder"));stage=1;return TRUE;
 }
 if(stage==1) {auto* d=top("Open Browser Folder");if(!d)return TRUE;gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d),(root+"/files").c_str());stage=2;return TRUE;}
 if(stage==2) {auto* d=top("Open Browser Folder");if(!d)return TRUE;char* path=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));bool ready=path&&root+"/files"==path;g_free(path);if(ready){gtk_dialog_response(GTK_DIALOG(d),GTK_RESPONSE_OK);stage=3;}return TRUE;}
 auto* widget=find(browser,"browser-audio-waveform");if(!widget)return finish("Missing waveform");
 auto* wave=static_cast<BrowserAudioPreview*>(Glib::wrap(widget));
 auto data=wave->analysis();
 auto actual_gain=[&] {auto p=session->the_auditioner()->playlist();if(p){auto regions=p->region_list();for(auto const& r:*regions)if(auto a=std::dynamic_pointer_cast<ARDOUR::AudioRegion>(r))return a->scale_amplitude();}return -1.f;};
 switch(stage) {
 case 3: waiting="Select quiet audio";if(select("quiet.wav"))stage=4;break;
 case 4:
  waiting="Waveform analysis";if(!data)break;if(!data->error.empty())return finish("Analysis failed");
  if(data->peaks.size()!=2048||std::abs(data->seconds-8)>.001||data->gain<=1)return finish("Waveform values");gain=data->gain;click(control(0));stage=5;break;
 case 5:
  waiting="Normalized playback and advancing waveform";if(!session->is_auditioning()||wave->position()<.01)break;
  if(std::abs(actual_gain()-gain)>.001)return finish("Auditioner did not apply normalized gain");
  click_at(widget,.7);stage=6;break;
 case 6:
  waiting="Waveform seek";if(wave->position()<.7||wave->position()>.9)break;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(browser,"browser-normalize")),FALSE);stage=7;break;
 case 7:
  waiting="Raw playback with preserved position";if(!session->is_auditioning()||std::abs(actual_gain()-1)>.001||wave->position()<.65)break;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(browser,"browser-normalize")),TRUE);stage=8;break;
 case 8: {
  waiting="Reenabled normalization";if(!session->is_auditioning()||std::abs(actual_gain()-gain)>.001||wave->position()<.65)break;
  GdkPixbuf* png=gdk_pixbuf_get_from_drawable(NULL,gtk_widget_get_window(widget),NULL,0,0,0,0,widget->allocation.width,widget->allocation.height);if(png){gdk_pixbuf_save(png,(root+"/waveform.png").c_str(),"png",NULL,NULL);g_object_unref(png);}
  click(control(1));stage=9;break;
 }
 case 9:
  waiting="Stop playback";if(session->is_auditioning())break;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(3)),TRUE);
  if(!select("alternate.wav")||!select("latest.wav"))return finish("Missing rapid-selection fixtures");
  // Direct clicks happen before the worker result can be delivered to GTK.
  click_at(control(1));stopped=g_get_monotonic_time();stage=10;break;
 case 10:
  waiting="Latest selection without stale autoplay";if(!data||data->path!=root+"/files/latest.wav")break;
  if(session->is_auditioning())return finish("Stop allowed delayed autoplay");
  if(g_get_monotonic_time()-stopped<300000)break;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(3)),FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(browser,"browser-normalize")),FALSE);return finish(nullptr);
 }
 return TRUE;
}
__attribute__((constructor)) static void initialize() {if(getenv("ARDOUR_BROWSER_AUDIO_UI_PROBE"))g_timeout_add(30,tick,NULL);}
