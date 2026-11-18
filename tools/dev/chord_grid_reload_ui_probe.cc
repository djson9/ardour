/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test-only injection into a private Ardour GUI process; never a live session. */
#include <fstream>
#include <ytkmm/window.h>
#include <ytkmm/container.h>
#include <ytkmm/label.h>
#include "widgets/ardour_button.h"
#include "../../gtk2_ardour/chord_grid_reload.h"

static ArdourWidgets::ArdourButton* find_reload(Gtk::Widget* widget) {
 if(auto* b=dynamic_cast<ArdourWidgets::ArdourButton*>(widget))if(b->get_text()=="Reload"&&b->get_visible())return b;
 if(auto* c=dynamic_cast<Gtk::Container*>(widget))for(auto* child:c->get_children())if(auto* b=find_reload(child))return b;
 return nullptr;
}
static void describe(Gtk::Widget* w,std::ostream& out){
 if(auto* l=dynamic_cast<Gtk::Label*>(w))out<<"  "<<l->get_text()<<"\n";
 if(auto* c=dynamic_cast<Gtk::Container*>(w))for(auto* child:c->get_children())describe(child,out);
}
static gboolean check(gpointer) {
 static int attempts=0,stage=0;static std::shared_ptr<ARDOUR::PluginInsert> original;static std::shared_ptr<ARDOUR::Route> route;static std::string state;static bool old_button_destroyed=false;
 const char* result=getenv("ARDOUR_RELOAD_UI_RESULT");if(!result)return FALSE;
 auto fail=[&](const char* why){std::ofstream(result)<<"{\"passed\":false,\"reason\":\""<<why<<"\"}";return FALSE;};
 {std::ofstream progress(std::string(result)+".progress");progress<<"stage="<<stage<<" attempts="<<attempts<<"\n";for(auto* w:Gtk::Window::list_toplevels())if(w->get_visible()){progress<<w->get_title()<<"\n";describe(w,progress);}}
 if(++attempts>80)return fail("GUI timeout");
 try {
  auto* engine=ARDOUR::AudioEngine::instance();if(!engine||!engine->session())return TRUE;
  auto* session=engine->session();if(session->name()!="Reload UI")return fail("Wrong session");
  if(stage==0){
   for(auto& r:*session->get_routes())if(r->name()=="Reload fixture")route=r;
   if(!route)return TRUE;
   auto info=std::make_shared<ARDOUR::VST3PluginInfo>();info->name="Chord Grid MIDI";info->creator="Jay";info->unique_id="ABCDEF019182FAEB4A61797343676D69";info->path=getenv("ARDOUR_RELOAD_UI_PLUGIN");info->n_inputs=ARDOUR::ChanCount(ARDOUR::DataType::AUDIO,2);info->n_outputs=info->n_inputs;
   auto plugin=info->load(*session);if(!plugin)return fail("Plugin load");
   original=std::make_shared<ARDOUR::PluginInsert>(*session,*route,plugin);if(route->add_processor(original,ARDOUR::PreFader))return fail("Plugin insertion");
   std::unique_ptr<XMLNode> saved(&plugin->get_state());state=ChordGridReload::state_chunk(*saved);stage=1;return TRUE;
  }
  if(stage==1){original->ShowUI();stage=2;return TRUE;}
  ArdourWidgets::ArdourButton* button=nullptr;
  for(auto* w:Gtk::Window::list_toplevels())if(w->get_visible())if(auto* b=find_reload(w))button=b;
  if(!button)return TRUE;
  if(stage==2){g_signal_connect(button->gobj(),"destroy",G_CALLBACK(+[](GtkObject*,gpointer v){*static_cast<bool*>(v)=true;}),&old_button_destroyed);button->signal_clicked();stage=3;return TRUE;}
  std::shared_ptr<ARDOUR::PluginInsert> replacement;
  route->foreach_processor([&](std::weak_ptr<ARDOUR::Processor> p){auto q=std::dynamic_pointer_cast<ARDOUR::PluginInsert>(p.lock());if(q&&q->id()==original->id())replacement=q;});
  if(!replacement||replacement==original)return TRUE;
  std::unique_ptr<XMLNode> saved(&replacement->plugin()->get_state());if(ChordGridReload::state_chunk(*saved)!=state)return fail("State changed");
  bool native_window=false;for(auto* w:Gtk::Window::list_toplevels())if(w->get_visible()&&w->get_title().find("Chord Grid")!=std::string::npos&&find_reload(w))native_window=true;
  if(!native_window||!old_button_destroyed)return TRUE;
  std::ofstream(result)<<"{\"passed\":true,\"toolbar_button_found\":true,\"button_action_replaced_instance\":true,\"state_preserved\":true,\"editor_reopened\":true}";return FALSE;
 }catch(std::exception const& e){g_printerr("Reload UI probe: %s\n",e.what());return fail("Exception");}
}
__attribute__((constructor)) static void initialize(){if(getenv("ARDOUR_RELOAD_UI_RESULT"))g_timeout_add(500,check,nullptr);}
