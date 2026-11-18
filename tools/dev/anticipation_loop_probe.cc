// Predictable loop wraps reuse the same DSP history and monotonic queue indices.
#define ANTICIPATION_CALLBACK_HELPERS_ONLY
#include "anticipation_callback_probe.cc"
#include "ardour/location.h"
struct ClockWitness : Processor {
 samplepos_t expected;samplecnt_t last_device=-1;unsigned calls=0,repeated_device_clock=0;
 std::atomic<unsigned> entered{0};
 ClockWitness(Session& s,samplepos_t origin):Processor(s,"Unwrapped render clock",s),expected(origin){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void run(BufferSet&,samplepos_t start,samplepos_t,double,pframes_t n,bool)override{
  need(entered.fetch_add(1)==0,"concurrent DSP ownership");auto* time=ProcessThread::render_time();
  need(time&&time->continuous_at(start)==expected&&time->continuous_at(start-257)==expected-257,"render clock discontinuity");
  const auto device=AudioEngine::instance()->processed_samples();if(device==last_device)++repeated_device_clock;last_device=device;
  expected+=n;++calls;--entered;
 }
};
struct LoopTap : Processor {
 std::vector<float> captured;unsigned cursor=0;double peak=0;
 LoopTap(Session& s,unsigned frames):Processor(s,"Loop capture",s),captured(2*frames){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void run(BufferSet& b,samplepos_t,samplepos_t,double speed,pframes_t n,bool)override{
  need(ProcessThread::render_time()==nullptr,"render clock leaked to normal bus");
  if(speed!=1||2*cursor>=captured.size())return;
  need(2*(cursor+n)<=captured.size(),"loop capture chunk mismatch");const BufferSet& input=b;
  for(unsigned i=0;i<n;++i)for(unsigned c=0;c<2;++c){auto value=input.get_audio(c).data()[i];captured[2*(cursor+i)+c]=value;peak=std::max(peak,double(std::abs(value)));}cursor+=n;
 }
};
int main(int argc,char** argv){try{
 need(argc==2,"session path");need(std::string(g_get_home_dir())==getenv("ARDOUR_PROBE_HOME"),"preferences isolation");
 need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();Config->set_loop_is_mode(true);
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","LoopProbe","");
 const unsigned block=getenv("ARDOUR_PROBE_BLOCK")?std::stoul(getenv("ARDOUR_PROBE_BLOCK")):64;
 need(bool(backend)&&backend->set_driver("Realtime")==0&&backend->set_sample_rate(48000)==0&&backend->set_buffer_size(block)==0&&engine->start()==0,"engine");
 auto* s=new FixtureSession(*engine,argv[1],"loop-probe",nullptr,"",true);engine->set_session(nullptr);
 double error=0;unsigned misses=0,late=0,wraps=0,cutover=0,frames=0,loop_length=0;
 {
  auto* location=s->locations()->auto_loop_location();
  if(!location){location=new Location(*s,timepos_t(samplepos_t(48000)),timepos_t(samplepos_t(57973)),"Nonaligned loop",Location::IsAutoLoop);s->locations()->add(location);}
  else {need(location->set_end(timepos_t(samplepos_t(57973)))==0&&location->set_start(timepos_t(samplepos_t(48000)))==0,"loop range");}
  s->set_auto_loop_location(location);
  // Beat-domain sessions can round the requested endpoints. Schedule the
  // effective host range, rather than the values originally requested by UI.
  const auto loop_start=location->start_sample(),loop_end=location->end_sample();loop_length=loop_end-loop_start;
  Fixture source(*s,"Future loop DSP",true,true,false),reference(*s,"Reference loop DSP",true,true,false);
  auto clock=std::make_shared<ClockWitness>(*s,loop_start),reference_clock=std::make_shared<ClockWitness>(*s,loop_start);
  need(source.route->add_processor(clock,PreFader)==0&&reference.route->add_processor(reference_clock,PreFader)==0,"clock witnesses");source.endpoint=clock;reference.endpoint=reference_clock;
  auto playlist=make_disk_playlist(*s,std::string(argv[1])+"/source.wav");use_disk_input(*s,source,playlist);use_disk_input(*s,reference,playlist);
  Fixture output(*s,"Future loop output",false,false),reference_output(*s,"Reference loop output",false,false),bus(*s,"Future loop bus",true,false),reference_bus(*s,"Reference loop bus",true,false);
  auto bridge=std::make_shared<Bridge>(*s,source,block),reference_bridge=std::make_shared<Bridge>(*s,reference,block,false);
  need(output.route->add_processor(bridge,PreFader)==0&&reference_output.route->add_processor(reference_bridge,PreFader)==0,"bridges");
  connect_send(*s,output.route,bus.route);connect_send(*s,reference_output.route,reference_bus.route);
  unsigned cycle_left=block;samplepos_t cursor=loop_start;
  for(unsigned k=0;k<total_blocks;++k){
   const auto n=pframes_t(std::min(samplecnt_t(cycle_left),loop_end-cursor));need(n>0,"empty loop chunk");
   bridge->render_plan.push_back({cursor,n,loop_start+frames});cursor+=n;frames+=n;cycle_left-=n;if(!cycle_left)cycle_left=block;
   if(cursor==loop_end){cursor=loop_start;++wraps;}
  }
  reference_bridge->render_plan=bridge->render_plan;bridge->check_position=reference_bridge->check_position=true;
  auto tap=std::make_shared<LoopTap>(*s,frames),reference_tap=std::make_shared<LoopTap>(*s,frames);
  need(bus.route->add_processor(tap,PostFader)==0&&reference_bus.route->add_processor(reference_tap,PostFader)==0,"captures");s->resort_routes();
  s->request_play_loop(true);
  std::cerr<<"LOOP actual_range="<<location->start_sample()<<","<<location->end_sample()<<" planned="<<loop_start<<","<<loop_end<<std::endl;
  engine->set_session(s);bool ready=false;const auto setup_deadline=Clock::now()+std::chrono::seconds(5);
  while(!ready&&Clock::now()<setup_deadline){
   {PBD::Mutex::Lock lock(engine->process_lock());ready=s->setup_settled()&&s->get_play_loop()&&!s->global_locate_pending()&&s->transport_stopped()&&s->transport_sample()==loop_start;}
   if(!ready)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  engine->set_session(nullptr);need(ready,"loop setup incomplete");
  bridge->start_worker();while(bridge->write.load()<bridge->capacity)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  s->request_roll();engine->set_session(s);const auto deadline=Clock::now()+std::chrono::seconds(20);
  while(!bridge->done.load()&&Clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  engine->set_session(nullptr);bridge->stop_worker();
  std::cerr<<"LOOP chunks="<<bridge->k<<","<<reference_bridge->k<<" bad_size="<<bridge->bad_size<<","<<reference_bridge->bad_size<<" wraps="<<wraps<<" frames="<<tap->cursor<<","<<reference_tap->cursor<<" planned_frames="<<frames<<std::endl;
  need(bridge->done&&reference_bridge->done&&bridge->bad_size==0&&reference_bridge->bad_size==0&&tap->cursor==frames&&reference_tap->cursor==frames,"loop callbacks incomplete");
  for(unsigned i=0;i<tap->captured.size();++i)error=std::max(error,double(std::abs(tap->captured[i]-reference_tap->captured[i])));
  misses=bridge->misses;late=bridge->late;cutover=bridge->cutover;
  std::cerr<<"RENDER CLOCK calls="<<clock->calls<<","<<reference_clock->calls<<" final_position="<<clock->expected<<","<<reference_clock->expected<<" repeated_device_clock="<<clock->repeated_device_clock<<std::endl;
  need(clock->calls==total_blocks&&reference_clock->calls==total_blocks&&clock->expected==loop_start+frames&&clock->expected==reference_clock->expected&&clock->repeated_device_clock>0,"unwrapped clock not exercised");
  need(wraps>4&&error<1e-7&&misses==0&&tap->peak>1e-4&&cutover>arm_block,"loop state mismatch");
 }
 delete s;engine->stop();PBD::microseconds_t minimum,maximum;double average,deviation;
 need(engine->dsp_stats[AudioEngine::ProcessCallback].get_stats(minimum,maximum,average,deviation),"timing");
 std::cout<<"RESULT {\"passed\":true,\"normal_session_callback\":true,\"blocks\":["<<block<<"],\"loop_length\":"<<loop_length<<",\"loop_wraps\":"<<wraps<<",\"verified_frames\":"<<frames<<",\"monitor_cutover\":"<<cutover<<",\"max_sample_error\":"<<error<<",\"cache_underruns\":"<<misses<<",\"bridge_deadline_overruns\":"<<late<<",\"engine_mean_us\":"<<average<<",\"engine_max_us\":"<<maximum<<",\"hardware_latency_measured\":false}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
