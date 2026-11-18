#define ANTICIPATION_NATIVE_HELPERS_ONLY
#include "anticipation_native_route_probe.cc"
#include <fstream>

struct NativeCapacityTap : Processor {
 std::vector<float> captured;std::atomic<unsigned> frames{0};unsigned limit,max_at_frame=0;double peak=0,observed_max=0;
 NativeCapacityTap(Session& s,unsigned count):Processor(s,"Native capacity capture",s),captured(count*2),limit(count){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void run(BufferSet& b,samplepos_t,samplepos_t,double speed,pframes_t n,bool)override{
  unsigned offset=frames.load();if(speed!=1||offset>=limit)return;n=std::min(n,limit-offset);
  PBD::microseconds_t lo,hi;double mean,sd;
  if(AudioEngine::instance()->dsp_stats[AudioEngine::ProcessCallback].get_stats(lo,hi,mean,sd)&&double(hi)>observed_max){observed_max=hi;max_at_frame=offset;}
  for(unsigned i=0;i<n;++i)for(unsigned c=0;c<2;++c){float v=b.get_audio(c).data()[i];captured[2*(offset+i)+c]=v;peak=std::max(peak,double(std::abs(v)));}
  frames.store(offset+n,std::memory_order_release);
 }
};
int main(int argc,char** argv){try{
 need(argc==2,"session path");need(getenv("ARDOUR_PROBE_HOME")&&std::string(g_get_home_dir())==getenv("ARDOUR_PROBE_HOME"),"preferences isolation");need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();
 auto* engine=AudioEngine::create();std::string backend_name=getenv("ARDOUR_PROBE_BACKEND")?getenv("ARDOUR_PROBE_BACKEND"):"None (Dummy)";
 auto backend=engine->set_backend(backend_name,"NativeCapacity","");need(bool(backend),"backend");
 if(backend_name=="CoreAudio"){
  backend->enumerate_input_devices();for(auto const& d:backend->enumerate_output_devices())std::cerr<<"OUTPUT_DEVICE "<<d.name<<std::endl;
  need(backend->set_input_device_name(AudioBackend::get_standard_device_name(AudioBackend::DeviceNone))==0&&backend->set_output_device_name(getenv("ARDOUR_PROBE_OUTPUT_DEVICE")?getenv("ARDOUR_PROBE_OUTPUT_DEVICE"):"BlackHole 2ch")==0,"CoreAudio device");
 }else need(backend->set_driver("Realtime")==0,"driver");
 unsigned block=getenv("ARDOUR_PROBE_BLOCK")?std::stoul(getenv("ARDOUR_PROBE_BLOCK")):64;
 unsigned chains=getenv("ARDOUR_PROBE_CHAINS")?std::stoul(getenv("ARDOUR_PROBE_CHAINS")):8;
 unsigned inserts=getenv("ARDOUR_PROBE_INSERTS")?std::stoul(getenv("ARDOUR_PROBE_INSERTS")):64;
 unsigned blocks=getenv("ARDOUR_PROBE_BLOCKS")?std::stoul(getenv("ARDOUR_PROBE_BLOCKS")):10000;
 bool anticipated=!getenv("ARDOUR_PROBE_DIRECT");
 need(backend->set_sample_rate(48000)==0&&backend->set_buffer_size(block)==0&&engine->start()==0,"engine");
 auto* s=new NativeSession(*engine,argv[1],"native-capacity",nullptr,"",true);engine->set_session(nullptr);
 std::atomic<bool> measuring{false};std::atomic<unsigned> xruns{0};PBD::ScopedConnection xrun;
 engine->Xrun.connect_same_thread(xrun,[&]{if(measuring)++xruns;});
 auto wait=[&](auto predicate,const char* msg){auto deadline=Clock::now()+std::chrono::seconds(60);while(Clock::now()<deadline){{PBD::Mutex::Lock lock(engine->process_lock());if(predicate())return;}std::this_thread::sleep_for(std::chrono::milliseconds(2));}throw std::runtime_error(msg);};
 uint64_t misses=0;double peak=0,observed=0;std::string capture=std::string(argv[1])+"/native-capacity.f32";
 {
  Fixture live(*s,"Live source",true),bus(*s,"Shared live bus",true,false);
  live.route->set_anticipative_processing_disabled(true);bus.route->set_anticipative_processing_disabled(true);
  need(live.route->replace_processor(live.probe,std::make_shared<NativeInput>(*s))==0,"live input");connect_send(*s,live.route,bus.route);
  std::vector<std::unique_ptr<Fixture>> sources;
  for(unsigned i=0;i<chains;++i){
   sources.emplace_back(new Fixture(*s,("Native chain "+std::to_string(i)).c_str(),true,true,true,inserts));auto route=sources.back()->route;
   need(route->replace_processor(sources.back()->probe,std::make_shared<NativeInput>(*s))==0,"playback input");
   auto send=connect_send(*s,route,bus.route);send->gain_control()->set_value(.0037/chains,PBD::Controllable::NoGroup);
  }
  auto tap=std::make_shared<NativeCapacityTap>(*s,block*blocks);need(bus.route->add_processor(tap,PostFader)==0,"tap");
  s->resort_routes();s->calculate_latency();engine->set_session(s);wait([&]{return s->setup_settled()&&!s->global_locate_pending();},"setup");engine->set_session(nullptr);
  for(auto const& route:*s->get_routes())route->foreach_processor([](std::weak_ptr<Processor> weak){auto insert=std::dynamic_pointer_cast<PluginInsert>(weak.lock());if(insert)for(unsigned i=0;i<insert->get_count();++i)insert->plugin(i)->flush();});
  // Plugin activation resets parameter smoothing too. Settle the silent stopped
  // graph before measuring; otherwise asynchronous start preparation can give
  // separate runs different numbers of smoothing blocks before their first note.
  auto silent_start=engine->processed_samples();engine->set_session(s);
  wait([&]{return engine->processed_samples()>=silent_start+64*block&&s->setup_settled();},"silent parameter settling");engine->set_session(nullptr);
  Config->set_anticipation_milliseconds(200);Config->set_anticipative_processing(anticipated);
  engine->dsp_stats[AudioEngine::ProcessCallback].queue_reset();measuring=true;s->request_roll();engine->set_session(s);
  wait([&]{return tap->frames.load()>=block*blocks;},"capture incomplete");engine->set_session(nullptr);measuring=false;
  auto& plan=s->anticipative_process();plan.stop();
  if(anticipated)for(auto const& source:sources){auto* node=plan.find_node(source->route.get());need(node,"native node");misses+=node->queue->underruns();need(!node->queue->discontinuities(),"timeline fault");}
  peak=tap->peak;observed=tap->observed_max;std::cerr<<"MAX_CALLBACK first_observed_frame="<<tap->max_at_frame<<" us="<<observed<<std::endl;
  {std::ofstream f(capture,std::ios::binary);f.write(reinterpret_cast<const char*>(tap->captured.data()),tap->captured.size()*sizeof(float));need(bool(f),"capture");}
  plan.clear();
 }
 PBD::microseconds_t lo,hi;double mean,sd;need(engine->dsp_stats[AudioEngine::ProcessCallback].get_stats(lo,hi,mean,sd),"timing");observed=std::max(observed,double(hi));
 engine->stop();delete s;need(peak>1e-4,"silent capture");
 std::cout<<"RESULT {\"passed\":"<<(misses==0?"true":"false")<<",\"native_registered_routes\":true,\"mode\":\""<<(anticipated?"anticipated":"direct")<<"\",\"backend\":\""<<backend_name<<"\",\"block\":"<<block<<",\"chains\":"<<chains<<",\"inserts_per_chain\":"<<inserts<<",\"verified_blocks\":"<<blocks<<",\"cache_underruns\":"<<misses<<",\"engine_mean_us\":"<<mean<<",\"observed_engine_max_us\":"<<observed<<",\"playback_xruns\":"<<xruns.load()<<",\"capture\":\""<<capture<<"\",\"duration_seconds\":"<<double(block*blocks)/48000<<"}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return misses?1:0;
}catch(std::exception const& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
