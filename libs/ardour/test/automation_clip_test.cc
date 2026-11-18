/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <cppunit/extensions/HelperMacros.h>
#include "test_needing_session.h"
#include "test_util.h"
#include "pbd/memento_command.h"
#include "ardour/automation_clip.h"
#include "ardour/automation_clip_source.h"
#include "ardour/audio_track.h"
#include "ardour/audioregion.h"
#include "ardour/gain_control.h"
#include "ardour/automation_list.h"
#include "ardour/playlist.h"
#include "ardour/source_factory.h"
#include "ardour/region_factory.h"
#include "ardour/session.h"
#include "ardour/audioengine.h"
using namespace ARDOUR;
using namespace Temporal;
class AutomationClipTest : public TestNeedingSession {
 CPPUNIT_TEST_SUITE (AutomationClipTest);
 CPPUNIT_TEST (timeline);
 CPPUNIT_TEST_SUITE_END ();
public:
 void tearDown () override {
  /* Model tests need no audible fade-out; detach under the engine lock. */
  AudioEngine::instance()->set_session(nullptr);
  TestNeedingSession::tearDown();
 }
 void timeline () {
  std::string path = _session->path(), track_id, target_id;
  {
  AutomationClips clips (*_session);
  auto track = clips.new_track(); CPPUNIT_ASSERT(track);
  auto targets = _session->new_audio_track (1, 1, {}, 2, "Target", PresentationInfo::max_order, Normal, false);
  auto c1 = targets.front()->gain_control(), c2 = targets.back()->gain_control();
  track_id = track->id().to_s(); target_id = targets.back()->id().to_s();
  auto r = clips.add_clip (track, timepos_t(1000), timecnt_t(1000)); CPPUNIT_ASSERT(r);
  std::string error;
  CPPUNIT_ASSERT (clips.map(track, c1, .2, .8, false, error));
  CPPUNIT_ASSERT (clips.map(track, c2, .1, .9, true, error));
  auto value = [] (std::shared_ptr<AutomationControl> c, samplepos_t t) { return c->alist()->eval (timepos_t(t)); };
  CPPUNIT_ASSERT (c1->automation_state() == Play);
  CPPUNIT_ASSERT_DOUBLES_EQUAL (1., value(c1,999), 1e-6);
  CPPUNIT_ASSERT_DOUBLES_EQUAL (.2, value(c1,1000), 1e-6);
  CPPUNIT_ASSERT_DOUBLES_EQUAL (.5, value(c1,1500), 1e-6);
  CPPUNIT_ASSERT_DOUBLES_EQUAL (.9, value(c2,1000), 1e-6);
  CPPUNIT_ASSERT_DOUBLES_EQUAL (.7, value(c2,1250), 1e-6);
  CPPUNIT_ASSERT_DOUBLES_EQUAL (1., value(c1,2000), 1e-6);
  r->set_position(timepos_t(2000));
  CPPUNIT_ASSERT_DOUBLES_EQUAL (1., value(c1,1500), 1e-6);
  CPPUNIT_ASSERT_DOUBLES_EQUAL (.5, value(c1,2500), 1e-6);
  r->envelope()->editor_add(timepos_t(500), 1., false);
  CPPUNIT_ASSERT_DOUBLES_EQUAL (.8, value(c1,2500), 1e-6);
  CPPUNIT_ASSERT_DOUBLES_EQUAL (.1, value(c2,2500), 1e-6);
  r->set_muted(true);
  CPPUNIT_ASSERT_DOUBLES_EQUAL (1., value(c1,2500), 1e-6);
  r->set_muted(false);
  auto copy = std::dynamic_pointer_cast<AudioRegion>(RegionFactory::create(r, true));
  track->playlist()->add_region(copy,timepos_t(4000));
  CPPUNIT_ASSERT_DOUBLES_EQUAL (.8, value(c1,4500), 1e-6);
  track->playlist()->remove_region(copy);
  CPPUNIT_ASSERT_DOUBLES_EQUAL (1., value(c1,4500), 1e-6);
  XMLNode* before_trim = &r->get_state();
  r->trim_front(timepos_t(2250));
  CPPUNIT_ASSERT_DOUBLES_EQUAL(.2+.6*r->envelope()->eval(timepos_t(0)),value(c1,2250),1e-6);
  CPPUNIT_ASSERT_DOUBLES_EQUAL(.8,value(c1,2500),1e-6);
  MementoCommand<AudioRegion> trim(*r,before_trim,&r->get_state());
  trim.undo();
  CPPUNIT_ASSERT_EQUAL(samplepos_t(2000),r->position().samples());
  CPPUNIT_ASSERT_DOUBLES_EQUAL(.2,value(c1,2000),1e-6);
  auto overlay = std::dynamic_pointer_cast<AudioRegion>(RegionFactory::create(r,true));
  overlay->envelope()->clear();
  overlay->envelope()->fast_simple_add(timepos_t(0),.25);
  overlay->envelope()->fast_simple_add(timepos_t(1000),.25);
  track->playlist()->add_region(overlay,timepos_t(2250)); overlay->raise_to_top();
  CPPUNIT_ASSERT_DOUBLES_EQUAL(.35,value(c1,2500),1e-6);
  track->playlist()->remove_region(overlay);
  CPPUNIT_ASSERT_DOUBLES_EQUAL(.8,value(c1,2500),1e-6);
  auto other = clips.new_track();
  CPPUNIT_ASSERT(!clips.map(other,c1,0,1,false,error));
  CPPUNIT_ASSERT(!clips.map(track,c1,.9,.1,false,error));
  std::unique_ptr<XMLNode> state (&clips.get_state());
  clips.unmap(c1->id().to_s());
  CPPUNIT_ASSERT (c1->automation_state() == Off && c1->alist()->empty());
  CPPUNIT_ASSERT_DOUBLES_EQUAL (1., c1->get_value(), 1e-6);
  clips.set_state(*state, PBD::Stateful::current_state_version);
  CPPUNIT_ASSERT_DOUBLES_EQUAL (.8, value(c1,2500), 1e-6);
  std::unique_ptr<XMLNode> source_state (&r->source(0)->get_state());
  source_state->set_property("id", PBD::ID().to_s());
  auto restored = std::dynamic_pointer_cast<AutomationClipSource>(SourceFactory::create(*_session,*source_state,true));
  CPPUNIT_ASSERT(restored);
  Sample buffer[16]; std::fill(buffer,buffer+16,1);
  CPPUNIT_ASSERT_EQUAL (samplecnt_t(16), restored->read(buffer,0,16));
  for (auto x : buffer) { CPPUNIT_ASSERT_EQUAL (0.f,x); }
  clips.unmap(c1->id().to_s());
  c1->alist()->editor_add(timepos_t(0), .4, false);
  CPPUNIT_ASSERT (!clips.map(track,c1,0,1,false,error));
  CPPUNIT_ASSERT_DOUBLES_EQUAL (.4,value(c1,0),1e-6);
  CPPUNIT_ASSERT_EQUAL(0,_session->save_state(""));
  }
  AudioEngine::instance()->set_session(nullptr);
  delete _session; _session = nullptr;
  _session = load_session(path,"test_session");
  {
   AutomationClips clips(*_session);
   CPPUNIT_ASSERT_EQUAL(size_t(1),clips.mappings().size());
   auto track = std::dynamic_pointer_cast<AudioTrack>(_session->route_by_id(PBD::ID(track_id)));
   auto target = _session->route_by_id(PBD::ID(target_id))->gain_control();
   CPPUNIT_ASSERT(AutomationClips::is_track(track));
   CPPUNIT_ASSERT_DOUBLES_EQUAL(.1,target->alist()->eval(timepos_t(2500)),1e-6);
   std::shared_ptr<Region> region;
   track->playlist()->foreach_region([&] (std::shared_ptr<Region> r) { region=r; });
   CPPUNIT_ASSERT(AutomationClips::is_clip(std::dynamic_pointer_cast<AudioRegion>(region)));
   region->set_position(timepos_t(3000));
   CPPUNIT_ASSERT_DOUBLES_EQUAL(1.,target->alist()->eval(timepos_t(2500)),1e-6);
   CPPUNIT_ASSERT_DOUBLES_EQUAL(.1,target->alist()->eval(timepos_t(3500)),1e-6);
   clips.unmap(target->id().to_s());
   CPPUNIT_ASSERT(target->alist()->empty() && target->automation_state()==Off);
  }
 }
};
CPPUNIT_TEST_SUITE_REGISTRATION (AutomationClipTest);
