#define ANTICIPATION_CALLBACK_HELPERS_ONLY
#include "anticipation_callback_probe.cc"
#include <fstream>
int main(int argc,char** argv){try{
 need(argc==2,"session path");need(std::string(g_get_home_dir())==getenv("ARDOUR_PROBE_HOME"),"preferences isolation");
 need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","AnticipationCapacity","");
 need(bool(backend),"backend");need(backend->set_driver("Realtime")==0,"driver");
 need(backend->set_sample_rate(48000)==0,"sample rate");
 const unsigned block=getenv("ARDOUR_PROBE_BLOCK")?std::stoul(getenv("ARDOUR_PROBE_BLOCK")):64;
 const unsigned chains=getenv("ARDOUR_PROBE_CHAINS")?std::stoul(getenv("ARDOUR_PROBE_CHAINS")):8;
 const unsigned inserts=getenv("ARDOUR_PROBE_INSERTS")?std::stoul(getenv("ARDOUR_PROBE_INSERTS")):32;
 const bool cached=!getenv("ARDOUR_PROBE_DIRECT");
 need(backend->set_buffer_size(block)==0&&engine->start()==0,"start");
 auto* s=new Session(*engine,argv[1],"capacity-probe",nullptr,"",true);engine->set_session(nullptr);
 unsigned misses=0,late=0;double peak=0,wall=0,bridge_max=0;std::vector<float> captured;
 {
  Fixture live(*s,"Live source"),bus(*s,"Shared bus",true,false);
  connect_send(*s,live.route,bus.route);
  std::vector<std::unique_ptr<Fixture>> sources;
  std::vector<std::shared_ptr<Bridge>> bridges;
  for(unsigned i=0;i<chains;++i){
   sources.emplace_back(new Fixture(*s,("Private chain "+std::to_string(i)).c_str(),true,true,false,inserts));
   auto route=s->new_audio_route(2,2,{},1,"Playback "+std::to_string(i),PresentationInfo::AudioBus,0).front();
   auto bridge=std::make_shared<Bridge>(*s,*sources.back(),block,cached,false);
   need(route->add_processor(bridge,PreFader)==0,"bridge");bridges.push_back(bridge);
   auto send=connect_send(*s,route,bus.route);send->gain_control()->set_value(.37/chains,PBD::Controllable::NoGroup);
  }
  auto tap=std::make_shared<Tap>(*s,block);need(bus.route->add_processor(tap,PostFader)==0,"tap");
  s->resort_routes();std::cerr<<"DSP threads="<<engine->process_thread_count()<<" graph_present="<<s->plot_process_graph(std::string(argv[1])+"/graph.dot")<<std::endl;
  if(cached){
   for(auto& bridge:bridges)bridge->start_worker();
   for(auto& bridge:bridges)while(bridge->write.load()<bridge->capacity)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  engine->dsp_stats[AudioEngine::ProcessCallback].queue_reset();s->request_roll();engine->set_session(s);
  const auto begin=Clock::now();const auto deadline=begin+std::chrono::seconds(60);
  while(!bridges.front()->done.load(std::memory_order_acquire)&&Clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
  engine->set_session(nullptr);wall=std::chrono::duration<double>(Clock::now()-begin).count();
  if(cached)for(auto& bridge:bridges)bridge->stop_worker();
  for(auto& bridge:bridges){need(bridge->done&&bridge->bad_size==0,"incomplete callback");misses+=bridge->misses;late+=bridge->late;bridge_max=std::max(bridge_max,bridge->max_us);std::cerr<<"Source processed on "<<bridge->callback_thread_name<<std::endl;}
  need(tap->k==total_blocks,"incomplete bus");captured=std::move(tap->captured);peak=tap->peak;
 }
 delete s;engine->stop();
 PBD::microseconds_t minimum,maximum;double average,deviation;
 need(engine->dsp_stats[AudioEngine::ProcessCallback].get_stats(minimum,maximum,average,deviation),"engine timing");
 const std::string capture=std::string(argv[1])+"/capacity-output.f32";
 {std::ofstream f(capture,std::ios::binary);f.write(reinterpret_cast<const char*>(captured.data()),captured.size()*sizeof(float));need(bool(f),"capture write");}
 need(peak>1e-4,"silent capacity test");
 std::cout<<"RESULT {\"passed\":"<<(misses==0?"true":"false")<<",\"mode\":\""<<(cached?"anticipated":"direct")<<"\",\"backend\":\"Dummy Realtime\",\"block\":"<<block<<",\"chains\":"<<chains<<",\"inserts_per_chain\":"<<inserts<<",\"verified_blocks\":"<<total_blocks<<",\"cache_underruns\":"<<misses<<",\"bridge_overruns\":"<<late<<",\"bridge_max_us\":"<<bridge_max<<",\"engine_mean_us\":"<<average<<",\"engine_max_us\":"<<maximum<<",\"period_us\":"<<(1e6*block/48000.)<<",\"wall_seconds\":"<<wall<<",\"peak\":"<<peak<<",\"capture\":\""<<capture<<"\",\"hardware_deadlines_measured\":false}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return misses?1:0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
