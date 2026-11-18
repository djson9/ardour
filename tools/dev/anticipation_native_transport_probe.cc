#define ANTICIPATION_NATIVE_HELPERS_ONLY
#include "anticipation_native_route_probe.cc"


struct LifecycleWitness : Processor {
 std::atomic<unsigned> entered{0},races{0},resets{0},direct_calls{0};
 std::atomic<bool> hold{false},held{false},fail_next{false};
 LifecycleWitness(Session& s):Processor(s,"Lifecycle ownership witness",s){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void run(BufferSet&,samplepos_t,samplepos_t,double,pframes_t,bool)override{
  if(entered.fetch_add(1))++races;
  if(ProcessThread::render_time()){
   if(hold){held=true;while(hold)std::this_thread::sleep_for(std::chrono::microseconds(100));}
   if(fail_next.exchange(false)){--entered;throw std::runtime_error("intentional producer failure");}
  }else ++direct_calls;
  --entered;
 }
 void realtime_handle_transport_stopped()override{if(entered)++races;++resets;}
 void non_realtime_locate(samplepos_t p)override{if(entered)++races;++resets;Processor::non_realtime_locate(p);}
};
int main(int argc,char** argv){try{
 need(argc==2&&ARDOUR::init(true,"/tmp"),"init");Temporal::reset();if(getenv("ARDOUR_PROBE_RT_THREADS"))Config->set_processor_usage(std::stoi(getenv("ARDOUR_PROBE_RT_THREADS")));auto ui=std::make_unique<TestUI>();
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","NativeLifecycle","");
 constexpr unsigned block=64;
 need(backend&&backend->set_driver("Realtime")==0&&backend->set_sample_rate(48000)==0&&backend->set_buffer_size(block)==0&&engine->start()==0,"engine");
 auto* s=new NativeSession(*engine,argv[1],"native-lifecycle",nullptr,"",true);engine->set_session(nullptr);
 auto route=s->new_audio_track(2,2,{},1,"Native route",0,Normal,false).front();add_effects(*s,route,true);
 auto gain=route->gain_control();gain->alist()->add(timepos_t(samplepos_t(0)),.8,false,false);gain->alist()->add(timepos_t(samplepos_t(96000)),.2,false,false);gain->alist()->set_automation_state(Play);
 auto witness=std::make_shared<LifecycleWitness>(*s);need(route->add_processor(witness,PreFader)==0,"witness");
 route->monitoring_control()->set_value(MonitorDisk,PBD::Controllable::NoGroup);s->resort_routes();s->calculate_latency();
 auto wait=[&](auto predicate,const char* message){auto end=Clock::now()+std::chrono::seconds(5);while(Clock::now()<end){{PBD::Mutex::Lock lock(engine->process_lock());if(predicate())return;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}std::cerr<<"TIMEOUT "<<message<<" rolling="<<s->transport_rolling()<<" pos="<<s->transport_sample()<<" loop="<<s->get_play_loop()<<" running="<<s->anticipative_process().running()<<" size="<<s->anticipative_process().size()<<" generation="<<s->anticipative_process().generation()<<" locate="<<s->global_locate_pending()<<" settled="<<s->setup_settled()<<std::endl;throw std::runtime_error(message);};
 engine->set_session(s);wait([&]{return s->setup_settled()&&!s->global_locate_pending();},"setup");
 auto& plan=s->anticipative_process();
 const bool automatic=getenv("ARDOUR_PROBE_AUTOMATIC");if(automatic){Config->set_anticipation_milliseconds(50);Config->set_anticipative_processing(true);}
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
  route->set_anticipative_processing_disabled(true);
  wait([&]{auto* node=plan.find_node(route.get());return node&&node->queue->state()==AnticipationQueue::State::Live;},"force realtime");
  route->set_anticipative_processing_disabled(false);
  wait([&]{auto* node=plan.find_node(route.get());return node&&node->queue->state()==AnticipationQueue::State::Ahead;},"resume anticipation");
  Config->set_anticipative_processing(false);
  wait([&]{return plan.size()==0&&s->transport_rolling()&&s->setup_settled();},"global disable while rolling");
  Config->set_anticipative_processing(true);
  wait([&]{return plan.running()&&plan.find_node(route.get())&&s->transport_rolling()&&s->setup_settled();},"global enable while rolling");
  {PBD::Mutex::Lock lock(engine->process_lock());before=plan.generation();}
  Config->set_anticipation_milliseconds(100);
  wait([&]{return plan.running()&&plan.generation()>before&&s->transport_rolling()&&s->setup_settled();},"horizon rebuild");++edits;
  {PBD::Mutex::Lock lock(engine->process_lock());before=plan.generation();}
  s->request_transport_speed(1.5);
  wait([&]{return plan.size()==0&&s->transport_rolling()&&s->engine_speed()==1.5&&s->setup_settled();},"varispeed uses direct processing");
  auto direct_before=witness->direct_calls.load();wait([&]{return witness->direct_calls>direct_before+10;},"varispeed DSP continues");
  s->request_transport_speed(1.0);
  wait([&]{return plan.running()&&plan.find_node(route.get())&&s->engine_speed()==1&&s->transport_rolling()&&s->setup_settled();},"normal speed restores anticipation");++edits;
  {PBD::Mutex::Lock lock(engine->process_lock());before=plan.generation();}
  Config->set_loop_is_mode(true);
  auto* loop=new Location(*s,timepos_t(samplepos_t(96000)),timepos_t(samplepos_t(240013)),"Live loop edit",Location::IsAutoLoop);
  s->locations()->add(loop);s->set_auto_loop_location(loop);s->request_play_loop(true);
  wait([&]{return s->get_play_loop()&&plan.running()&&plan.generation()>before&&s->transport_rolling()&&s->setup_settled();},"enable loop while playing");++edits;
  witness->fail_next=true;
  wait([&]{return s->transport_stopped()&&plan.size()==0&&s->setup_settled();},"fault retires through normal stop");
  need(witness->races==0,"fault reset raced DSP");
  s->request_roll();wait([&]{return plan.running()&&plan.find_node(route.get())&&s->transport_rolling()&&s->setup_settled();},"restart after producer fault");
  auto* removed=route.get();
  s->remove_route(route);route.reset();
  wait([&]{return !plan.find_node(removed)&&s->transport_rolling()&&s->setup_settled();},"remove playing route");++edits;
  s->request_stop(false,false);wait([&]{return plan.size()==0&&s->transport_stopped()&&s->setup_settled();},"final stop");
  need(witness->races==0,"edit raced original DSP");
 }
 engine->set_session(nullptr);route.reset();witness.reset();engine->stop();delete s;
 std::cout<<"RESULT {\"passed\":true,\"native_transport_retirements\":"<<acknowledged<<",\"automatic_edit_rebuilds\":"<<edits<<",\"fault_stop_and_restart\":"<<(automatic?"true":"false")<<",\"stalled_worker_callback_progress\":true,\"concurrent_dsp_resets\":0,\"stopped_processing_restored\":true}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(std::exception const& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
