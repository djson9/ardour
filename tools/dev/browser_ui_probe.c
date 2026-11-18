/* SPDX-License-Identifier: GPL-2.0-or-later
 * Inject only into a disposable Verification GUI. The trigger starts a bounded
 * test after app startup; no production hooks or user session changes. */
#include <ytk/ytk.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static GtkWidget *browser;
static int stage, active;
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
 active=0;
}
static gboolean click_idle(gpointer data) {
 GtkWidget *w=data;
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
static void respond(GtkWidget *dialog,int response) {gtk_dialog_response(GTK_DIALOG(dialog),response);}
static void edit(GtkWidget *dialog,const char *text) {gtk_entry_set_text(GTK_ENTRY(find(dialog,"browser-extension-entry")),text);}
static gboolean tick(gpointer unused) {
 if(!active) {
  if(!g_file_test(trigger,G_FILE_TEST_EXISTS))return TRUE;
  FILE *f=fopen(trigger,"r");if(!f)return TRUE;mode[0]=0;fscanf(f,"%31s",mode);fclose(f);remove(trigger);
  started=g_get_monotonic_time();active=1;stage=0;browser=NULL;
 }
 if(g_get_monotonic_time()-started>8000000){finish("8 second GUI deadline");return TRUE;}
 GtkWidget *dialog;GtkTreeIter i;
 switch(stage) {
 case 0: {
  GList *list=gtk_window_list_toplevels();
  for(GList *p=list;p;p=p->next) {
   const char *title=gtk_window_get_title(GTK_WINDOW(p->data));
   if(title&&strstr(title,"Verification - Ardour")) {browser=find(p->data,"file-browser");if(browser)break;}
  }
  g_list_free(list);if(!browser)return TRUE;
  GtkWidget *child=browser,*parent;
  while((parent=gtk_widget_get_parent(child))) {if(GTK_IS_NOTEBOOK(parent))gtk_notebook_set_current_page(GTK_NOTEBOOK(parent),gtk_notebook_page_num(GTK_NOTEBOOK(parent),child));child=parent;}
  stage=!strcmp(mode,"restore")?30:1;break;
 }
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
  if(!row("Nested")||!row("Piano.WaV")||!row("Chords.MID")||!row("Bass.flac")||!files(3)||row("notes.txt")||row("pipe.wav")){finish("Default audio/MIDI directory listing");break;}
  row("Nested",&i);{
   GtkTreePath *p=gtk_tree_model_get_path(model(),&i);gtk_tree_view_expand_row(GTK_TREE_VIEW(find(browser,"browser-files")),p,FALSE);gtk_tree_path_free(p);
  }
  stage=5;break;
 case 5:
  row("Nested",&i);if(gtk_tree_model_iter_n_children(model(),&i)!=1){finish("Lazy subfolder whitelist");break;}
  if(strcmp(gtk_label_get_text(GTK_LABEL(find(browser,"browser-directory"))),directory)){finish("Expansion changed root folder");break;}
  if(click("browser-refresh"))stage=6;break;
 case 6:
  if(!row("Nested")||!row("Piano.WaV")){finish("Refresh lost root folder");break;}
  if(click("browser-file-types"))stage=7;break;
 case 7:
  dialog=top("Browser File Types");if(!dialog)break;
  edit(dialog,"*.MID, .mid");respond(dialog,GTK_RESPONSE_OK);stage=8;break;
 case 8:
  if(!files(1)||!row("Chords.MID")||!row("Nested")){finish("Custom MIDI whitelist");break;}
  if(click("browser-file-types"))stage=9;break;
 case 9:
  dialog=top("Browser File Types");if(!dialog)break;
  edit(dialog,"*");respond(dialog,GTK_RESPONSE_OK);stage=10;break;
 case 10:
  dialog=top("Browser File Types");if(!dialog||!strlen(gtk_label_get_text(GTK_LABEL(find(dialog,"browser-filter-error"))))){finish("Invalid filter was accepted");break;}
  edit(dialog,"wav");respond(dialog,GTK_RESPONSE_CANCEL);stage=11;break;
 case 11:
  if(!files(1)||!row("Chords.MID")){finish("Cancel changed filter");break;}
  if(click("browser-file-types"))stage=12;break;
 case 12:
  dialog=top("Browser File Types");if(!dialog)break;
  edit(dialog,"");respond(dialog,GTK_RESPONSE_OK);stage=13;break;
 case 13:
  if(!files(0)||!row("Nested")||!strlen(gtk_label_get_text(GTK_LABEL(find(browser,"browser-status"))))){finish("Empty whitelist behavior");break;}
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
  if(!files(1)||!row("Chords.MID")){finish("Final saved filter");break;}
  finish(NULL);break;
 case 30:
  if(strcmp(gtk_label_get_text(GTK_LABEL(find(browser,"browser-directory"))),directory)||!files(1)||!row("Chords.MID")||!row("Nested")){finish("Folder/filter not restored at startup");break;}
  finish(NULL);break;
 }
 return TRUE;
}
__attribute__((constructor)) static void initialize(void) {
 const char *root=getenv("ARDOUR_BROWSER_UI_PROBE");if(!root)return;
 snprintf(directory,sizeof(directory),"%s/files",root);snprintf(trigger,sizeof(trigger),"%s/trigger",root);snprintf(result,sizeof(result),"%s/result.json",root);
 g_timeout_add(50,tick,NULL);
}
