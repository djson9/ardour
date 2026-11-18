/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <cppunit/extensions/HelperMacros.h>
#include "test_needing_session.h"
#include "test_util.h"
#include "evoral/midi_events.h"
#include "ardour/audio_rack.h"
#include "ardour/audio_buffer.h"
#include "ardour/midi_buffer.h"
#include "ardour/audioengine.h"
#include "ardour/luaproc.h"
#include "ardour/process_thread.h"
#include "ardour/thread_buffers.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "pbd/xml++.h"
#include <limits>
#include <iostream>
using namespace ARDOUR;
using namespace PBD;

static ChanCount channels (uint32_t audio, uint32_t midi) { ChanCount c; c.set_audio (audio); c.set_midi (midi); return c; }

static const char* script = R"lua(
ardour { ["type"] = "dsp", name = "Rack test", license = "MIT", author = "Test" }
function dsp_ioconfig () return {{audio_in = 1, audio_out = 1}} end
function dsp_params () return {
 {type="input", name="Gain", min=0, max=2, default=1},
 {type="input", name="Toggle", min=0, max=1, default=0, toggled=true},
 {type="input", name="Choice", min=1, max=9, default=1, enum=true, scalepoints={One=1,Three=3,Nine=9}},
 {type="input", name="Integer", min=0, max=10, default=0, integer=true}
} end
function dsp_run (ins, outs, n)
 local g=CtrlPorts:array()[1]
 local input=ins[1]:array(); local output=outs[1]:array()
 for i=1,n do output[i]=input[i]*g end
end
)lua";

class AudioRackTest : public TestNeedingSession {
 CPPUNIT_TEST_SUITE (AudioRackTest);
 CPPUNIT_TEST (mix_and_state);
 CPPUNIT_TEST (mapping);
 CPPUNIT_TEST (latency);
 CPPUNIT_TEST (session_roundtrip);
 CPPUNIT_TEST (transfers);
 CPPUNIT_TEST_SUITE_END ();
public:
 void tearDown () override { AudioEngine::instance ()->set_session (nullptr); TestNeedingSession::tearDown (); }
 std::shared_ptr<PluginInsert> plugin (AudioRack& rack) {
  auto p = std::make_shared<LuaProc> (*AudioEngine::instance (), *_session, script);
  return std::make_shared<PluginInsert> (*_session, rack, p);
 }
 void process (AudioRack& rack, BufferSet& b, float expected) {
  ThreadBuffers scratch; scratch.ensure_buffers (channels(2,1), _session->get_block_size ());
  ProcessThread::BufferScope scope (scratch);
  Mutex::Lock lock (AudioEngine::instance ()->process_lock ());
  for (int i=0;i<64;++i) b.get_audio(0).data()[i] = .25;
  rack.run (b, 0, 64, 1, 64, true);
  for (int i=0;i<64;++i) CPPUNIT_ASSERT_DOUBLES_EQUAL (expected, b.get_audio(0).data()[i], 1e-6);
 }
 void mix_and_state () {
  auto route = _session->new_audio_route (1,1,{},1,"Rack test",PresentationInfo::AudioBus,0).front ();
  auto rack = std::make_shared<AudioRack> (*_session,*route);
  CPPUNIT_ASSERT_EQUAL (0, route->add_processor (rack, PreFader));
  CPPUNIT_ASSERT (rack->add_chain ("Dry"));
  auto p = plugin (*rack); CPPUNIT_ASSERT (rack->add_plugin (0,p));
  BufferSet b; b.ensure_buffers (channels (1,1), 256); b.set_count (channels (1,1));
  uint8_t note[] = {0x90,60,100}; b.get_midi(0).push_back (0, Evoral::MIDI_EVENT, 3, note); const size_t midi_size = b.get_midi(0).size ();
  process (*rack,b,.5); CPPUNIT_ASSERT_EQUAL (midi_size,b.get_midi(0).size ());
  CPPUNIT_ASSERT (rack->set_chain (0,"Wet",.5,false,false)); process (*rack,b,.375);
  CPPUNIT_ASSERT (rack->set_chain (1,"Dry",1,true,false)); process (*rack,b,.125);
  CPPUNIT_ASSERT (rack->set_chain (1,"Dry",1,false,true)); process (*rack,b,.25);
  rack->deactivate (); process (*rack,b,.25); rack->activate ();
  CPPUNIT_ASSERT (!rack->set_chain (1,"bad",std::numeric_limits<float>::quiet_NaN (),false,false));
  rack->rename_macro (0,"Tone"); CPPUNIT_ASSERT (rack->map_parameter (0,p,0,.1,.9));
  rack->macros ()[0].control->set_value (.4,Controllable::NoGroup);
  std::unique_ptr<XMLNode> state (&rack->get_state ());
  auto restored = std::make_shared<AudioRack> (*_session,*route); restored->set_owner (route.get ());
  CPPUNIT_ASSERT_EQUAL (0, restored->set_state (*state,Stateful::current_state_version));
  CPPUNIT_ASSERT_EQUAL (size_t(2),restored->chains ().size ());
  CPPUNIT_ASSERT_EQUAL (std::string("Tone"),restored->macros ()[0].name);
  CPPUNIT_ASSERT_EQUAL (size_t(1),restored->macros ()[0].mappings.size ());
  CPPUNIT_ASSERT_EQUAL (p->id ().to_s (),restored->chains ()[0]->plugins[0]->id ().to_s ());
  { Mutex::Lock lock (AudioEngine::instance ()->process_lock ()); CPPUNIT_ASSERT (restored->configure_io (channels (1,0),channels (1,0))); }
  process (*restored,b,.25);
  restored->set_macro_presentation (0,"choices");
  restored->regenerate_ids ();
  CPPUNIT_ASSERT (restored->chains ()[0]->plugins[0]->id () != p->id ());
  CPPUNIT_ASSERT_EQUAL (restored->chains ()[0]->plugins[0]->id ().to_s (),restored->macros ()[0].mappings[0].plugin_id);
  std::unique_ptr<XMLNode> copied (&restored->get_state ());
  auto copy = std::make_shared<AudioRack> (*_session,*route); copy->set_owner (route.get ());
  CPPUNIT_ASSERT_EQUAL (0,copy->set_state (*copied,Stateful::current_state_version));
  CPPUNIT_ASSERT_EQUAL (std::string("choices"),copy->macros ()[0].presentation);

  CPPUNIT_ASSERT (rack->remove_plugin (0,0)); CPPUNIT_ASSERT (rack->macros ()[0].mappings.empty ());
  CPPUNIT_ASSERT (rack->remove_chain (1)); CPPUNIT_ASSERT (!rack->remove_chain (0));
  state->set_property ("rack-version", 999); CPPUNIT_ASSERT_EQUAL (-1,restored->set_state (*state,Stateful::current_state_version));
 }
 void latency () {
  auto route = _session->new_audio_route (1,1,{},1,"Latency",PresentationInfo::AudioBus,0).front ();
  auto rack = std::make_shared<AudioRack> (*_session,*route);
  CPPUNIT_ASSERT_EQUAL (0,route->add_processor (rack,PreFader));
  CPPUNIT_ASSERT (rack->add_chain ("Dry"));
  std::string delayed = script;
  delayed += R"lua(
local ring = {0,0,0,0,0,0,0}; local pos = 1
function dsp_latency () return 7 end
function dsp_run (ins, outs, n)
 local input=ins[1]:array(); local output=outs[1]:array()
 for i=1,n do local x=input[i]; output[i]=ring[pos]; ring[pos]=x; pos=pos%7+1 end
end
)lua";
  auto lp = std::make_shared<LuaProc> (*AudioEngine::instance (),*_session,delayed);
  auto p = std::make_shared<PluginInsert> (*_session,*rack,lp);
  CPPUNIT_ASSERT (rack->add_plugin (0,p));
  BufferSet b; b.ensure_buffers (channels(1,0),256); b.set_count(channels(1,0));
  ThreadBuffers scratch; scratch.ensure_buffers (channels(2,1), _session->get_block_size ());
  ProcessThread::BufferScope scope (scratch);
  Mutex::Lock lock (AudioEngine::instance ()->process_lock ());
  b.silence (64,0); rack->run (b,0,64,1,64,true); rack->update_latency ();
  CPPUNIT_ASSERT_EQUAL (samplecnt_t(7),rack->signal_latency ());
  b.silence (64,0); b.get_audio(0).data()[0]=1;
  rack->run (b,64,128,1,64,true);
  for (int i=0;i<64;++i) CPPUNIT_ASSERT_DOUBLES_EQUAL (i==7 ? 2. : 0.,b.get_audio(0).data()[i],1e-6);
  b.silence (64,0); b.get_audio(0).data()[60]=1; rack->run(b,128,192,1,64,true);
  for (int i=0;i<64;++i) CPPUNIT_ASSERT_DOUBLES_EQUAL (0.,b.get_audio(0).data()[i],1e-6);
  b.silence (64,0); rack->run(b,192,256,1,64,true);
  for (int i=0;i<64;++i) CPPUNIT_ASSERT_DOUBLES_EQUAL (i==3 ? 2. : 0.,b.get_audio(0).data()[i],1e-6);

 }
 void session_roundtrip () {
  const std::string path = _session->path ();
  std::cout << "Rack fixture: " << path << std::endl;
  std::string rack_id;
  {
   auto route = _session->new_audio_route (1,1,{},1,"Saved rack",PresentationInfo::AudioBus,0).front ();
   auto rack = std::make_shared<AudioRack> (*_session,*route);
   CPPUNIT_ASSERT_EQUAL (0,route->add_processor (rack,PreFader));
   auto p = plugin (*rack); CPPUNIT_ASSERT (rack->add_plugin (0,p));
   CPPUNIT_ASSERT (rack->map_parameter (0,p,0)); rack->rename_macro(0,"Saved macro");
   rack_id = rack->id ().to_s ();
   CPPUNIT_ASSERT_EQUAL (0,_session->save_state (""));
  }
  AudioEngine::instance ()->set_session (nullptr); delete _session;
  _session = load_session (path,"test_session");
  std::shared_ptr<AudioRack> restored;
  for (auto const& r : *_session->get_routes ()) r->foreach_processor ([&] (std::weak_ptr<Processor> p) {
   if (auto rack = std::dynamic_pointer_cast<AudioRack> (p.lock ())) if (rack->id ().to_s () == rack_id) restored = rack;
  });
  CPPUNIT_ASSERT (restored); CPPUNIT_ASSERT_EQUAL (std::string("Saved macro"),restored->macros ()[0].name);
  CPPUNIT_ASSERT_EQUAL (size_t(1),restored->macros ()[0].mappings.size ());
  CPPUNIT_ASSERT_EQUAL (size_t(1),restored->chains ()[0]->plugins.size ());
 }
 void transfers () {
  auto a = _session->new_audio_route (1,1,{},1,"Transfer source",PresentationInfo::AudioBus,0).front ();
  auto b = _session->new_audio_route (2,2,{},1,"Transfer destination",PresentationInfo::AudioBus,0).front ();
  auto rack = std::make_shared<AudioRack> (*_session,*a);
  auto other = std::make_shared<AudioRack> (*_session,*b);
  CPPUNIT_ASSERT_EQUAL (0,a->add_processor (rack,PreFader));
  CPPUNIT_ASSERT_EQUAL (0,b->add_processor (other,PreFader));
  CPPUNIT_ASSERT (rack->add_chain ("Second"));
  auto p = plugin (*rack), q = plugin (*rack);
  CPPUNIT_ASSERT (rack->add_plugin (0,p)); CPPUNIT_ASSERT (rack->add_plugin (0,q));
  auto control = p->automation_control (Evoral::Parameter (PluginAutomation,0,0));
  control->set_value (.73,Controllable::NoGroup);
  p->deactivate ();
  auto learned = new XMLNode ("LearnedParameters"); learned->add_child ("Parameter")->set_property ("id",0); p->add_extra_xml (*learned);
  const auto id = p->id ();
  CPPUNIT_ASSERT (rack->map_parameter (0,p,0));
  control->set_value (.73,Controllable::NoGroup);
  CPPUNIT_ASSERT (a->move_rack_plugin (p,rack,rack,1,0));
  CPPUNIT_ASSERT (rack->chains ()[1]->plugins.front () == p);
  CPPUNIT_ASSERT (rack->chains ()[0]->plugins.front () == q);
  CPPUNIT_ASSERT_EQUAL (size_t(1),rack->macros ()[0].mappings.size ());
  CPPUNIT_ASSERT (a->move_rack_plugin (p,rack,rack,0,0));
  CPPUNIT_ASSERT (a->move_rack_plugin (p,rack,rack,0,2));
  CPPUNIT_ASSERT (rack->chains ()[0]->plugins[1] == p);
  CPPUNIT_ASSERT (a->move_rack_plugin (p,rack,rack,0,2)); // no-op end drop
  CPPUNIT_ASSERT (!a->move_rack_plugin (p,rack,rack,7,0));
  CPPUNIT_ASSERT (!a->move_rack_plugin (p,rack,rack,0,99));
  CPPUNIT_ASSERT (a->move_rack_plugin (p,rack,{},0,0,rack));
  CPPUNIT_ASSERT (a->processor_by_id (id) == p);
  CPPUNIT_ASSERT (rack->macros ()[0].mappings.empty ());
  CPPUNIT_ASSERT (p->automation_control (Evoral::Parameter (PluginAutomation,0,0)) == control);
  CPPUNIT_ASSERT (p->extra_xml ("LearnedParameters"));
  CPPUNIT_ASSERT (!p->active ());
  CPPUNIT_ASSERT_DOUBLES_EQUAL (.73,control->get_value (),1e-6);
  const double value = control->get_value ();
  CPPUNIT_ASSERT (a->move_rack_plugin (p,{},rack,1,0));
  CPPUNIT_ASSERT (!a->processor_by_id (id));
  CPPUNIT_ASSERT (rack->map_parameter (0,p,0));
  control->set_value (value,Controllable::NoGroup);
  CPPUNIT_ASSERT (b->move_rack_plugin (p,rack,other,0,0));
  CPPUNIT_ASSERT (other->contains (p)); CPPUNIT_ASSERT (!rack->contains (p));
  CPPUNIT_ASSERT (rack->macros ()[0].mappings.empty ());
  CPPUNIT_ASSERT (p->owner () == b.get ());
  CPPUNIT_ASSERT (b->move_rack_plugin (p,other,{},0,0,other));
  CPPUNIT_ASSERT (p->time_domain_parent () == b.get ());
  CPPUNIT_ASSERT_EQUAL (0,b->remove_processor (other)); other.reset ();
  CPPUNIT_ASSERT (p->id () == id); CPPUNIT_ASSERT (p->automation_control (Evoral::Parameter (PluginAutomation,0,0)) == control);
  CPPUNIT_ASSERT_DOUBLES_EQUAL (value, control->get_value (), 1e-6);
  // Force a plugin configuration failure on the mono destination. A failed reconfiguration must restore both routes.
  std::string stereo = script;
  auto config = stereo.find ("audio_in = 1, audio_out = 1"); stereo.replace (config,std::string ("audio_in = 1, audio_out = 1").size (),"audio_in = -1, audio_out = -1");
  stereo += "\nfunction dsp_configure (ins, outs) assert(ins:n_audio() == 2) end\n";
  auto stereo_plugin = std::make_shared<LuaProc> (*AudioEngine::instance (),*_session,stereo);
  auto incompatible = std::make_shared<PluginInsert> (*_session,*b,stereo_plugin);
  CPPUNIT_ASSERT_EQUAL (0,b->add_processor (incompatible,p));
  CPPUNIT_ASSERT (!a->move_rack_plugin (incompatible,{},rack,0,0));
  CPPUNIT_ASSERT (b->processor_by_id (incompatible->id ()) == incompatible);
  CPPUNIT_ASSERT (!rack->contains (incompatible));
  CPPUNIT_ASSERT (incompatible->owner () == b.get ());
  CPPUNIT_ASSERT_EQUAL (size_t(1),rack->chains ()[0]->plugins.size ());
  CPPUNIT_ASSERT_EQUAL (0,_session->save_state (""));
  std::unique_ptr<XMLNode> saved (&b->get_state ());
  bool found = false;
  for (auto node : saved->children ()) if (node->name () == "Processor") {
   std::string saved_id; if (node->get_property ("id",saved_id) && saved_id == id.to_s ()) found = true;
  }
  CPPUNIT_ASSERT (found);
  const std::string session_path = _session->path ();
  p.reset (); q.reset (); rack.reset (); a.reset (); b.reset (); control.reset (); incompatible.reset (); stereo_plugin.reset ();
  AudioEngine::instance ()->set_session (nullptr); delete _session;
  _session = load_session (session_path,"test_session");
  std::shared_ptr<PluginInsert> restored;
  for (auto const& route : *_session->get_routes ()) {
   if (auto candidate = std::dynamic_pointer_cast<PluginInsert> (route->processor_by_id (id))) restored = candidate;
  }
  CPPUNIT_ASSERT (restored); CPPUNIT_ASSERT (restored->extra_xml ("LearnedParameters"));
  CPPUNIT_ASSERT (!restored->active ());
  CPPUNIT_ASSERT_DOUBLES_EQUAL (value,restored->automation_control (Evoral::Parameter (PluginAutomation,0,0))->get_value (),1e-6);
 }
 void mapping () {
  auto route = _session->new_audio_route (1,1,{},1,"Mapping",PresentationInfo::AudioBus,0).front ();
  auto rack = std::make_shared<AudioRack> (*_session,*route);
  CPPUNIT_ASSERT_EQUAL (0, route->add_processor (rack,PreFader));
  auto p = plugin (*rack); CPPUNIT_ASSERT (rack->add_plugin (0,p));
  for (uint32_t id=0;id<4;++id) CPPUNIT_ASSERT (rack->map_parameter (0,p,id));
  BufferSet b; b.ensure_buffers (channels (1,0),256); b.set_count (channels (1,0));
  auto set = [&] (double v) { rack->macros ()[0].control->set_value (v,Controllable::NoGroup); process (*rack,b,float(.5*v)); };
  set (.2); CPPUNIT_ASSERT_DOUBLES_EQUAL (0,p->plugin ()->get_parameter (1),1e-6); CPPUNIT_ASSERT_DOUBLES_EQUAL (3,p->plugin ()->get_parameter (2),1e-6); CPPUNIT_ASSERT_DOUBLES_EQUAL (2,p->plugin ()->get_parameter (3),1e-6);
  set (.9); CPPUNIT_ASSERT_DOUBLES_EQUAL (1,p->plugin ()->get_parameter (1),1e-6); CPPUNIT_ASSERT_DOUBLES_EQUAL (9,p->plugin ()->get_parameter (2),1e-6);
  CPPUNIT_ASSERT (rack->map_parameter (1,p,0,1,0)); CPPUNIT_ASSERT_EQUAL (size_t(3),rack->macros ()[0].mappings.size ());
  rack->macros ()[1].control->set_value (.25,Controllable::NoGroup); process (*rack,b,.375);
  CPPUNIT_ASSERT (!rack->map_parameter (9,p,0)); CPPUNIT_ASSERT (!rack->map_parameter (1,p,999));
  CPPUNIT_ASSERT (!rack->map_parameter (1,p,0,-.1,1));
  CPPUNIT_ASSERT (rack->unmap_parameter (1,0)); rack->macros ()[1].control->set_value (.5,Controllable::NoGroup); process (*rack,b,.375);
 }
};
CPPUNIT_TEST_SUITE_REGISTRATION (AudioRackTest);
