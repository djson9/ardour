#define ANTICIPATION_NATIVE_HELPERS_ONLY
#include "anticipation_native_route_probe.cc"
struct MonitorCompare : Processor {
 std::atomic<unsigned> frames{0},compared{0};std::atomic<unsigned> compare_from{UINT_MAX};double error=0,peak=0;
 MonitorCompare(Session& s):Processor(s,"Current-cycle monitor oracle",s){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void run(BufferSet& b,samplepos_t,samplepos_t,double speed,pframes_t n,bool)override{
  if(speed!=1)return;unsigned start=frames.load();
  if(start>=compare_from.load())for(unsigned c=0;c<2;++c)for(unsigned i=0;i<n;++i){error=std::max(error,double(std::abs(b.get_audio(c).data()[i]-b.get_audio(c+2).data()[i])));peak=std::max(peak,double(std::abs(b.get_audio(c+2).data()[i])));++compared;}
  frames+=n;
 }
};
int main(int argc,char** argv){try{
 need(argc==2&&ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","NativeMonitor","");
 unsigned block=getenv("ARDOUR_PROBE_BLOCK")?std::stoul(getenv("ARDOUR_PROBE_BLOCK")):64;
 need(backend&&backend->set_driver("Realtime")==0&&backend->set_sample_rate(48000)==0&&backend->set_buffer_size(block)==0&&engine->start()==0,"engine");
 auto* s=new NativeSession(*engine,argv[1],"native-monitor",nullptr,"",true);engine->set_session(nullptr);
 double error=0,peak=0;unsigned compared=0;uint64_t misses=0,handoffs=0;
 {
  auto source=s->new_audio_route(2,2,{},1,"Playback source",PresentationInfo::AudioBus,0).front();add_effects(*s,source,true);
  auto monitored=s->new_audio_track(2,2,{},1,"Monitor transition",0,Normal,false).front();
  auto reference=s->new_audio_track(2,2,{},1,"Direct monitor reference",0,Normal,false).front();reference->set_anticipative_processing_disabled(true);
  auto sink=s->new_audio_route(4,4,{},1,"Monitor comparison",PresentationInfo::AudioBus,0).front();sink->set_anticipative_processing_disabled(true);
  auto oracle=std::make_shared<MonitorCompare>(*s);need(sink->add_processor(oracle,PreFader)==0,"oracle");
  {PBD::Mutex::Lock lock(engine->process_lock());for(unsigned c=0;c<2;++c){for(auto route:{monitored,reference})need(route->input()->connect(route->input()->nth(c),source->output()->nth(c)->name())==0,"input");need(sink->input()->connect(sink->input()->nth(c),monitored->output()->nth(c)->name())==0,"native output");need(sink->input()->connect(sink->input()->nth(c+2),reference->output()->nth(c)->name())==0,"reference output");}}
  monitored->monitoring_control()->set_value(MonitorDisk,PBD::Controllable::NoGroup);reference->monitoring_control()->set_value(MonitorInput,PBD::Controllable::NoGroup);
  s->resort_routes();s->calculate_latency();auto& plan=s->anticipative_process();
  auto wait=[&](auto predicate,const char* msg){auto end=Clock::now()+std::chrono::seconds(15);while(Clock::now()<end){{PBD::Mutex::Lock lock(engine->process_lock());if(predicate())return;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}throw std::runtime_error(msg);};
  engine->set_session(s);wait([&]{return s->setup_settled()&&!s->global_locate_pending();},"setup");s->set_anticipative_processing(true,200);s->request_roll();
  wait([&]{auto* node=plan.find_node(monitored.get());return oracle->frames>block*400&&node&&node->queue->state()==AnticipationQueue::State::Ahead;},"initial playback");
  monitored->monitoring_control()->set_value(MonitorInput,PBD::Controllable::NoGroup);
  wait([&]{auto* node=plan.find_node(monitored.get());return node&&node->queue->state()==AnticipationQueue::State::Live;},"monitor handoff");
  oracle->compare_from=oracle->frames+2048;
  wait([&]{return oracle->compared>=block*512;},"live comparison");
  {PBD::Mutex::Lock lock(engine->process_lock());oracle->compare_from=UINT_MAX;error=oracle->error;peak=oracle->peak;compared=oracle->compared;need(plan.find_node(source.get())->queue->state()==AnticipationQueue::State::Ahead,"independent playback still anticipates");}
  monitored->monitoring_control()->set_value(MonitorDisk,PBD::Controllable::NoGroup);
  wait([&]{return plan.find_node(monitored.get())->queue->state()==AnticipationQueue::State::Ahead;},"monitor off demotion");engine->set_session(nullptr);plan.stop();
  for(auto route:std::vector<std::shared_ptr<Route>>{source,monitored}){auto* node=plan.find_node(route.get());misses+=node->queue->underruns();handoffs+=node->queue->live_handoffs();need(!node->queue->discontinuities(),"timeline fault");}plan.clear();
 }
 engine->stop();delete s;need(error==0&&peak>1e-4&&misses==0&&handoffs==1,"current-cycle monitor equivalence");
 std::cout<<"RESULT {\"passed\":true,\"block\":"<<block<<",\"max_sample_error\":"<<error<<",\"compared_samples\":"<<compared<<",\"monitor_live_handoffs\":"<<handoffs<<",\"cache_underruns\":"<<misses<<",\"same_cycle_as_direct_monitor\":true,\"physical_roundtrip_measured\":false}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(std::exception const& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
