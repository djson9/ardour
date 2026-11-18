/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test-only injection into a private Ardour GUI session. */
#import <Cocoa/Cocoa.h>
#include <fstream>
#include "../../gtk2_ardour/chord_grid_reload.h"

static NSView* find_host(NSView* view) {
 if([NSStringFromClass([view class]) isEqualToString:@"ArdourChordGridHostView"])return view;
 for(NSView* child in [view subviews])if(auto* hit=find_host(child))return hit;
 return nullptr;
}
static id name_field(id element) {
 if([element respondsToSelector:@selector(accessibilityValue)] && [[element accessibilityRole] isEqualToString:NSAccessibilityTextAreaRole] && [[element accessibilityValue] isEqualToString:@"New session"])return element;
 if([element respondsToSelector:@selector(accessibilityChildren)])for(id child in [element accessibilityChildren])if(id match=name_field(child))return match;
 return nullptr;
}
static void key(NSView* view,NSString* text,NSUInteger flags=0,bool repeat=false,bool up=false) {
 NSEvent* event=[NSEvent keyEventWithType:up?NSKeyUp:NSKeyDown location:NSZeroPoint modifierFlags:flags timestamp:[[NSProcessInfo processInfo] systemUptime] windowNumber:[[view window] windowNumber] context:nullptr characters:text charactersIgnoringModifiers:text isARepeat:repeat keyCode:[text isEqualToString:@" "]?49:36];
 if(up)[view keyUp:event];else [view keyDown:event];
}
static gboolean check(gpointer) {
 static int attempts=0,stage=0;static std::shared_ptr<ARDOUR::PluginInsert> insert;static std::string beforeText;
 const char* result=getenv("ARDOUR_SPACE_UI_RESULT");if(!result)return FALSE;
 auto fail=[&](const char* why){std::ofstream(result)<<"{\"passed\":false,\"stage\":"<<stage<<",\"reason\":\""<<why<<"\"}";return FALSE;};
 if(++attempts>100)return fail("GUI timeout");
 try {
  auto* engine=ARDOUR::AudioEngine::instance();if(!engine||!engine->session())return TRUE;auto* session=engine->session();
  if(session->name()!="Space UI")return fail("Wrong private session");
  if(stage==0){
   std::shared_ptr<ARDOUR::Route> route;for(auto& r:*session->get_routes())if(r->name()=="Reload fixture")route=r;if(!route)return TRUE;
   auto info=std::make_shared<ARDOUR::VST3PluginInfo>();info->name="Chord Grid MIDI";info->creator="Jay";info->unique_id="ABCDEF019182FAEB4A61797343676D69";info->path=getenv("ARDOUR_SPACE_UI_PLUGIN");info->n_inputs=ARDOUR::ChanCount(ARDOUR::DataType::AUDIO,2);info->n_outputs=info->n_inputs;
   auto plugin=info->load(*session);if(!plugin)return fail("Plugin load");insert=std::make_shared<ARDOUR::PluginInsert>(*session,*route,plugin);if(route->add_processor(insert,ARDOUR::PreFader))return fail("Insert");insert->ShowUI();stage=1;return TRUE;
  }
  NSView* host=nullptr;for(NSWindow* w in [NSApp windows])if((host=find_host([w contentView])))break;if(!host||![[host subviews] count])return TRUE;
  NSView* view=[[host subviews] objectAtIndex:0];
  if(stage==1){if(session->transport_rolling())return fail("Fixture already rolling");[NSApp activateIgnoringOtherApps:YES];[[view window] makeKeyAndOrderFront:nullptr];[[view window] makeFirstResponder:view];key(view,@" ");stage=2;return TRUE;}
  if(stage==2){if(!session->transport_rolling())return fail("Space did not play host");key(view,@" ",0,true);key(view,@" ",0,false,true);stage=3;return TRUE;}
  if(stage==3){if(!session->transport_rolling())return fail("Repeat or release toggled transport");key(view,@" ");stage=4;return TRUE;}
  if(stage==4){
   if(session->transport_rolling())return fail("Second space did not stop host");std::unique_ptr<XMLNode> saved(&insert->plugin()->get_state());beforeText=ChordGridReload::state_chunk(*saved);
   id field=name_field(view);if(!field||![field accessibilityPerformPress])return fail("Name field not editable");
   stage=5;return TRUE;
  }
  if(stage==5){auto* bitmap=[view bitmapImageRepForCachingDisplayInRect:[view bounds]];[view cacheDisplayInRect:[view bounds] toBitmapImageRep:bitmap];[[bitmap representationUsingType:NSPNGFileType properties:@{}] writeToFile:[NSString stringWithUTF8String:(std::string(result)+".png").c_str()] atomically:YES];key(view,@" ");stage=6;return TRUE;}
  if(stage==6){if(session->transport_rolling())return fail("Space in text editor started host");key(view,@"\r");stage=7;return TRUE;}
  if(stage==7){std::unique_ptr<XMLNode> saved(&insert->plugin()->get_state());if(ChordGridReload::state_chunk(*saved)==beforeText)return fail("Text entry was not exercised");
   std::ofstream(result)<<"{\"passed\":true,\"native_juce_view\":true,\"space_play_stop\":true,\"repeat_and_keyup_ignored\":true,\"text_space_preserved\":true}";return FALSE;}
 }catch(std::exception const& e){g_printerr("Space probe: %s\n",e.what());return fail("Exception");}
 return TRUE;
}
__attribute__((constructor)) static void initialize(){if(getenv("ARDOUR_SPACE_UI_RESULT"))g_timeout_add(500,check,nullptr);}
