#define ANTICIPATION_NATIVE_HELPERS_ONLY
#include "anticipation_native_route_probe.cc"
#include <fstream>
#include <sys/mman.h>
#include <unistd.h>

struct NativeCapacityTap : Processor {
 std::vector<float> captured;std::atomic<unsigned> frames{0};std::atomic<bool>& rolling;unsigned limit,max_at_frame=0;double peak=0,observed_max=0;bool finite=true,hardware_block_consistent=true;unsigned expected_block;
 struct TraceEvent { unsigned frame; double gap_us; uint64_t misses,min_depth; unsigned xruns; };
 bool trace_enabled=getenv("ARDOUR_PROBE_TRACE");std::vector<TraceEvent> trace;size_t trace_count=0;
 std::vector<Route*> trace_routes;std::vector<AnticipationQueue*> trace_queues;
 std::atomic<unsigned>* trace_xruns=nullptr;Clock::time_point last_callback{};
 uint64_t last_misses=0,last_depth=UINT64_MAX;unsigned last_xruns=0,last_trace_frame=0;
 void* locked_begin=nullptr;size_t locked_bytes=0;
 NativeCapacityTap(Session& s,unsigned count,std::atomic<bool>& active,unsigned quantum):Processor(s,"Native capacity capture",s),captured(count*2),rolling(active),limit(count),expected_block(quantum){
  if(trace_enabled)trace.resize(20000);
  if(getenv("ARDOUR_PROBE_LOCK_CAPTURE")){
   // Keep the long test recorder's cold future pages out of the audio deadline.
   size_t page=getpagesize();uintptr_t address=reinterpret_cast<uintptr_t>(captured.data());
   uintptr_t begin=address/page*page;locked_begin=reinterpret_cast<void*>(begin);
   locked_bytes=(address+captured.size()*sizeof(float)-begin+page-1)/page*page;
   need(mlock(locked_begin,locked_bytes)==0,"capture memory lock failed");
   std::cerr<<"CAPTURE_MEMORY locked_bytes="<<locked_bytes<<std::endl;
  }
 }
 ~NativeCapacityTap(){if(locked_bytes)munlock(locked_begin,locked_bytes);}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void run(BufferSet& b,samplepos_t,samplepos_t,double speed,pframes_t n,bool)override{
  unsigned offset=frames.load();if(speed!=1||offset>=limit)return;n=std::min(n,limit-offset);rolling.store(true,std::memory_order_release);hardware_block_consistent=hardware_block_consistent&&AudioEngine::instance()->samples_per_cycle()==expected_block;
  if(trace_enabled){
   auto now=Clock::now();double gap=last_callback==Clock::time_point{}?0:std::chrono::duration<double,std::micro>(now-last_callback).count();last_callback=now;
   uint64_t misses=0,depth=UINT64_MAX;
   for(size_t i=0;i<trace_routes.size();++i){
    if(!trace_queues[i]){auto node=_session.anticipative_process().find_node(trace_routes[i]);if(node)trace_queues[i]=node->queue.get();}
    if(auto queue=trace_queues[i]){misses+=queue->underruns();depth=std::min(depth,queue->queued_blocks());}
   }
   unsigned xruns=trace_xruns->load(std::memory_order_relaxed);
   if(offset==0||misses!=last_misses||xruns!=last_xruns||gap>2e6*expected_block/48000||(depth<last_depth&&depth<10)||offset-last_trace_frame>=48000){
    if(trace_count<trace.size())trace[trace_count++]={offset,gap,misses,depth,xruns};
    last_trace_frame=offset;
   }
   last_misses=misses;last_depth=depth;last_xruns=xruns;
  }
  PBD::microseconds_t lo,hi;double mean,sd;
  if(AudioEngine::instance()->dsp_stats[AudioEngine::ProcessCallback].get_stats(lo,hi,mean,sd)&&double(hi)>observed_max){observed_max=hi;max_at_frame=offset;}
  for(unsigned i=0;i<n;++i)for(unsigned c=0;c<2;++c){float v=b.get_audio(c).data()[i];captured[2*(offset+i)+c]=v;finite=finite&&std::isfinite(v);peak=std::max(peak,double(std::abs(v)));}
  frames.store(offset+n,std::memory_order_release);
 }
};
int main(int argc,char** argv){try{
 need(argc==2,"session path");need(getenv("ARDOUR_PROBE_HOME")&&std::string(g_get_home_dir())==getenv("ARDOUR_PROBE_HOME"),"preferences isolation");need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();
 if(getenv("ARDOUR_PROBE_RT_THREADS"))Config->set_processor_usage(std::stoi(getenv("ARDOUR_PROBE_RT_THREADS")));
 auto* engine=AudioEngine::create();std::string backend_name=getenv("ARDOUR_PROBE_BACKEND")?getenv("ARDOUR_PROBE_BACKEND"):"None (Dummy)";
 auto backend=engine->set_backend(backend_name,"NativeCapacity","");need(bool(backend),"backend");
 if(backend_name=="CoreAudio"){
  backend->enumerate_input_devices();for(auto const& d:backend->enumerate_output_devices())std::cerr<<"OUTPUT_DEVICE "<<d.name<<std::endl;
  need(backend->set_input_device_name(AudioBackend::get_standard_device_name(AudioBackend::DeviceNone))==0&&backend->set_output_device_name(getenv("ARDOUR_PROBE_OUTPUT_DEVICE")?getenv("ARDOUR_PROBE_OUTPUT_DEVICE"):"BlackHole 2ch")==0,"CoreAudio device");
 }else need(backend->set_driver("Realtime")==0,"driver");
 unsigned block=getenv("ARDOUR_PROBE_BLOCK")?std::stoul(getenv("ARDOUR_PROBE_BLOCK")):64;
 unsigned chains=getenv("ARDOUR_PROBE_CHAINS")?std::stoul(getenv("ARDOUR_PROBE_CHAINS")):8;
 unsigned inserts=getenv("ARDOUR_PROBE_INSERTS")?std::stoul(getenv("ARDOUR_PROBE_INSERTS")):64;
 unsigned blocks=getenv("ARDOUR_PROBE_BLOCKS")?std::stoul(getenv("ARDOUR_PROBE_BLOCKS")):512;
 bool anticipated=!getenv("ARDOUR_PROBE_DIRECT");bool bounded_preset=!getenv("ARDOUR_PROBE_LEGACY_GAIN_PRESET");
 need(backend->set_sample_rate(48000)==0&&backend->set_buffer_size(block)==0&&engine->start()==0,"engine");
 std::cerr<<"DSP_THREADS configured="<<Config->get_processor_usage()<<" actual="<<how_many_dsp_threads()<<std::endl;
 need(backend->buffer_size()==block&&backend->sample_rate()==48000,"actual device format differs");
 auto* s=new NativeSession(*engine,argv[1],"native-capacity",nullptr,"",true);engine->set_session(nullptr);
 std::atomic<bool> measuring{false},rolling{false};std::atomic<unsigned> xruns{0},rolling_xruns{0};PBD::ScopedConnection xrun;
 engine->Xrun.connect_same_thread(xrun,[&]{if(measuring){++xruns;if(rolling.load(std::memory_order_acquire))++rolling_xruns;}});
 auto wait=[&](auto predicate,const char* msg,unsigned seconds=60){auto deadline=Clock::now()+std::chrono::seconds(seconds);while(Clock::now()<deadline){{PBD::Mutex::Lock lock(engine->process_lock());if(predicate())return;}std::this_thread::sleep_for(std::chrono::milliseconds(2));}throw std::runtime_error(msg);};
 uint64_t misses=0;double peak=0,observed=0;std::string capture=std::string(argv[1])+"/native-capacity.f32";
 {
  Fixture live(*s,"Live source",true),bus(*s,"Shared live bus",true,false);
  live.route->set_anticipative_processing_disabled(true);bus.route->set_anticipative_processing_disabled(true);
  need(live.route->replace_processor(live.probe,std::make_shared<NativeInput>(*s))==0,"live input");connect_send(*s,live.route,bus.route);
  std::vector<std::unique_ptr<Fixture>> sources;
  for(unsigned i=0;i<chains;++i){
   sources.emplace_back(new Fixture(*s,("Native chain "+std::to_string(i)).c_str(),true,true,true,inserts));auto route=sources.back()->route;
   need(route->replace_processor(sources.back()->probe,std::make_shared<NativeInput>(*s))==0,"playback input");
   auto send=connect_send(*s,route,bus.route);send->gain_control()->set_value((bounded_preset?1.0:.0037)/chains,PBD::Controllable::NoGroup);
  }
  // Keep long cascades at usable levels without bypassing any delay DSP.
  if(bounded_preset)for(auto const& route:*s->get_routes())route->foreach_processor([](std::weak_ptr<Processor> weak){
   auto insert=std::dynamic_pointer_cast<PluginInsert>(weak.lock());if(!insert)return;
   for(auto value:std::vector<std::pair<unsigned,double>>{{7,5},{8,10}})
    insert->automation_control(Evoral::Parameter(PluginAutomation,0,value.first))->set_value(value.second,PBD::Controllable::NoGroup);
  });
  auto tap=std::make_shared<NativeCapacityTap>(*s,block*blocks,rolling,block);need(bus.route->add_processor(tap,PostFader)==0,"tap");
  tap->trace_xruns=&rolling_xruns;for(auto const& source:sources)tap->trace_routes.push_back(source->route.get());tap->trace_queues.resize(sources.size(),nullptr);
  s->resort_routes();s->calculate_latency();engine->set_session(s);wait([&]{return s->setup_settled()&&!s->global_locate_pending();},"setup");engine->set_session(nullptr);
  for(auto const& route:*s->get_routes())route->foreach_processor([](std::weak_ptr<Processor> weak){auto insert=std::dynamic_pointer_cast<PluginInsert>(weak.lock());if(insert)for(unsigned i=0;i<insert->get_count();++i)insert->plugin(i)->flush();});
  // Plugin activation resets parameter smoothing too. Settle the silent stopped
  // graph before measuring; otherwise asynchronous start preparation can give
  // separate runs different numbers of smoothing blocks before their first note.
  auto silent_start=engine->processed_samples();engine->set_session(s);
  wait([&]{return engine->processed_samples()>=silent_start+64*block&&s->setup_settled();},"silent parameter settling");engine->set_session(nullptr);
  // Hardware paces the test; synthetic stress audio is captured internally.
  unsigned physical_disconnected=0;
  {PBD::Mutex::Lock lock(engine->process_lock());for(auto const& route:*s->get_routes())for(unsigned c=0;c<route->output()->n_ports().n_audio();++c){
   auto port=route->output()->nth(c);std::vector<std::string> peers;port->get_connections(peers);
   for(auto const& peer:peers)if(engine->port_is_physical(peer)){need(port->disconnect(peer)==0,"physical output disconnect");++physical_disconnected;}
   need(!port->physically_connected(),"stress output connected to hardware");
  }}
  std::cerr<<"PHYSICAL_OUTPUT disconnected="<<physical_disconnected<<" connected=false"<<std::endl;
  Config->set_anticipation_milliseconds(200);Config->set_anticipative_processing(anticipated);
  engine->dsp_stats[AudioEngine::ProcessCallback].queue_reset();measuring=true;s->request_roll();engine->set_session(s);
  // Capture progress is atomic. Taking process_lock here can make the audio
  // callback fail its try-lock and manufacture an xrun in the measurement.
  auto capture_deadline=Clock::now()+std::chrono::seconds(60+unsigned(double(block)*blocks/48000));
  while(tap->frames.load(std::memory_order_acquire)<block*blocks){
   need(Clock::now()<capture_deadline,"capture incomplete");
   std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  engine->set_session(nullptr);measuring=false;
  auto& plan=s->anticipative_process();plan.stop();
  if(anticipated)for(auto const& source:sources){auto* node=plan.find_node(source->route.get());need(node,"native node");misses+=node->queue->underruns();need(!node->queue->discontinuities(),"timeline fault");}
  need(tap->hardware_block_consistent&&backend->buffer_size()==block,"device buffer changed during capture");need(tap->finite,"non-finite capture");peak=tap->peak;need(!bounded_preset||peak<=1,"capture exceeds unity");observed=tap->observed_max;std::cerr<<"MAX_CALLBACK first_observed_frame="<<tap->max_at_frame<<" us="<<observed<<std::endl;
  {std::ofstream f(capture,std::ios::binary);f.write(reinterpret_cast<const char*>(tap->captured.data()),tap->captured.size()*sizeof(float));need(bool(f),"capture");}
  if(tap->trace_enabled){
   std::ofstream f(std::string(argv[1])+"/capacity-trace.csv");f<<"frame,gap_us,cache_underruns,min_queue_blocks,rolling_xruns\n";
   for(size_t i=0;i<tap->trace_count;++i){auto const& e=tap->trace[i];f<<e.frame<<","<<e.gap_us<<","<<e.misses<<","<<e.min_depth<<","<<e.xruns<<"\n";}
   std::cerr<<"TRACE events="<<tap->trace_count<<" capacity="<<tap->trace.size()<<std::endl;
   for(size_t i=0;i<tap->trace_queues.size();++i)if(auto q=tap->trace_queues[i])std::cerr<<"QUEUE index="<<i<<" misses="<<q->underruns()<<" depth="<<q->queued_blocks()<<std::endl;
  }
  plan.clear();
 }
 PBD::microseconds_t lo,hi;double mean,sd;need(engine->dsp_stats[AudioEngine::ProcessCallback].get_stats(lo,hi,mean,sd),"timing");observed=std::max(observed,double(hi));
 engine->stop();delete s;need(peak>1e-4,"silent capture");
 std::cout<<"RESULT {\"passed\":"<<(misses==0?"true":"false")<<",\"native_registered_routes\":true,\"mode\":\""<<(anticipated?"anticipated":"direct")<<"\",\"backend\":\""<<backend_name<<"\",\"block\":"<<block<<",\"chains\":"<<chains<<",\"inserts_per_chain\":"<<inserts<<",\"verified_blocks\":"<<blocks<<",\"cache_underruns\":"<<misses<<",\"engine_mean_us\":"<<mean<<",\"observed_engine_max_us\":"<<observed<<",\"playback_xruns\":"<<xruns.load()<<",\"rolling_xruns\":"<<rolling_xruns.load()<<",\"startup_xruns\":"<<(xruns.load()-rolling_xruns.load())<<",\"hardware_block_consistent\":true,\"rt_dsp_threads\":"<<how_many_dsp_threads()<<",\"effect_preset\":\""<<(bounded_preset?"bounded_5wet_10feedback":"legacy_70wet_55feedback")<<"\",\"capture_peak\":"<<peak<<",\"capture\":\""<<capture<<"\",\"duration_seconds\":"<<double(block*blocks)/48000<<"}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return misses?1:0;
}catch(std::exception const& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
