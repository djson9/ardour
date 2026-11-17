#define ANTICIPATION_CALLBACK_HELPERS_ONLY
#include "anticipation_callback_probe.cc"
#include "ardour/anticipative_process.h"

struct PlanConsumer : Processor {
 AnticipativeProcess& plan;AnticipativeProcess::Node& node;
 std::atomic<unsigned> completed{0};unsigned misses=0;
 PlanConsumer(Session& s,AnticipativeProcess::Node& owner):Processor(s,"Plan consumer",s),plan(s.anticipative_process()),node(owner){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void run(BufferSet& b,samplepos_t start,samplepos_t,double speed,pframes_t n,bool)override{
  if(speed!=1||completed.load()>=total_blocks){b.silence(n,0);return;}
  auto result=plan.process(node,b,{start,start,n});
  if(result!=AnticipationQueue::Result::Cached&&result!=AnticipationQueue::Result::Live)++misses;
  completed.fetch_add(1,std::memory_order_release);
 }
};
struct DirectBus : Processor {
 Fixture& fixture;
 DirectBus(Session& s,Fixture& f):Processor(s,"Direct bus DSP",s),fixture(f){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void run(BufferSet& b,samplepos_t start,samplepos_t,double speed,pframes_t n,bool)override{
  if(speed==1)fixture.run(b,start,n);else b.silence(n,0);
 }
};
int main(int argc,char** argv){try{
 need(argc==2,"path");need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","EngineGraph","");
 unsigned block=getenv("ARDOUR_PROBE_BLOCK")?std::stoul(getenv("ARDOUR_PROBE_BLOCK")):64;
 need(bool(backend)&&backend->set_driver("Realtime")==0&&backend->set_sample_rate(48000)==0&&backend->set_buffer_size(block)==0&&engine->start()==0,"engine");
 auto* s=new FixtureSession(*engine,argv[1],"engine-graph",nullptr,"",true);engine->set_session(nullptr);
  double error=0,peak=0;uint64_t handoffs=0;
 {
  Fixture source(*s,"Future source DSP",true,true,false),reference(*s,"Direct source DSP",true,true,false);
  Fixture future_bus(*s,"Future bus DSP",true,false,false),direct_bus(*s,"Direct bus DSP",true,false,false);
  for(auto* f:{&future_bus,&direct_bus}){auto route=f->route;auto endpoint=f->endpoint;f->render_override=[route,endpoint](BufferSet& b,samplepos_t start,unsigned n){(route.get()->*RouteAccess::bounce())(b,start,n,endpoint,true,true,false);};}
  auto a=s->new_audio_track(2,2,{},1,"A",0,Normal,false).front();
  auto b=s->new_audio_route(2,2,{},1,"B",PresentationInfo::AudioBus,0).front();
  auto ra=s->new_audio_route(2,2,{},1,"Reference A",PresentationInfo::AudioBus,0).front();
  auto rb=s->new_audio_route(2,2,{},1,"Reference B",PresentationInfo::AudioBus,0).front();
  {PBD::Mutex::Lock lock(engine->process_lock());
   for(unsigned c=0;c<2;++c){need(b->input()->connect(b->input()->nth(c),a->output()->nth(c)->name())==0,"A to B main output");need(rb->input()->connect(rb->input()->nth(c),ra->output()->nth(c)->name())==0,"reference main output");}
  }
  auto& plan=s->anticipative_process();AnticipationQueue::Configuration config{ChanCount(DataType::AUDIO,2),block,(horizon_samples+block-1)/block,8192};
  auto& an=plan.add(a,config,[&](BufferSet& data,AnticipationQueue::Range const& r,bool){source.run(data,r.project,r.frames);},{0,0,block});
  auto& reader=an.queue->add_reader();BufferSet input;prepare(input,block);std::vector<float> bus_input(total_blocks*2*block);
  auto& bn=plan.add(b,config,[&](BufferSet& data,AnticipationQueue::Range const& r,bool live){
   if(!live)data.read_from(input,r.frames);
   if(r.project>=0&&r.project+r.frames<=samplepos_t(total_blocks*block))for(unsigned c=0;c<2;++c)std::copy_n(data.get_audio(c).data(),r.frames,bus_input.begin()+((r.project/block)*2+c)*block);
   future_bus.run(data,r.project,r.frames);an.queue->release_until(reader,r.continuous+r.frames);
  },{0,0,block},AnticipationQueue::NextRange(),[&](AnticipationQueue::Range const& r){return an.queue->peek(reader,input,r,plan.generation());});
  auto ac=std::make_shared<PlanConsumer>(*s,an),bc=std::make_shared<PlanConsumer>(*s,bn);
  need(a->add_processor(ac,PreFader)==0&&b->add_processor(bc,PreFader)==0,"consumers");
  auto direct_processor=std::make_shared<DirectBus>(*s,direct_bus);auto ref_input=std::make_shared<Tap>(*s,block);
  need(ra->add_processor(std::make_shared<Bridge>(*s,reference,block,false,false),PreFader)==0&&rb->add_processor(direct_processor,PreFader)==0&&rb->add_processor(ref_input,direct_processor)==0,"oracles");
  auto tap=std::make_shared<Tap>(*s,block),oracle=std::make_shared<Tap>(*s,block);
  need(b->add_processor(tap,PostFader)==0&&rb->add_processor(oracle,PostFader)==0,"taps");
  a->monitoring_control()->set_value(MonitorDisk,PBD::Controllable::NoGroup);s->resort_routes();engine->set_session(s);
  bool ready=false;auto until=Clock::now()+std::chrono::seconds(5);
  while(!ready&&Clock::now()<until){{PBD::Mutex::Lock lock(engine->process_lock());ready=s->setup_settled()&&!s->global_locate_pending();}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  need(ready,"setup");engine->set_session(nullptr);plan.start(1);
  until=Clock::now()+std::chrono::seconds(10);while(!(an.queue->primed()&&bn.queue->primed())&&Clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  need(an.queue->primed()&&bn.queue->primed(),"upstream readiness/prefill");s->request_roll();engine->set_session(s);
  bool armed=false,released=false;until=Clock::now()+std::chrono::seconds(30);
  while(bc->completed.load(std::memory_order_acquire)<total_blocks&&Clock::now()<until){
   unsigned k=bc->completed.load();if(!armed&&k>=arm_block){a->rec_enable_control()->set_value(1,PBD::Controllable::NoGroup);armed=true;}
   if(armed&&!released&&k>=demote_block){a->rec_enable_control()->set_value(0,PBD::Controllable::NoGroup);released=true;}
   std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  engine->set_session(nullptr);plan.stop();
  need(tap->k==total_blocks&&oracle->k==total_blocks,"complete");
  for(unsigned i=0;i<tap->captured.size();++i)error=std::max(error,double(std::abs(tap->captured[i]-oracle->captured[i])));
  peak=tap->peak;handoffs=an.queue->live_handoffs()+bn.queue->live_handoffs();
  double input_error=0;unsigned first=bus_input.size();for(unsigned i=0;i<bus_input.size();++i){auto d=double(std::abs(bus_input[i]-ref_input->captured[i]));input_error=std::max(input_error,d);if(d>1e-7&&first==bus_input.size())first=i;}
  std::cerr<<"INPUT error="<<input_error<<" first="<<first<<" block="<<(first/(2*block));if(first<bus_input.size())std::cerr<<" values="<<bus_input[first]<<","<<ref_input->captured[first];std::cerr<<std::endl;
  std::cerr<<"ENGINE GRAPH error="<<error<<" misses="<<ac->misses<<","<<bc->misses<<" handoffs="<<handoffs<<" peak="<<peak<<std::endl;
  need(error==0&&peak>1e-4&&ac->misses==0&&bc->misses==0&&handoffs==2&&released,"routed engine queue equivalence");plan.clear();
 }
 delete s;engine->stop();std::cout<<"RESULT {\"passed\":true,\"engine_coordinator\":true,\"cached_bus\":true,\"automatic_downstream_handoffs\":"<<handoffs<<",\"block\":"<<block<<",\"max_sample_error\":"<<error<<",\"cache_underruns\":0}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(std::exception const& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
