/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test-only injection into a private Ardour GUI session. */
#include <fstream>
#include <ytkmm/window.h>
#include <ytkmm/container.h>
#include "../../gtk2_ardour/plugin_ui.h"
#include "../../gtk2_ardour/keyboard.h"
#include "../../gtk2_ardour/chord_grid_reload.h"

#import <Cocoa/Cocoa.h>

static Gtk::Widget* capture_button(Gtk::Widget* widget) {
 if(widget->get_tooltip_text().find("+K")!=Glib::ustring::npos && widget->get_tooltip_text().find("keyboard")!=Glib::ustring::npos)return widget;
 if(auto* container=dynamic_cast<Gtk::Container*>(widget))for(auto* child:container->get_children())if(auto* hit=capture_button(child))return hit;
 return nullptr;
}
static void post(NSWindow* window,NSUInteger flags=NSCommandKeyMask,bool up=false,bool repeat=false, unsigned short code=40, NSString* chars=@"k") {
 auto* event=[NSEvent keyEventWithType:up?NSKeyUp:NSKeyDown location:NSZeroPoint modifierFlags:flags timestamp:[[NSProcessInfo processInfo] systemUptime] windowNumber:[window windowNumber] context:nullptr characters:chars charactersIgnoringModifiers:chars isARepeat:repeat keyCode:code];
 [NSApp postEvent:event atStart:false];
}
static void post_escape (NSWindow* window, NSUInteger flags = 0)
{
 post (window, flags, false, false, 53, @"\x1b");
 post (window, flags, true, false, 53, @"\x1b");
}

static NSWindow* native_window (Gtk::Window* window)
{
 for (NSWindow* candidate in [NSApp windows]) {
  const char* title = [[candidate title] UTF8String];
  if (title && window->get_title () == title) return candidate;
 }
 return nullptr;
}

static gboolean check(gpointer) {
 static std::unique_ptr<PluginUIWindow> generic;
 static int attempts=0,stage=0,changes=0;static bool captured=false;static std::shared_ptr<ARDOUR::PluginInsert> insert;static PluginUIWindow* window=nullptr;static std::string chunk;
 const char* result=getenv("ARDOUR_CAPTURE_UI_RESULT");if(!result)return FALSE;
 auto fail=[&](const char* why){std::ofstream(result)<<"{\"passed\":false,\"stage\":"<<stage<<",\"reason\":\""<<why<<"\"}";return FALSE;};
 {std::ofstream progress(std::string(result)+".progress");progress<<stage<<" "<<changes;}
 if(++attempts>100)return fail("GUI timeout");
 try {
  auto* engine=ARDOUR::AudioEngine::instance();if(!engine||!engine->session())return TRUE;auto* session=engine->session();if(session->name()!="Capture UI")return fail("Wrong private session");
  if(stage==0){
   std::shared_ptr<ARDOUR::Route> route;for(auto& r:*session->get_routes())if(r->name()=="Reload fixture")route=r;if(!route)return TRUE;
   auto info=std::make_shared<ARDOUR::VST3PluginInfo>();info->name="Chord Grid MIDI";info->creator="Jay";info->unique_id="ABCDEF019182FAEB4A61797343676D69";info->path=getenv("ARDOUR_CAPTURE_UI_PLUGIN");info->n_inputs=ARDOUR::ChanCount(ARDOUR::DataType::AUDIO,2);info->n_outputs=info->n_inputs;
   auto plugin=info->load(*session);if(!plugin)return fail("Plugin load");insert=std::make_shared<ARDOUR::PluginInsert>(*session,*route,plugin);if(route->add_processor(insert,ARDOUR::PreFader))return fail("Insert");std::unique_ptr<XMLNode> state(&plugin->get_state());chunk=ChordGridReload::state_chunk(*state);insert->ShowUI();stage=1;return TRUE;
  }
  if(!window)for(auto* candidate:Gtk::Window::list_toplevels())if(candidate->get_visible())if(auto* plugin=dynamic_cast<PluginUIWindow*>(candidate))window=plugin;
  if(!window)return TRUE;auto* button=capture_button(window);if(!button)return fail("Capture toolbar control missing");
  NSWindow* native=native_window(window);if(!native && window->get_visible())return TRUE;
  if(stage==1){window->pluginui().KeyboardFocused.connect([](bool on){captured=on;++changes;});[NSApp activateIgnoringOtherApps:true];[native makeKeyAndOrderFront:nullptr];post(native);stage=2;return TRUE;}
  if(stage==2){if(!captured||changes!=1||!Gtkmm2ext::Keyboard::some_magic_widget_has_focus())return fail("Cmd-K did not enable capture");if(button->get_tooltip_text().find("normal use")==Glib::ustring::npos)return fail("Toolbar state did not follow shortcut");post(native,NSCommandKeyMask,false,true);post(native,NSCommandKeyMask,true);stage=3;return TRUE;}
  if(stage==3){if(changes!=1)return fail("Repeat or key-up toggled capture");post(native);post(native,NSCommandKeyMask,true);stage=4;return TRUE;}
  if(stage==4){if(captured||changes!=2||Gtkmm2ext::Keyboard::some_magic_widget_has_focus())return fail("Cmd-K did not restore Ardour shortcuts");GdkEventButton event={};event.type=GDK_BUTTON_RELEASE;event.button=1;gboolean handled=false;g_signal_emit_by_name(button->gobj(),"button-release-event",&event,&handled);if(!captured||changes!=3)return fail("Toolbar mouse toggle diverged");post(native,NSCommandKeyMask|NSShiftKeyMask);post(native,NSCommandKeyMask|NSShiftKeyMask,true);stage=5;return TRUE;}
  if(stage==5){if(!captured||changes!=3)return fail("Modified Cmd-K toggled capture");post(native);post(native,NSCommandKeyMask,true);stage=6;return TRUE;}
  if(stage==6){if(captured||changes!=4)return fail("Shortcut did not toggle after mouse");
   auto* down=gdk_event_new(GDK_KEY_PRESS);down->key.keyval=GDK_k;down->key.state=Gtkmm2ext::Keyboard::PrimaryModifier;window->on_key_press_event(&down->key);
   auto* release=gdk_event_new(GDK_KEY_RELEASE);release->key.keyval=GDK_Meta_L;release->key.state=Gtkmm2ext::Keyboard::PrimaryModifier;window->on_key_release_event(&release->key);
   window->on_key_press_event(&down->key);release->key.keyval=GDK_k;window->on_key_release_event(&release->key);gdk_event_free(down);gdk_event_free(release);if(captured||changes!=6)return fail("Command-first release left shortcut stuck");
   post(native);post(native,NSCommandKeyMask,true);stage=7;return TRUE;}
  if (stage == 7) {
   if (!captured) return fail("Capture setup failed");
   post_escape(native, NSShiftKeyMask);
  } else if (stage == 8) {
   if (!window->get_visible() || !captured) return fail("Shift-Escape dismissed editor or released capture");
   post_escape(native);
  } else if (stage == 9) {
   if (window->get_visible() || captured || Gtkmm2ext::Keyboard::some_magic_widget_has_focus()) return fail("Escape did not close captured editor and release keyboard");
   insert->ShowUI();
  } else if (stage == 10) {
   if (!window->get_visible()) return fail("Native editor did not reopen");
   [native makeKeyAndOrderFront:nullptr];
   post_escape(native);
  } else if (stage == 11) {
   if (window->get_visible()) return fail("Escape did not close uncaptured editor");
   insert->ShowUI();
   generic.reset(new PluginUIWindow(insert, false, false));
   if (generic->pluginui().toggle_keyboard_capture()) return fail("Generic editor unexpectedly enabled capture");
   generic->set_title("Generic Escape fixture");
   generic->show_all();
   generic->present();
  } else if (stage == 12) {
   auto* focused = native_window(generic.get());
   if (!focused || !generic->get_visible() || !window->get_visible()) return fail("Two-editor fixture failed");
   [focused makeKeyAndOrderFront:nullptr];
   post_escape(focused);
  } else if (stage == 13) {
   if (generic->get_visible() || !window->get_visible()) return fail("Escape did not close only the focused generic editor");
   std::unique_ptr<XMLNode> state(&insert->plugin()->get_state());
   if (ChordGridReload::state_chunk(*state) != chunk || session->transport_rolling()) return fail("Shortcuts changed plugin or transport state");
   std::ofstream(result)<<"{\"passed\":true,\"native_macos_events\":true,\"both_capture_modes\":true,\"repeat_and_keyup_ignored\":true,\"toolbar_mouse_parity\":true,\"modified_key_unchanged\":true,\"focused_native_and_generic_escape\":true,\"plugin_and_transport_state_preserved\":true}";
   return FALSE;
  }
  ++stage;
 }catch(std::exception const& e){g_printerr("Capture probe: %s\n",e.what());return fail("Exception");}
 return TRUE;
}
__attribute__((constructor)) static void initialize(){if(getenv("ARDOUR_CAPTURE_UI_RESULT"))g_timeout_add(500,check,nullptr);}
