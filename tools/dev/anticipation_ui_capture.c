/* Test-only in-process GTK rendering. No desktop/screen capture, production hook,
 * installed application or user configuration is required. */
#include <ytk/ytk.h>
#include <string.h>
#include <stdlib.h>
static GtkWidget* editor;
static GtkWidget* offscreen;
static GtkWidget* content;
static int stage;
static gboolean select_page(GtkTreeModel* model,GtkTreePath* path,GtkTreeIter* iter,gpointer data) {
 gchar* text=NULL;gtk_tree_model_get(model,iter,0,&text,-1);
 gboolean found=text&&!strcmp(text,"Performance");g_free(text);
 if(found){gtk_tree_view_set_cursor(GTK_TREE_VIEW(data),path,NULL,FALSE);return TRUE;}return FALSE;
}
static void select_tree(GtkWidget* widget,gpointer unused){
 if(GTK_IS_TREE_VIEW(widget)){GtkTreeModel* model=gtk_tree_view_get_model(GTK_TREE_VIEW(widget));if(model&&gtk_tree_model_get_column_type(model,0)==G_TYPE_STRING)gtk_tree_model_foreach(model,select_page,widget);}
 if(GTK_IS_CONTAINER(widget))gtk_container_foreach(GTK_CONTAINER(widget),select_tree,NULL);
}
static gboolean capture(gpointer unused){
 const char* path=getenv("ARDOUR_ANTICIPATION_UI_CAPTURE");if(!path)return FALSE;
 if(stage==0){
  GList* tops=gtk_window_list_toplevels();
  for(GList* p=tops;p;p=p->next){const char* title=gtk_window_get_title(GTK_WINDOW(p->data));if(title&&!strcmp(title,"Ardour - Preferences")){editor=p->data;break;}}
  g_list_free(tops);if(!editor||!gtk_widget_get_visible(editor))return TRUE;
  select_tree(editor,NULL);stage=1;return TRUE;
 }
 if(stage==1){
  content=gtk_bin_get_child(GTK_BIN(editor));if(!content)return FALSE;
  GtkAllocation a;gtk_widget_get_allocation(content,&a);g_object_ref(content);gtk_container_remove(GTK_CONTAINER(editor),content);
  offscreen=gtk_offscreen_window_new();gtk_widget_set_size_request(offscreen,a.width,a.height);gtk_container_add(GTK_CONTAINER(offscreen),content);gtk_widget_show_all(offscreen);stage=2;return TRUE;
 }
 GdkPixbuf* image=gtk_offscreen_window_get_pixbuf(GTK_OFFSCREEN_WINDOW(offscreen));
 GError* err=NULL;if(image){gdk_pixbuf_save(image,path,"png",&err,NULL);g_object_unref(image);}if(err){g_printerr("UI capture: %s\n",err->message);g_error_free(err);}
 gtk_container_remove(GTK_CONTAINER(offscreen),content);gtk_container_add(GTK_CONTAINER(editor),content);g_object_unref(content);gtk_widget_destroy(offscreen);
 g_printerr("Anticipation UI capture completed: %s\n",path);return FALSE;
}
__attribute__((constructor)) static void initialize(void){if(getenv("ARDOUR_ANTICIPATION_UI_CAPTURE"))g_timeout_add(700,capture,NULL);}
