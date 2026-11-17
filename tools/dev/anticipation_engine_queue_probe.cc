// Registered normal Route/Graph consumers of the production engine queue.
#define ANTICIPATION_CALLBACK_HELPERS_ONLY
#include "anticipation_callback_probe.cc"
#include "ardour/anticipation_queue.h"
#include "ardour/anticipative_process.h"

struct EngineQueueProcessor : Processor {
 using Queue=AnticipationQueue;
 Fixture& source;unsigned block,k=0,misses=0,live_calls=0;
 std::atomic<bool> done{false};std::atomic<unsigned> completed{0};
 AnticipativeProcess& plan;AnticipativeProcess::Node& node;Queue& queue;
 EngineQueueProcessor(Session& s,std::shared_ptr<Route> owner,Fixture& f,unsigned n):Processor(s,"Engine anticipation",s),source(f),block(n),plan(s.anticipative_process()),
 node(plan.add(owner,{ChanCount(DataType::AUDIO,2),n,(horizon_samples+n-1)/n,8192},
       [&](BufferSet& b,Queue::Range const& r,bool live){source.run(b,r.project,r.frames);if(live)++live_calls;},{0,0,n})),queue(*node.queue){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void run(BufferSet& b,samplepos_t start,samplepos_t,double speed,pframes_t n,bool)override{
  if(speed!=1||k>=total_blocks){b.silence(n,0);return;}
  auto result=plan.process(node,b,{start,start,n});
  if(result!=Queue::Result::Cached&&result!=Queue::Result::Live)++misses;
  if(++k==total_blocks)done.store(true,std::memory_order_release);completed.store(k,std::memory_order_release);
 }
};

int main(int argc,char** argv){try{
 need(argc==2,"session path");need(std::string(g_get_home_dir())==getenv("ARDOUR_PROBE_HOME"),"preferences isolation");
 need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","EngineQueueProbe","");
 unsigned block=getenv("ARDOUR_PROBE_BLOCK")?std::stoul(getenv("ARDOUR_PROBE_BLOCK")):64;
 need(bool(backend)&&backend->set_driver("Realtime")==0&&backend->set_sample_rate(48000)==0&&backend->set_buffer_size(block)==0&&engine->start()==0,"engine");
 auto* s=new FixtureSession(*engine,argv[1],"engine-queue",nullptr,"",true);engine->set_session(nullptr);
 double error=0;unsigned live_calls=0;uint64_t handoffs=0;
 {
  Fixture source(*s,"Ahead source",true,true,false),reference(*s,"Reference source",true,true,false);
  auto output=s->new_audio_track(2,2,{},1,"Anticipated",0,Normal,false).front();
  auto direct=s->new_audio_route(2,2,{},1,"Reference",PresentationInfo::AudioBus,0).front();
  auto queued=std::make_shared<EngineQueueProcessor>(*s,output,source,block);
  auto oracle=std::make_shared<Bridge>(*s,reference,block,false,false);
  auto tap=std::make_shared<Tap>(*s,block),ref=std::make_shared<Tap>(*s,block);
  need(output->add_processor(queued,PreFader)==0&&direct->add_processor(oracle,PreFader)==0,"processors");
  need(output->add_processor(tap,PostFader)==0&&direct->add_processor(ref,PostFader)==0,"taps");
  output->monitoring_control()->set_value(MonitorDisk,PBD::Controllable::NoGroup);
  s->resort_routes();engine->set_session(s);bool ready=false;auto until=Clock::now()+std::chrono::seconds(5);
  while(!ready&&Clock::now()<until){{PBD::Mutex::Lock lock(engine->process_lock());ready=s->setup_settled()&&!s->global_locate_pending();}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  need(ready,"setup");engine->set_session(nullptr);
  s->anticipative_process().start(1);until=Clock::now()+std::chrono::seconds(5);
  while(!queued->queue.primed()&&Clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  need(queued->queue.primed(),"prime");s->request_roll();engine->set_session(s);until=Clock::now()+std::chrono::seconds(30);
  bool armed=false,released=false;
  while(!queued->done.load(std::memory_order_acquire)&&Clock::now()<until){
   auto completed=queued->completed.load(std::memory_order_acquire);
   if(!armed&&completed>=arm_block){output->rec_enable_control()->set_value(1,PBD::Controllable::NoGroup);armed=true;}
   if(armed&&!released&&completed>=demote_block){output->rec_enable_control()->set_value(0,PBD::Controllable::NoGroup);released=true;}
   std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  engine->set_session(nullptr);s->anticipative_process().stop();
  need(queued->done&&tap->k==total_blocks&&ref->k==total_blocks,"complete");
  for(unsigned i=0;i<tap->captured.size();++i)error=std::max(error,double(std::abs(tap->captured[i]-ref->captured[i])));
  live_calls=queued->live_calls;handoffs=queued->queue.live_handoffs();
  need(error==0&&tap->peak>1e-4&&queued->misses==0&&queued->queue.underruns()==0&&handoffs==1&&live_calls>0&&released,"engine queue equivalence");
  s->anticipative_process().clear();
 }
 delete s;engine->stop();
 std::cout<<"RESULT {\"passed\":true,\"production_engine_queue\":true,\"session_owned_plan\":true,\"automatic_record_arm_handoff\":true,\"block\":"<<block<<",\"max_sample_error\":"<<error<<",\"live_calls\":"<<live_calls<<",\"handoffs\":"<<handoffs<<",\"cache_underruns\":0}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(std::exception const& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
