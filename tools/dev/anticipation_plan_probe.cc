// Eligibility-plan construction using Ardour's existing dependency queries.
#define ANTICIPATION_CALLBACK_HELPERS_ONLY
#include "anticipation_callback_probe.cc"
#include <set>
#include "ardour/anticipative_process.h"

int main(int argc,char** argv){try{
 need(argc==2,"session path");need(std::string(g_get_home_dir())==getenv("ARDOUR_PROBE_HOME"),"preferences isolation");
 need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();if(getenv("ARDOUR_PROBE_RT_THREADS"))Config->set_processor_usage(std::stoi(getenv("ARDOUR_PROBE_RT_THREADS")));auto ui=std::make_unique<TestUI>();
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","EligibilityProbe","");
 need(bool(backend)&&backend->set_driver("Realtime")==0&&backend->set_sample_rate(48000)==0&&backend->set_buffer_size(64)==0&&engine->start()==0,"engine");
 auto* s=new FixtureSession(*engine,argv[1],"eligibility-probe",nullptr,"",true);engine->set_session(nullptr);
 unsigned checks=0;
 {
  auto a=s->new_audio_track(2,2,{},1,"Playback A",0,Normal,false).front();
  auto key=s->new_audio_track(2,2,{},1,"Sidechain key",0,Normal,false).front();
  Fixture bus(*s,"Playback bus",true,false),side(*s,"Sidechain target",false,false),master(*s,"Live master",false,false);
  auto plugin=find_plugin(*s,"urn:ardour:a-comp#stereo",LV2);need(bool(plugin),"compressor");
  auto comp=std::make_shared<PluginInsert>(*s,*side.route,plugin);need(side.route->add_processor(comp,PreFader)==0&&side.route->add_sidechain(comp),"sidechain configuration");
  connect_send(*s,a,bus.route);connect_send(*s,bus.route,master.route);connect_send(*s,side.route,master.route);
  auto sc=comp->sidechain_input();const auto key_port=key->output()->nth(0)->name();
  {PBD::Mutex::Lock lock(engine->process_lock());need(sc->connect(sc->nth(0),key_port)==0,"sidechain connection");}
  s->resort_routes();
  std::vector<std::shared_ptr<Route>> nodes{a,key,bus.route,side.route,master.route};
  engine->set_session(s);bool settled=false;auto setup_deadline=Clock::now()+std::chrono::seconds(5);
  while(!settled&&Clock::now()<setup_deadline){{PBD::Mutex::Lock lock(engine->process_lock());settled=s->setup_settled()&&!s->global_locate_pending();}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  engine->set_session(nullptr);need(settled,"initial session setup");
  auto& plan=s->anticipative_process();
  auto set=[&](std::shared_ptr<AutomationControl> control,double value){
   control->set_value(value,PBD::Controllable::NoGroup);engine->set_session(s);
   const auto deadline=Clock::now()+std::chrono::seconds(5);bool ready=false;
   while(!ready&&Clock::now()<deadline){
    {PBD::Mutex::Lock lock(engine->process_lock());ready=control->get_value()==value&&!s->global_locate_pending();}
    if(!ready)std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
   engine->set_session(nullptr);need(ready,"control did not reach real Session callback");need(plan.size()==0,"eligibility fixture retains no DSP between control changes");
  };
  auto classify=[&](std::shared_ptr<Route> forced={}){
   std::set<Route*> result;
   std::vector<AnticipativeProcess::Node*> owners;
   for(auto const& route:nodes)owners.push_back(&plan.add(route,{ChanCount(DataType::AUDIO,2),64,4,8192},[](BufferSet& b,AnticipationQueue::Range const& r,bool){b.silence(r.frames,0);},{0,0,64}));
   for(auto* owner:owners)owner->forced=owner->route==master.route||owner->route==forced;
   plan.start(1);plan.begin_cycle();
   for(auto* owner:owners)if(owner->reason.load()!=AnticipativeProcess::Predictable)result.insert(owner->route.get());
   plan.clear();
   return result;
  };
  auto check=[&](std::set<Route*> actual,std::initializer_list<Route*> expected){if(actual!=std::set<Route*>(expected)){std::cerr<<"CHECK "<<checks<<" nodes="<<plan.size()<<" actual:";for(auto* r:actual)std::cerr<<" "<<r->name();std::cerr<<std::endl;}need(actual==std::set<Route*>(expected),"wrong live dependency closure");++checks;};
  set(a->monitoring_control(),MonitorDisk);set(key->monitoring_control(),MonitorDisk);
  need(key->direct_feeds_according_to_graph(side.route)&&key->feeds(master.route),"sidechain missing from existing graph");
  check(classify(),{master.route.get()});
  set(a->rec_enable_control(),1);check(classify(),{a.get(),bus.route.get(),master.route.get()});
  set(a->rec_enable_control(),0);check(classify(),{master.route.get()});
  set(key->monitoring_control(),MonitorInput);check(classify(),{key.get(),side.route.get(),master.route.get()});
  check(classify(bus.route),{key.get(),side.route.get(),bus.route.get(),master.route.get()});
  {PBD::Mutex::Lock lock(engine->process_lock());need(sc->disconnect(sc->nth(0),key_port)==0,"disconnect sidechain");}
  s->resort_routes();check(classify(),{key.get(),master.route.get()});
  plan.clear();
 }
 delete s;engine->stop();
 std::cout<<"RESULT {\"passed\":true,\"session_owned_engine_plan\":true,\"eligibility_cases\":"<<checks<<",\"existing_graph_reused\":true,\"actual_record_and_monitor_controls\":true,\"sidechain_graph_revision_checked\":true,\"hardware_latency_measured\":false}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
