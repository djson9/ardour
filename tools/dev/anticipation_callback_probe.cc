// Normal Session/Graph/Route callback experiment, using the dummy backend.
// Reuse the correctness probe's real-Route fixtures, not its main function.
#define main correctness_main_unused
#include "anticipation_probe.cc"
#undef main
#include <chrono>
using Clock=std::chrono::steady_clock;
constexpr unsigned total_blocks=2048,queue_blocks=150,arm_block=512,demote_block=1024;

struct Bridge : Processor {
 enum Mode {Ahead,Drain,Live};
 Fixture& source;unsigned block;bool cache_enabled,transitions;
 std::vector<std::vector<float>> slots;
 std::atomic<unsigned> read{0},write{0},mode{Ahead};
 std::atomic<bool> acknowledged{false},quit{false},done{false};
 unsigned k=0,misses=0,late=0,cutover=0,bad_size=0;double max_us=0;
 double observed_engine_max=0,steady_engine_max=0;unsigned observed_engine_max_block=0,steady_engine_max_block=0;
 std::thread worker;
 char callback_thread_name[64]={0};
 Bridge(Session& s,Fixture& f,unsigned n,bool cached=true,bool handoffs=true):Processor(s,"Cache bridge",s),source(f),block(n),cache_enabled(cached),transitions(handoffs),slots(queue_blocks,std::vector<float>(2*n)){if(!cached)mode=Live;}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out) override {out=in;return true;}
 void start_worker(){worker=std::thread([&]{
  Temporal::TempoMap::fetch();ProcessThread pt;pt.get_buffers();BufferSet b;prepare(b,block);
  while(!quit.load(std::memory_order_acquire)){
   auto m=mode.load(std::memory_order_acquire);
   if(m==Drain){acknowledged.store(true,std::memory_order_release);std::this_thread::sleep_for(std::chrono::microseconds(50));continue;}
   if(m!=Ahead){std::this_thread::sleep_for(std::chrono::microseconds(50));continue;}
   const auto w=write.load();const auto r=read.load(std::memory_order_acquire);
   if(w>=total_blocks||w-r>=queue_blocks){std::this_thread::sleep_for(std::chrono::microseconds(50));continue;}
   source.run(b,w*block,block);
   for(unsigned c=0;c<2;++c)std::copy_n(b.get_audio(c).data(),block,slots[w%queue_blocks].begin()+c*block);
   write.store(w+1,std::memory_order_release);
  }
  pt.drop_buffers();
 });}
 void stop_worker(){quit.store(true,std::memory_order_release);worker.join();}
 void run(BufferSet& b,samplepos_t,samplepos_t,double speed,pframes_t n,bool) override {
  if(speed!=1||k>=total_blocks){b.silence(n,0);return;}
  const auto begin=Clock::now();
  if(k==0)pthread_getname_np(pthread_self(),callback_thread_name,sizeof(callback_thread_name));
  if(cache_enabled){
   PBD::microseconds_t minimum,maximum;double average,deviation;
   if(AudioEngine::instance()->dsp_stats[AudioEngine::ProcessCallback].get_stats(minimum,maximum,average,deviation)){
    if(maximum>observed_engine_max){observed_engine_max=maximum;observed_engine_max_block=k?k-1:0;}
    if(k>=128&&maximum>steady_engine_max){steady_engine_max=maximum;steady_engine_max_block=k-1;}
   }
  }
  if(n!=block){++bad_size;b.silence(n,0);return;}
  if(cache_enabled&&transitions&&k==arm_block)mode.store(Drain,std::memory_order_release);
  if(mode.load()==Drain&&acknowledged.load(std::memory_order_acquire)&&read.load()==write.load(std::memory_order_acquire)){
   mode.store(Live,std::memory_order_release);cutover=k;
  }
  if(mode.load()==Live)source.run(b,k*block,block);
  else if(write.load(std::memory_order_acquire)>k){
   for(unsigned c=0;c<2;++c)b.get_audio(c).read_from(slots[k%queue_blocks].data()+c*block,block);
   read.store(k+1,std::memory_order_release);
  }else {++misses;b.silence(n,0);read.store(k+1,std::memory_order_release);}
  ++k;
  if(cache_enabled&&transitions&&k==demote_block){
   read.store(k);write.store(k);acknowledged.store(false);
   mode.store(Ahead,std::memory_order_release);
  }
  const double us=std::chrono::duration<double,std::micro>(Clock::now()-begin).count();
  max_us=std::max(max_us,us);if(us>1e6*block/48000.)++late;
  if(k==total_blocks)done.store(true,std::memory_order_release);
 }
};
struct Tap : Processor {
 std::vector<float> captured;unsigned block,k=0;double peak=0;
 Tap(Session& s,unsigned n):Processor(s,"Bus comparison",s),captured(total_blocks*2*n),block(n){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out) override{out=in;return true;}
 void run(BufferSet& b,samplepos_t,samplepos_t,double speed,pframes_t n,bool)override{
  if(speed!=1||k>=total_blocks||n!=block)return;
  for(unsigned c=0;c<2;++c)for(unsigned i=0;i<block;++i){
   captured[(k*2+c)*block+i]=b.get_audio(c).data()[i];
   peak=std::max(peak,double(std::abs(b.get_audio(c).data()[i])));
  }++k;
 }
};
static std::shared_ptr<InternalSend> connect_send(Session& s,std::shared_ptr<Route> from,std::shared_ptr<Route> to){
 auto list=std::make_shared<RouteList>();list->push_back(from);s.add_internal_sends(to,PostFader,list);
 auto send=std::dynamic_pointer_cast<InternalSend>(from->internal_send_for(to));need(bool(send),"send");
 send->gain_control()->set_value(.37,PBD::Controllable::NoGroup);send->activate();return send;
}
#ifndef ANTICIPATION_CALLBACK_HELPERS_ONLY
int main(int argc,char** argv){try{
 need(argc==2,"session path");need(std::string(g_get_home_dir())==getenv("ARDOUR_PROBE_HOME"),"preferences isolation");
 need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();
 std::string backend_name=getenv("ARDOUR_PROBE_BACKEND")?getenv("ARDOUR_PROBE_BACKEND"):"None (Dummy)";
 auto* engine=AudioEngine::create();auto backend=engine->set_backend(backend_name,"AnticipationCallback","");
 need(bool(backend),"backend");
 if(backend_name=="CoreAudio"){
  backend->enumerate_input_devices();backend->enumerate_output_devices();
  need(backend->set_input_device_name(AudioBackend::get_standard_device_name(AudioBackend::DeviceNone))==0,"disable physical input");
  need(backend->set_output_device_name("BlackHole 2ch")==0,"BlackHole output");
 }else need(backend->set_driver("Realtime")==0,"dummy realtime driver");
 need(backend->set_sample_rate(48000)==0,"sample rate");
 double max_error=0,max_us=0,peak=0,engine_mean_us=0,engine_max_us=0;unsigned total_misses=0,total_late=0,total_taps=0;
 unsigned selected_block=getenv("ARDOUR_PROBE_BLOCK")?std::stoul(getenv("ARDOUR_PROBE_BLOCK")):64;
 for(unsigned block:{selected_block}){
  need(backend->set_buffer_size(block)==0,"block size");need(engine->start()==0,"start");
  auto* s=new Session(*engine,std::string(argv[1])+"-"+std::to_string(block),"callback-probe",nullptr,"",true);
  engine->set_session(nullptr);need(engine->session()==nullptr,"detach");
  {
   Fixture source(*s,"Private worker source",true,true,false),reference(*s,"Private reference",true,true,false);
   auto output=s->new_audio_route(2,2,{},1,"Cached output",PresentationInfo::AudioBus,0).front();
   auto ref_output=s->new_audio_route(2,2,{},1,"Direct output",PresentationInfo::AudioBus,0).front();
   Fixture live(*s,"Live callback source"),bus(*s,"Shared callback bus",true,false),bus_ref(*s,"Reference callback bus",true,false);
   auto bridge=std::make_shared<Bridge>(*s,source,block);need(output->add_processor(bridge,PreFader)==0,"bridge");
   auto ref_bridge=std::make_shared<Bridge>(*s,reference,block,false);need(ref_output->add_processor(ref_bridge,PreFader)==0,"reference bridge");
   connect_send(*s,output,bus.route);connect_send(*s,live.route,bus.route);
   connect_send(*s,ref_output,bus_ref.route);connect_send(*s,live.route,bus_ref.route);
   auto tap=std::make_shared<Tap>(*s,block);need(bus.route->add_processor(tap,PostFader)==0,"tap");
   auto ref_tap=std::make_shared<Tap>(*s,block);need(bus_ref.route->add_processor(ref_tap,PostFader)==0,"reference tap");
   s->resort_routes();
   bridge->start_worker();while(bridge->write.load()<queue_blocks)std::this_thread::sleep_for(std::chrono::milliseconds(1));
   engine->dsp_stats[AudioEngine::ProcessCallback].queue_reset();s->request_roll();engine->set_session(s);
   const auto run_start=Clock::now();auto deadline=run_start+std::chrono::seconds(45);
   while(!bridge->done.load(std::memory_order_acquire)&&Clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
   engine->set_session(nullptr);bridge->stop_worker();
   std::cerr<<"CALLBACK wall_seconds="<<std::chrono::duration<double>(Clock::now()-run_start).count()<<" sample_rate="<<backend->sample_rate()<<" buffer="<<backend->buffer_size()<<" backend="<<backend_name<<std::endl;
   double error=0;for(unsigned i=0;i<tap->captured.size();++i)error=std::max(error,double(std::abs(tap->captured[i]-ref_tap->captured[i])));
   std::cerr<<"CALLBACK block="<<block<<" processed="<<bridge->k<<" taps="<<tap->k<<" misses="<<bridge->misses<<" cutover="<<bridge->cutover<<" error="<<error<<" peak="<<tap->peak<<" max_us="<<bridge->max_us<<std::endl;
   std::cerr<<"ENGINE observed_max_us="<<bridge->observed_engine_max<<" at_block="<<bridge->observed_engine_max_block<<std::endl;
   std::cerr<<"ENGINE steady_max_us="<<bridge->steady_engine_max<<" at_block="<<bridge->steady_engine_max_block<<std::endl;
   need(bridge->done&&tap->k==total_blocks&&ref_tap->k==total_blocks&&bridge->bad_size==0,"callback incomplete");
   need(bridge->cutover>=arm_block&&bridge->cutover<=arm_block+queue_blocks+1,"invalid handoff cursor");
   total_misses+=bridge->misses;total_late+=bridge->late;total_taps+=tap->k;
   max_error=std::max(max_error,error);max_us=std::max(max_us,bridge->max_us);peak=std::max(peak,tap->peak);
  }
  delete s;engine->stop();
  PBD::microseconds_t minimum,maximum;double deviation;
  need(engine->dsp_stats[AudioEngine::ProcessCallback].get_stats(minimum,maximum,engine_mean_us,deviation),"callback timing stats");engine_max_us=maximum;
 }
 need(total_misses==0&&max_error<1e-5&&peak>1e-4,"callback output failed");
 std::cout<<"RESULT {\"passed\":true,\"backend\":\""<<backend_name<<"\",\"normal_session_callback\":true,\"blocks\":["<<selected_block<<"],\"verified_bus_blocks\":"<<total_taps<<",\"max_sample_error\":"<<max_error<<",\"cache_underruns\":"<<total_misses<<",\"bridge_deadline_overruns\":"<<total_late<<",\"bridge_max_us\":"<<max_us<<",\"engine_mean_us\":"<<engine_mean_us<<",\"engine_max_us\":"<<engine_max_us<<",\"peak\":"<<peak<<",\"hardware_latency_measured\":false}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
#endif
