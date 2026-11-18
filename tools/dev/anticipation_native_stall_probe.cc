// Controlled producer failures through the original Session/Graph/Route path.
#define ANTICIPATION_NATIVE_HELPERS_ONLY
#include "anticipation_native_route_probe.cc"
#include <fstream>

constexpr samplepos_t stall_at=24576, arm_at=20480, finish_at=48000;
struct StallControl {
 std::atomic<samplepos_t> audible_end{0}, entered_at{-1};
 std::atomic<bool> triggered{false},held{false},abort{false},watchdog{false};
 std::atomic<unsigned> active{0},races{0};
 std::atomic<int64_t> began_ns{0},last_render_ns{0},max_render_ns{0};
 samplepos_t release_at,trigger_at;
 explicit StallControl(samplepos_t release,samplepos_t trigger=stall_at):release_at(release),trigger_at(trigger){}
};
static int64_t now_ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
struct StallBegin : Processor {
 StallControl& control;
 StallBegin(Session& s,StallControl& c):Processor(s,"Controlled worker stall",s),control(c){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void run(BufferSet&,samplepos_t start,samplepos_t,double speed,pframes_t,bool)override{
  if(control.active.fetch_add(1))++control.races;
  control.began_ns=now_ns();
  if(speed==1&&!AudioEngine::instance()->in_process_thread()&&start>=control.trigger_at&&!control.triggered.exchange(true)){
   control.entered_at=start;control.held=true;
   auto deadline=Clock::now()+std::chrono::seconds(2);
   while(control.audible_end.load(std::memory_order_acquire)<control.release_at&&!control.abort){
    if(Clock::now()>=deadline){control.watchdog=true;break;}
    std::this_thread::sleep_for(std::chrono::microseconds(100));
   }
   control.held=false;
  }
 }
};
struct StallEnd : Processor {
 StallControl& control;
 StallEnd(Session& s,StallControl& c):Processor(s,"Worker timing end",s),control(c){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void run(BufferSet&,samplepos_t,samplepos_t,double,pframes_t,bool)override{
  int64_t duration=now_ns()-control.began_ns.load();control.last_render_ns=duration;
  auto old=control.max_render_ns.load();while(old<duration&&!control.max_render_ns.compare_exchange_weak(old,duration)){}
  --control.active;
 }
};
// Analytic current-cycle signal makes a delayed live-monitor path observable.
struct CurrentSignal : Processor {
 CurrentSignal(Session& s):Processor(s,"Current input timestamp oracle",s){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 static float sample(samplepos_t t,unsigned channel){return float((t*17+channel*31)%4093-2046)/8192.f;}
 void run(BufferSet& b,samplepos_t start,samplepos_t,double speed,pframes_t n,bool)override{
  for(unsigned c=0;c<2;++c)for(unsigned i=0;i<n;++i)b.get_audio(c).data()[i]=speed==1?sample(start+i,c):0.f;
 }
};
// Negative controls alter only the isolated fixture, never engine code.
struct DelayedOutput : Processor {
 std::vector<float> previous;unsigned block;samplepos_t from,to;
 DelayedOutput(Session& s,unsigned n,samplepos_t begin,samplepos_t end):Processor(s,"Deliberately stale fixture output",s),previous(2*n,0),block(n),from(begin),to(end){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void run(BufferSet& b,samplepos_t start,samplepos_t,double speed,pframes_t n,bool)override{
  for(unsigned c=0;c<2;++c)for(unsigned i=0;i<n;++i){float current=b.get_audio(c).data()[i];if(speed==1&&start>=from&&start<to)b.get_audio(c).data()[i]=previous[c*block+i];previous[c*block+i]=current;}
 }
};
struct StallOracle : Processor {
 struct Row {samplepos_t frame;uint64_t depth,misses,generation;int state;bool held,missing;double playback_error,monitor_error,gap_us,render_us;};
 StallControl& control;Route* source;AnticipationQueue* queue=nullptr;
 std::vector<Row> trace;size_t used=0;unsigned block;
 uint64_t previous_misses=0,previous_generation=0,generation_changes=0,missing_blocks=0,exact_frames=0,live_frames=0;
 samplepos_t first_missing=-1,last_missing=-1,first_live=-1,first_playback_error=-1,first_monitor_error=-1;
 double playback_error=0,monitor_error=0,missing_peak=0,reference_peak=0;
 bool finite=true,consecutive=true,trace_overflow=false;samplepos_t next=0;Clock::time_point previous{};
 StallOracle(Session& s,StallControl& c,Route* route,unsigned quantum):Processor(s,"Stall and current-input comparison",s),control(c),source(route),trace(2048),block(quantum){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void run(BufferSet& b,samplepos_t start,samplepos_t,double speed,pframes_t n,bool)override{
  if(speed!=1||start>=finish_at)return;
  // Plans can retire even at startup. Resolve the current queue on the callback;
  // retaining a raw queue pointer across generations would invalidate this oracle.
  auto node=_session.anticipative_process().find_node(source);queue=node?node->queue.get():nullptr;
  if(!queue){consecutive=false;return;}
  consecutive=consecutive&&start==next&&n==block;next=start+n;
  auto generation=queue->generation();if(generation!=previous_generation){if(previous_generation)++generation_changes;previous_generation=generation;previous_misses=0;}
  auto misses=queue->underruns();bool missing=misses!=previous_misses;
  consecutive=consecutive&&misses-previous_misses<=1;previous_misses=misses;
  double pe=0,me=0;
  for(unsigned c=0;c<2;++c)for(unsigned i=0;i<n;++i){
   float value=b.get_audio(c).data()[i],reference=b.get_audio(c+2).data()[i];
   finite=finite&&std::isfinite(value)&&std::isfinite(reference)&&std::isfinite(b.get_audio(c+4).data()[i]);
   reference_peak=std::max(reference_peak,double(std::abs(reference)));
   if(missing)missing_peak=std::max(missing_peak,double(std::abs(value)));
   else pe=std::max(pe,double(std::abs(value-reference)));
   me=std::max(me,double(std::abs(b.get_audio(c+4).data()[i]-CurrentSignal::sample(start+i,c))));
  }
  if(missing){++missing_blocks;if(first_missing<0)first_missing=start;last_missing=start;}
  else if(pe==0)exact_frames+=n;
  if(pe>0&&first_playback_error<0)first_playback_error=start;
  if(me>0&&first_monitor_error<0)first_monitor_error=start;
  auto state=queue->state();if(state==AnticipationQueue::State::Live){if(first_live<0)first_live=start;live_frames+=n;}
  playback_error=std::max(playback_error,pe);monitor_error=std::max(monitor_error,me);
  auto now=Clock::now();double gap=previous==Clock::time_point{}?0:std::chrono::duration<double,std::micro>(now-previous).count();previous=now;
  if(used<trace.size())trace[used++]={start,queue->queued_blocks(),misses,generation,int(state),control.held.load(),missing,pe,me,gap,double(control.last_render_ns.load())/1000};else trace_overflow=true;
  control.audible_end.store(start+n,std::memory_order_release);
 }
 void save(std::string const& path){
  std::ofstream f(path);f<<"frame,queued_frames,coverage_ms,cache_underruns,generation,state,worker_held,missing,playback_error,monitor_error,callback_gap_us,last_route_render_us\n";
  for(size_t i=0;i<used;++i){auto const& r=trace[i];f<<r.frame<<","<<r.depth*block<<","<<double(r.depth*block)/48<<","<<r.misses<<","<<r.generation<<","<<r.state<<","<<r.held<<","<<r.missing<<","<<r.playback_error<<","<<r.monitor_error<<","<<r.gap_us<<","<<r.render_us<<"\n";}
 }
};
int main(int argc,char** argv){try{
 need(argc==2&&ARDOUR::init(true,"/tmp"),"init");Temporal::reset();Config->set_processor_usage(1);auto ui=std::make_unique<TestUI>();
 std::string scenario=getenv("ARDOUR_PROBE_STALL")?getenv("ARDOUR_PROBE_STALL"):"absorbed";
 std::string mutation=getenv("ARDOUR_PROBE_ORACLE_MUTATION")?getenv("ARDOUR_PROBE_ORACLE_MUTATION"):"none";
 need(mutation=="none"||mutation=="monitor-delay"||mutation=="stale-playback","mutation");
 need(scenario=="absorbed"||scenario=="starved"||scenario=="long-starved"||scenario=="handoff"||scenario=="demotion","scenario");
 unsigned block=getenv("ARDOUR_PROBE_BLOCK")?std::stoul(getenv("ARDOUR_PROBE_BLOCK")):64;need(block==64||block==128,"quantum");
 bool starved=scenario=="starved"||scenario=="long-starved"||scenario=="demotion";
 StallControl control(scenario=="demotion"?arm_at+1536:scenario=="long-starved"?stall_at+12032:scenario=="starved"?stall_at+1536:scenario=="handoff"?stall_at-2048:stall_at-4096,scenario=="demotion"?0:stall_at);
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","NativeStall","");
 need(backend&&backend->set_driver(getenv("ARDOUR_PROBE_DRIVER")?getenv("ARDOUR_PROBE_DRIVER"):"Realtime")==0&&backend->set_sample_rate(48000)==0&&backend->set_buffer_size(block)==0&&engine->start()==0,"engine");
 auto* s=new NativeSession(*engine,argv[1],"native-stall",nullptr,"",true);engine->set_session(nullptr);
 auto dispose=[&](NativeSession* session){control.abort=true;engine->set_session(nullptr);session->anticipative_process().clear();engine->stop();delete session;};
 std::unique_ptr<NativeSession,decltype(dispose)> cleanup(s,dispose);
 auto source=s->new_audio_track(2,2,{},1,"Stalled playback",0,Normal,false).front();
 auto reference=s->new_audio_track(2,2,{},1,"Uninterrupted reference",0,Normal,false).front();reference->set_anticipative_processing_disabled(true);
 need(source->add_processor(std::make_shared<StallBegin>(*s,control),PreFader)==0,"begin marker");
 add_effects(*s,source,true);add_effects(*s,reference,true);
 need(source->add_processor(std::make_shared<StallEnd>(*s,control),PreFader)==0,"end marker");
 if(mutation=="stale-playback")need(source->add_processor(std::make_shared<DelayedOutput>(*s,block,control.release_at+4800,control.release_at+9600),PreFader)==0,"stale playback control");
 for(auto route:{source,reference})route->monitoring_control()->set_value(MonitorDisk,PBD::Controllable::NoGroup);
 if(scenario=="demotion")source->rec_enable_control()->set_value(1,PBD::Controllable::NoGroup);
 auto input=s->new_audio_route(2,2,{},1,"Current live input",PresentationInfo::AudioBus,0).front();input->set_anticipative_processing_disabled(true);
 need(input->add_processor(std::make_shared<CurrentSignal>(*s),PreFader)==0,"live source");
 auto monitor=s->new_audio_track(2,2,{},1,"Always monitored",0,Normal,false).front();monitor->monitoring_control()->set_value(MonitorInput,PBD::Controllable::NoGroup);
 if(mutation=="monitor-delay")need(monitor->add_processor(std::make_shared<DelayedOutput>(*s,block,arm_at,finish_at),PreFader)==0,"delayed monitor control");
 auto sink=s->new_audio_route(6,6,{},1,"Independent comparison lanes",PresentationInfo::AudioBus,0).front();sink->set_anticipative_processing_disabled(true);
 auto oracle=std::make_shared<StallOracle>(*s,control,source.get(),block);need(sink->add_processor(oracle,PreFader)==0,"oracle");
 {PBD::Mutex::Lock lock(engine->process_lock());for(unsigned c=0;c<2;++c){
  need(monitor->input()->connect(monitor->input()->nth(c),input->output()->nth(c)->name())==0,"monitor input");
  for(auto edge:std::vector<std::pair<std::shared_ptr<Route>,unsigned>>{{source,0},{reference,2},{monitor,4}})need(sink->input()->connect(sink->input()->nth(c+edge.second),edge.first->output()->nth(c)->name())==0,"oracle lane");
 }}
 s->resort_routes();s->calculate_latency();auto warmup=engine->processed_samples();engine->set_session(s);
 auto until=Clock::now()+std::chrono::seconds(2);bool ready=false;
 while(Clock::now()<until){{PBD::Mutex::Lock lock(engine->process_lock());ready=s->setup_settled()&&!s->global_locate_pending()&&engine->processed_samples()>=warmup+8192;}if(ready)break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}need(ready,"setup");
 engine->set_session(nullptr);
 for(auto route:{source,reference})route->foreach_processor([](std::weak_ptr<Processor> weak){auto insert=std::dynamic_pointer_cast<PluginInsert>(weak.lock());if(insert)for(unsigned i=0;i<insert->get_count();++i)insert->plugin(i)->flush();});
 s->set_anticipative_processing(true,200);s->request_roll();engine->set_session(s);
 bool armed=false,armed_while_held=false;samplepos_t requested_arm=-1;
 until=Clock::now()+std::chrono::seconds(3);
 while(control.audible_end.load(std::memory_order_acquire)<finish_at&&Clock::now()<until){
  if((scenario=="handoff"||scenario=="demotion")&&!armed&&control.audible_end.load()>=arm_at){
   requested_arm=control.audible_end;armed_while_held=control.held;source->rec_enable_control()->set_value(scenario=="handoff"?1:0,PBD::Controllable::NoGroup);armed=true;
  }
  std::this_thread::sleep_for(std::chrono::microseconds(200));
 }
 control.abort=true;engine->set_session(nullptr);auto& plan=s->anticipative_process();plan.stop();
 oracle->save(std::string(argv[1])+"/stall-trace.csv");
 auto* node=plan.find_node(source.get());need(node,"native queue");auto misses=node->queue->underruns();auto handoffs=node->queue->live_handoffs();
 std::cerr<<"STALL scenario="<<scenario<<" frames="<<control.audible_end<<" entered="<<control.entered_at<<" misses="<<misses<<" missing_blocks="<<oracle->missing_blocks<<" first_missing="<<oracle->first_missing<<" last_missing="<<oracle->last_missing<<" playback_error="<<oracle->playback_error<<" monitor_error="<<oracle->monitor_error<<" handoffs="<<handoffs<<" first_live="<<oracle->first_live<<" requested_arm="<<requested_arm<<" watchdog="<<control.watchdog<<std::endl;
 need(control.audible_end>=finish_at&&!control.watchdog&&control.triggered&&(scenario=="demotion"?control.entered_at>=arm_at:control.entered_at==stall_at),"completed injected stall");
 need(!control.races&&!node->queue->discontinuities()&&oracle->consecutive&&!oracle->trace_overflow,"ownership/timeline");
 need(oracle->finite&&oracle->reference_peak>1e-4,"finite samples and non-silent reference");
 if(mutation=="none")need(oracle->playback_error==0&&oracle->monitor_error==0,"exact playback/current-cycle monitoring");
 else if(mutation=="monitor-delay")need(oracle->monitor_error>0&&oracle->playback_error==0&&oracle->first_monitor_error==arm_at,"oracle missed delayed monitoring");
 else need(oracle->playback_error>0&&oracle->monitor_error==0&&oracle->first_playback_error==((control.release_at+4800+block-1)/block)*block,"oracle missed stale recovery audio");
 if(starved)need(misses>0&&misses==oracle->missing_blocks&&oracle->first_missing==control.entered_at&&oracle->last_missing<control.release_at+4800&&oracle->missing_peak==0&&oracle->exact_frames>=24000,"bounded diagnosed starvation and exact recovery");
 else need(misses==0&&oracle->missing_blocks==0,"stall absorbed by coverage");
 if(scenario=="handoff")need(armed_while_held&&handoffs==1&&oracle->first_live==stall_at+block&&oracle->live_frames>=12000,"exclusive record-arm handoff");
 else need(handoffs==0,"unexpected ownership transfer");
 if(scenario=="demotion")need(armed&&oracle->first_live==0&&oracle->trace[oracle->used-1].state==int(AnticipationQueue::State::Ahead),"live-to-ahead demotion completed");
 if(scenario=="long-starved")need(misses*block>9600,"starvation must exceed one entire cache");
 auto first_missing=oracle->first_missing,last_missing=oracle->last_missing,first_live=oracle->first_live;auto exact=oracle->exact_frames;auto generations=oracle->generation_changes;auto missing_frames=oracle->missing_blocks*block;double pe=oracle->playback_error,me=oracle->monitor_error;
 oracle.reset();source.reset();reference.reset();input.reset();monitor.reset();sink.reset();cleanup.reset();
 std::cout<<"RESULT {\"passed\":true,\"scenario\":\""<<scenario<<"\",\"block\":"<<block<<",\"stall_render_frame\":"<<control.entered_at<<",\"release_audible_frame\":"<<control.release_at<<",\"first_missing_frame\":"<<first_missing<<",\"last_missing_frame\":"<<last_missing<<",\"cache_underruns\":"<<misses<<",\"exact_playback_frames\":"<<exact<<",\"captured_frames\":"<<control.audible_end<<",\"silent_playback_frames\":"<<missing_frames<<",\"lost_audio_ms\":"<<double(missing_frames)/48<<",\"recovery_frame\":"<<(last_missing<0?-1:last_missing+block)<<",\"generation_changes\":"<<generations<<",\"transition_request_frame\":"<<requested_arm<<",\"oracle_mutation\":\""<<mutation<<"\",\"monitor_sample_error\":"<<me<<",\"playback_sample_error_outside_missing_blocks\":"<<pe<<",\"all_samples_finite\":true,\"ownership_races\":0,\"live_handoffs\":"<<handoffs<<",\"first_live_frame\":"<<first_live<<",\"max_route_render_us\":"<<double(control.max_render_ns.load())/1000<<",\"physical_roundtrip_measured\":false}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(std::exception const& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
