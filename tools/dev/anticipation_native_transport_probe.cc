#define ANTICIPATION_NATIVE_HELPERS_ONLY
#include "anticipation_native_route_probe.cc"


struct LifecycleWitness : Processor {
 std::atomic<unsigned> entered{0},races{0},resets{0},direct_calls{0};
 std::atomic<bool> hold{false},held{false};
 LifecycleWitness(Session& s):Processor(s,"Lifecycle ownership witness",s){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void run(BufferSet&,samplepos_t,samplepos_t,double,pframes_t,bool)override{
  if(entered.fetch_add(1))++races;
  if(ProcessThread::render_time()){
   if(hold){held=true;while(hold)std::this_thread::sleep_for(std::chrono::microseconds(100));}
  }else ++direct_calls;
  --entered;
 }
 void realtime_handle_transport_stopped()override{if(entered)++races;++resets;}
 void non_realtime_locate(samplepos_t p)override{if(entered)++races;++resets;Processor::non_realtime_locate(p);}
};
int main(int argc,char** argv){try{
 need(argc==2&&ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","NativeLifecycle","");
 constexpr unsigned block=64;
 need(backend&&backend->set_driver("Realtime")==0&&backend->set_sample_rate(48000)==0&&backend->set_buffer_size(block)==0&&engine->start()==0,"engine");
 auto* s=new NativeSession(*engine,argv[1],"native-lifecycle",nullptr,"",true);engine->set_session(nullptr);
 auto route=s->new_audio_track(2,2,{},1,"Native route",0,Normal,false).front();add_effects(*s,route,true);
 auto witness=std::make_shared<LifecycleWitness>(*s);need(route->add_processor(witness,PreFader)==0,"witness");
 route->monitoring_control()->set_value(MonitorDisk,PBD::Controllable::NoGroup);s->resort_routes();s->calculate_latency();
 auto wait=[&](auto predicate,const char* message){auto end=Clock::now()+std::chrono::seconds(5);while(Clock::now()<end){{PBD::Mutex::Lock lock(engine->process_lock());if(predicate())return;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}throw std::runtime_error(message);};
 engine->set_session(s);wait([&]{return s->setup_settled()&&!s->global_locate_pending();},"setup");
 auto& plan=s->anticipative_process();
 const bool automatic=getenv("ARDOUR_PROBE_AUTOMATIC");if(automatic)s->set_anticipative_processing(true,50);
 unsigned acknowledged=0;
 for(unsigned pass=0;pass<2;++pass){
  if(!automatic){
   engine->set_session(nullptr);AnticipativeProcess::Timeline timeline(*s,s->transport_sample());
   auto& node=plan.add_route(route,timeline.first(*route),32,[timeline](AnticipationQueue::Range const& r){return timeline.next(r);});
   plan.start(pass+1);wait([&]{return node.queue->primed();},"prefill");
  }
  s->request_roll();engine->set_session(s);wait([&]{return s->transport_rolling()&&plan.running()&&plan.find_node(route.get());},"roll");
  auto warmup=engine->processed_samples();wait([&]{return engine->processed_samples()>warmup+2*block;},"backend warmup");
  witness->hold=true;wait([&]{return witness->held.load();},"producer did not enter stall");
  auto device_before=engine->processed_samples();auto resets_before=witness->resets.load();
  if(pass==0)s->request_stop(false,false);else s->request_locate(96000,true,MustStop);
  wait([&]{return !plan.running();},"transport did not cancel worker");
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  {PBD::Mutex::Lock lock(engine->process_lock());std::cerr<<"RETIRE pass="<<pass<<" device_progress="<<engine->processed_samples()-device_before<<" nodes="<<plan.size()<<" quiescent="<<plan.quiescent()<<" resets="<<witness->resets-resets_before<<std::endl;need(engine->processed_samples()>device_before+512,"audio callback blocked on producer");need(plan.size()>0&&!plan.quiescent()&&witness->resets==resets_before,"DSP reset before ownership acknowledgement");}
  witness->hold=false;witness->held=false;
  wait([&]{return plan.size()==0&&s->setup_settled()&&s->transport_stopped();},"transport failed to retire plan");
  need(witness->races==0&&witness->resets>resets_before,"lifecycle raced DSP");
  auto calls=witness->direct_calls.load();wait([&]{return witness->direct_calls>calls;},"stopped processing not restored");
  if(pass==1)need(s->transport_sample()==96000,"seek destination");++acknowledged;
 }
 unsigned edits=0;
 if(automatic){
  s->request_roll();wait([&]{return plan.running()&&s->transport_rolling()&&plan.find_node(route.get());},"edit roll");
  uint64_t before;{PBD::Mutex::Lock lock(engine->process_lock());before=plan.generation();}
  need(route->add_processor(std::make_shared<NativeDelay>(*s),PreFader)==0,"add latent processor while rolling");
  wait([&]{return plan.running()&&plan.generation()>before&&s->transport_rolling()&&s->setup_settled();},"rebuild after insert");++edits;
  {PBD::Mutex::Lock lock(engine->process_lock());before=plan.generation();}
  route->set_meter_point(MeterInput);
  wait([&]{return plan.running()&&plan.generation()>before&&s->transport_rolling()&&s->setup_settled();},"rebuild after RT meter relocation");++edits;
  s->request_stop(false,false);wait([&]{return plan.size()==0&&s->transport_stopped()&&s->setup_settled();},"final stop");
  need(witness->races==0,"edit raced original DSP");
 }
 engine->set_session(nullptr);route.reset();witness.reset();delete s;engine->stop();
 std::cout<<"RESULT {\"passed\":true,\"native_transport_retirements\":"<<acknowledged<<",\"automatic_edit_rebuilds\":"<<edits<<",\"stalled_worker_callback_progress\":true,\"concurrent_dsp_resets\":0,\"stopped_processing_restored\":true}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(std::exception const& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
