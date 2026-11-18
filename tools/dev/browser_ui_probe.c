/* SPDX-License-Identifier: GPL-2.0-or-later
 * Inject only into a disposable Verification GUI. The trigger starts a bounded
 * test after app startup; no production hooks or user session changes. */
#include <ytk/ytk.h>
#include <ydk/gdkkeysyms.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static GtkWidget *browser;
static int stage, active;
static const char *pending_failure;
static gint64 started;
static char directory[4096], trigger[4096], result[4096], mode[32];
struct Search {const char *name; GtkWidget *found;};
static void find_named(GtkWidget *w, gpointer data) {
 struct Search *s=data;
 if(s->found)return;
 if(!strcmp(gtk_widget_get_name(w),s->name)){s->found=w;return;}
 if(GTK_IS_CONTAINER(w))gtk_container_foreach(GTK_CONTAINER(w),find_named,s);
}
static GtkWidget *find(GtkWidget *w,const char *name) {
 struct Search s={name,NULL};find_named(w,&s);return s.found;
}
static GtkWidget *top(const char *title) {
 GList *list=gtk_window_list_toplevels();GtkWidget *w=NULL;
 for(GList *p=list;p;p=p->next)if(gtk_widget_get_visible(p->data)&&!strcmp(gtk_window_get_title(GTK_WINDOW(p->data)),title)){w=p->data;break;}
 g_list_free(list);return w;
}
static void finish(const char *error) {
 FILE *f=fopen(result,"w");if(f){fprintf(f,"{\"passed\":%s,\"stage\":%d,\"elapsed\":%.3f,\"error\":\"%s\"}\n",error?"false":"true",stage,(g_get_monotonic_time()-started)/1000000.,error?error:"");fclose(f);}
 if(error && browser) {
  GtkTreeSelection *selection=gtk_tree_view_get_selection(GTK_TREE_VIEW(find(browser,"browser-files")));
  g_printerr("BROWSER KEY FAILURE stage=%d selected=%d focus=%s\n",stage,gtk_tree_selection_count_selected_rows(selection),gtk_widget_get_name(gtk_window_get_focus(GTK_WINDOW(gtk_widget_get_toplevel(browser)))));
 }
 active=0;
}
static gboolean click_idle(gpointer data) {
 GtkWidget *w=data;
 GdkEvent *enter=gdk_event_new(GDK_ENTER_NOTIFY);
 enter->crossing.window=g_object_ref(gtk_widget_get_window(w));enter->crossing.send_event=TRUE;
 enter->crossing.mode=GDK_CROSSING_NORMAL;enter->crossing.detail=GDK_NOTIFY_NONLINEAR;
 gtk_widget_event(w,enter);gdk_event_free(enter);
 for(int release=0;release<2;release++) {
  GdkEvent *e=gdk_event_new(release?GDK_BUTTON_RELEASE:GDK_BUTTON_PRESS);
  e->button.window=g_object_ref(gtk_widget_get_window(w));e->button.send_event=TRUE;
  e->button.button=1;e->button.x=8;e->button.y=8;e->button.time=GDK_CURRENT_TIME;
  gtk_widget_event(w,e);gdk_event_free(e);
 }
 g_object_unref(w);return FALSE;
}
static int click(const char *name) {
 GtkWidget *w=find(browser,name);if(!w||!gtk_widget_get_mapped(w))return 0;
 g_idle_add(click_idle,g_object_ref(w));return 1;
}
static GtkTreeModel *model(void) {return gtk_tree_view_get_model(GTK_TREE_VIEW(find(browser,"browser-files")));}
static int row(const char *name, GtkTreeIter *out) {
 GtkTreeModel *m=model();GtkTreeIter i;
 if(!gtk_tree_model_get_iter_first(m,&i))return 0;
 do {char *s=NULL;gtk_tree_model_get(m,&i,0,&s,-1);int match=s&&!strcmp(s,name);g_free(s);if(match){if(out)*out=i;return 1;}}while(gtk_tree_model_iter_next(m,&i));
 return 0;
}
static int files(int expected) {
 GtkTreeModel *m=model();GtkTreeIter i;int count=0;
 if(gtk_tree_model_get_iter_first(m,&i))do {gboolean file=FALSE;gtk_tree_model_get(m,&i,3,&file,-1);if(file)count++;}while(gtk_tree_model_iter_next(m,&i));
 return count==expected;
}
/* Ask the runner for a native macOS key event. It activates only the private
 * app and acknowledges dispatch before this probe checks the actual GTK tree. */
static void key(guint keyval,guint modifiers) {
 char path[4096];snprintf(path,sizeof(path),"%s/key-request",getenv("ARDOUR_BROWSER_UI_PROBE"));
 FILE *f=fopen(path,"w");if(f){fprintf(f,"%u %u",keyval,modifiers);fclose(f);}
}
static int cursor(const char *name) {
 GtkTreePath *path=NULL;GtkTreeIter iter;char *text=NULL;
 gtk_tree_view_get_cursor(GTK_TREE_VIEW(find(browser,"browser-files")),&path,NULL);
 if(!path)return 0;
 if(gtk_tree_model_get_iter(model(),&iter,path))gtk_tree_model_get(model(),&iter,0,&text,-1);
 int match=text&&!strcmp(text,name);g_free(text);gtk_tree_path_free(path);return match;
}
static void select_row(const char *name) {
 GtkTreeIter iter;if(!row(name,&iter)){finish("Missing keyboard fixture row");return;}
 GtkTreePath *path=gtk_tree_model_get_path(model(),&iter);
 gtk_tree_view_set_cursor(GTK_TREE_VIEW(find(browser,"browser-files")),path,NULL,FALSE);gtk_tree_path_free(path);
}
static int expanded(const char *name) {
 GtkTreeIter iter;if(!row(name,&iter))return 0;
 GtkTreePath *path=gtk_tree_model_get_path(model(),&iter);
 int result=gtk_tree_view_row_expanded(GTK_TREE_VIEW(find(browser,"browser-files")),path);gtk_tree_path_free(path);return result;
}
static void respond(GtkWidget *dialog,int response) {gtk_dialog_response(GTK_DIALOG(dialog),response);}
static void edit(GtkWidget *dialog,const char *text) {gtk_entry_set_text(GTK_ENTRY(find(dialog,"browser-extension-entry")),text);}
static void disable_autoplay(GtkWidget *w,gpointer unused) {
 if(GTK_IS_CHECK_BUTTON(w)) {
  const char *label=gtk_button_get_label(GTK_BUTTON(w));
  if(label&&!strcmp(label,"Auto-play"))gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w),FALSE);
 }
 if(GTK_IS_CONTAINER(w))gtk_container_foreach(GTK_CONTAINER(w),disable_autoplay,NULL);
}
static gboolean tick(gpointer unused) {
 if(!active) {
  if(!g_file_test(trigger,G_FILE_TEST_EXISTS))return TRUE;
  FILE *f=fopen(trigger,"r");if(!f)return TRUE;mode[0]=0;fscanf(f,"%31s",mode);fclose(f);remove(trigger);
  started=g_get_monotonic_time();active=1;stage=0;browser=NULL;pending_failure=NULL;
 }
 if(g_get_monotonic_time()-started>8000000){finish(pending_failure?pending_failure:"8 second GUI deadline");return TRUE;}
 char pending_key[4096];snprintf(pending_key,sizeof(pending_key),"%s/key-request",getenv("ARDOUR_BROWSER_UI_PROBE"));
 if(g_file_test(pending_key,G_FILE_TEST_EXISTS))return TRUE;
 GtkWidget *dialog;GtkTreeIter i;
 switch(stage) {
 case 0: {
  GList *list=gtk_window_list_toplevels();
  for(GList *p=list;p;p=p->next) {
   const char *title=gtk_window_get_title(GTK_WINDOW(p->data));
   if(title&&strstr(title,"Verification - Ardour")) {browser=find(p->data,"file-browser");if(browser)break;}
  }
  g_list_free(list);if(!browser)return TRUE;
  gtk_window_present(GTK_WINDOW(gtk_widget_get_toplevel(browser)));
  disable_autoplay(browser,NULL);

  GtkWidget *child=browser,*parent;
  while((parent=gtk_widget_get_parent(child))) {if(GTK_IS_NOTEBOOK(parent))gtk_notebook_set_current_page(GTK_NOTEBOOK(parent),gtk_notebook_page_num(GTK_NOTEBOOK(parent),child));child=parent;}
  stage=!strcmp(mode,"restore")?30:40;break;
 }
 case 40: if(click("browser-file-types"))stage=41;break;
 case 41:
  dialog=top("Browser File Types");if(!dialog)break;
  respond(dialog,GTK_RESPONSE_APPLY);stage=42;break;
 case 42:
  dialog=top("Browser File Types");if(!dialog)break;
  respond(dialog,GTK_RESPONSE_OK);stage=1;break;
 case 1: if(click("browser-open-folder"))stage=2;break;
 case 2:
  dialog=top("Open Browser Folder");if(!dialog)break;
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog),directory);stage=3;break;
 case 3: {
  dialog=top("Open Browser Folder");if(!dialog)break;
  char *path=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));int ready=path&&!strcmp(path,directory);g_free(path);
  if(ready){respond(dialog,GTK_RESPONSE_OK);stage=4;}break;
 }
 case 4:
  if(!strcmp(mode,"keys")){stage=50;break;}
  if(!row("Nested",NULL)||!row("Piano.WaV",NULL)||!row("Chords.MID",NULL)||!row("Bass.flac",NULL)||!files(3)||row("notes.txt",NULL)||row("pipe.wav",NULL)){finish("Default audio/MIDI directory listing");break;}
  row("Nested",&i);{
   GtkTreePath *p=gtk_tree_model_get_path(model(),&i);gtk_tree_view_expand_row(GTK_TREE_VIEW(find(browser,"browser-files")),p,FALSE);gtk_tree_path_free(p);
  }
  stage=5;break;
 case 5:
  row("Nested",&i);if(gtk_tree_model_iter_n_children(model(),&i)!=1){finish("Lazy subfolder whitelist");break;}
  if(strcmp(gtk_label_get_text(GTK_LABEL(find(browser,"browser-directory"))),directory)){finish("Expansion changed root folder");break;}
  if(click("browser-refresh"))stage=6;break;
 case 6:
  if(!row("Nested",NULL)||!row("Piano.WaV",NULL)){finish("Refresh lost root folder");break;}
  if(click("browser-file-types"))stage=7;break;
 case 7:
  dialog=top("Browser File Types");if(!dialog)break;
  edit(dialog,"*.MID, .mid");respond(dialog,GTK_RESPONSE_OK);stage=8;break;
 case 8:
  if(!files(1)||!row("Chords.MID",NULL)||!row("Nested",NULL)){finish("Custom MIDI whitelist");break;}
  if(click("browser-file-types"))stage=9;break;
 case 9:
  dialog=top("Browser File Types");if(!dialog)break;
  edit(dialog,"*");respond(dialog,GTK_RESPONSE_OK);stage=10;break;
 case 10:
  dialog=top("Browser File Types");if(!dialog||!strlen(gtk_label_get_text(GTK_LABEL(find(dialog,"browser-filter-error"))))){finish("Invalid filter was accepted");break;}
  edit(dialog,"wav");respond(dialog,GTK_RESPONSE_CANCEL);stage=11;break;
 case 11:
  if(!files(1)||!row("Chords.MID",NULL)){finish("Cancel changed filter");break;}
  if(click("browser-file-types"))stage=12;break;
 case 12:
  dialog=top("Browser File Types");if(!dialog)break;
  edit(dialog,"");respond(dialog,GTK_RESPONSE_OK);stage=13;break;
 case 13:
  if(!files(0)||!row("Nested",NULL)||!strlen(gtk_label_get_text(GTK_LABEL(find(browser,"browser-status"))))){finish("Empty whitelist behavior");break;}
  if(click("browser-file-types"))stage=14;break;
 case 14:
  dialog=top("Browser File Types");if(!dialog)break;
  respond(dialog,GTK_RESPONSE_APPLY);stage=15;break;
 case 15:
  dialog=top("Browser File Types");if(!dialog)break;
  respond(dialog,GTK_RESPONSE_OK);stage=16;break;
 case 16:
  if(!files(3)){finish("Reset audio/MIDI defaults");break;}
  if(click("browser-file-types"))stage=17;break;
 case 17:
  dialog=top("Browser File Types");if(!dialog)break;
  edit(dialog,"mid");respond(dialog,GTK_RESPONSE_OK);stage=18;break;
 case 18:
  if(!files(1)||!row("Chords.MID",NULL)){finish("Final saved filter");break;}
  finish(NULL);break;
 case 50:
  if(!gtk_widget_has_focus(find(browser,"browser-files")))return TRUE;
  if(gtk_window_get_focus(GTK_WINDOW(gtk_widget_get_toplevel(browser)))!=find(browser,"browser-files")){pending_failure="Folder chooser did not return focus to files";return TRUE;}
  select_row("Nested");key(GDK_Down,0);stage=51;break;
 case 51:
  if(!cursor("Bass.flac")){pending_failure="Down did not select next file";return TRUE;}
  key(GDK_Up,0);stage=52;break;
 case 52:
  if(!cursor("Nested")){pending_failure="Up did not select previous row";return TRUE;}
  key(GDK_Right,0);stage=53;break;
 case 53:
  if(!expanded("Nested")){pending_failure="Right did not expand folder";return TRUE;}
  key(GDK_Down,0);stage=54;break;
 case 54:
  if(!cursor("Inside.wav")){pending_failure="Down did not enter expanded folder";return TRUE;}
  key(GDK_Left,0);stage=55;break;
 case 55:
  if(!cursor("Nested")){pending_failure="Left did not select parent row";return TRUE;}
  key(GDK_Left,0);stage=56;break;
 case 56:
  if(expanded("Nested")){pending_failure="Left did not collapse folder";return TRUE;}
  key(GDK_Home,0);stage=57;break;
 case 57:
  if(!cursor("..")){pending_failure="Home did not select first row";return TRUE;}
  key(GDK_Up,0);stage=58;break;
 case 58:
  if(!cursor("..")){pending_failure="Top boundary changed browser cursor";return TRUE;}
  key(GDK_End,0);stage=63;break;
 case 63:
  key(GDK_Up,0);stage=64;break;
 case 64:
  key(GDK_Up,0);stage=65;break;
 case 65:
  if(!cursor("Bass.flac")){pending_failure="Keyboard selection anchor";return TRUE;}
  key(GDK_Down,GDK_SHIFT_MASK);stage=59;break;
 case 59:
  if(gtk_tree_selection_count_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(find(browser,"browser-files"))))!=2){pending_failure="Shift-Down selection did not extend";return TRUE;}
  select_row("Nested");key(GDK_Return,0);stage=60;break;
 case 60:
  if(!row("Inside.wav",NULL)||row("Nested",NULL)){pending_failure="Enter did not open selected folder";return TRUE;}
  select_row("..");key(GDK_Return,0);stage=61;break;
 case 61:
  if(strcmp(gtk_label_get_text(GTK_LABEL(find(browser,"browser-directory"))),directory)){pending_failure="Enter on parent did not return to root";return TRUE;}
  key(GDK_End,0);stage=62;break;
 case 62:
  if(!cursor("Piano.WaV")){pending_failure="End did not select last file";return TRUE;}
  finish(NULL);break;
 case 30:
  if(strcmp(gtk_label_get_text(GTK_LABEL(find(browser,"browser-directory"))),directory)||!files(1)||!row("Chords.MID",NULL)||!row("Nested",NULL)){finish("Folder/filter not restored at startup");break;}
  finish(NULL);break;
 }
 return TRUE;
}
__attribute__((constructor)) static void initialize(void) {
 const char *root=getenv("ARDOUR_BROWSER_UI_PROBE");if(!root)return;
 snprintf(directory,sizeof(directory),"%s/files",root);snprintf(trigger,sizeof(trigger),"%s/trigger",root);snprintf(result,sizeof(result),"%s/result.json",root);
 g_timeout_add(50,tick,NULL);
}
