// Exercise conservative generation replacement using the existing stopped
// transport/locate barrier. This measures timeline correctness, not seek gap.
#define ANTICIPATION_CALLBACK_HELPERS_ONLY
#include "anticipation_callback_probe.cc"
#include "ardour/butler.h"
struct EpochSession : Session {
 using Session::Session;
 bool setup_settled(){return pending_events.read_space()==0&&immediate_events.empty()&&!butler()->transport_work_requested();}
};
struct EpochBridge : Bridge {
 std::atomic<bool>& enabled;
 EpochBridge(Session& s,Fixture& f,unsigned n,bool cached,std::atomic<bool>& e):Bridge(s,f,n,cached,false),enabled(e){}
 void run(BufferSet& b,samplepos_t start,samplepos_t end,double speed,pframes_t n,bool more)override{
  if(!enabled.load(std::memory_order_acquire)){b.silence(n,0);return;}
  Bridge::run(b,start,end,speed,n,more);
 }
 void reset(samplepos_t origin){
  render_origin=origin;k=0;read=0;write=0;quit=false;done=false;acknowledged=false;
  completed_blocks=0;mode=cache_enabled?Ahead:Live;misses=late=0;
 }
};
struct EpochTap : Tap {
 std::atomic<bool>& enabled;
 EpochTap(Session& s,unsigned n,std::atomic<bool>& e):Tap(s,n),enabled(e){}
 void run(BufferSet& b,samplepos_t start,samplepos_t end,double speed,pframes_t n,bool more)override{
  if(enabled.load(std::memory_order_acquire))Tap::run(b,start,end,speed,n,more);
 }
};
int main(int argc,char** argv){try{
 need(argc==2,"session path");need(std::string(g_get_home_dir())==getenv("ARDOUR_PROBE_HOME"),"preferences isolation");
 need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","SeekProbe","");
 const unsigned block=getenv("ARDOUR_PROBE_BLOCK")?std::stoul(getenv("ARDOUR_PROBE_BLOCK")):64;
 need(bool(backend)&&backend->set_driver("Realtime")==0&&backend->set_sample_rate(48000)==0&&backend->set_buffer_size(block)==0&&engine->start()==0,"engine");
 auto* s=new EpochSession(*engine,argv[1],"seek-probe",nullptr,"",true);engine->set_session(nullptr);
 double error=0;unsigned discarded=0,checks=0,misses=0,late=0;
 {
  Fixture source(*s,"Future DSP",true,true,false),reference(*s,"Reference DSP",true,true,false);
  auto playlist=make_disk_playlist(*s,std::string(argv[1])+"/source.wav");use_disk_input(*s,source,playlist);use_disk_input(*s,reference,playlist);
  Fixture output(*s,"Future output",false,false),reference_output(*s,"Reference output",false,false),bus(*s,"Future bus",true,false),reference_bus(*s,"Reference bus",true,false);
  std::atomic<bool> enabled{false};
  auto bridge=std::make_shared<EpochBridge>(*s,source,block,true,enabled),reference_bridge=std::make_shared<EpochBridge>(*s,reference,block,false,enabled);
  need(output.route->add_processor(bridge,PreFader)==0&&reference_output.route->add_processor(reference_bridge,PreFader)==0,"bridges");
  connect_send(*s,output.route,bus.route);connect_send(*s,reference_output.route,reference_bus.route);
  auto tap=std::make_shared<EpochTap>(*s,block,enabled),reference_tap=std::make_shared<EpochTap>(*s,block,enabled);
  need(bus.route->add_processor(tap,PostFader)==0&&reference_bus.route->add_processor(reference_tap,PostFader)==0,"taps");s->resort_routes();
  auto settle=[&](samplepos_t target){
   std::atomic<bool> located{false};PBD::ScopedConnection connection;
   s->Located.connect_same_thread(connection,[&]{located=true;});s->request_locate(target,true,MustStop);
   engine->set_session(s);const auto deadline=Clock::now()+std::chrono::seconds(5);bool ready=false;
   while(!ready&&Clock::now()<deadline){
    {PBD::Mutex::Lock lock(engine->process_lock());ready=located&&s->setup_settled()&&s->transport_stopped()&&!s->locate_pending()&&!s->global_locate_pending()&&s->transport_sample()==target;}
    if(!ready)std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
   engine->set_session(nullptr);need(ready,"locate barrier did not finish");
  };
  for(auto target:{samplepos_t(0),samplepos_t(240000),samplepos_t(48000)}){
   settle(target);
   // Replacement and plugin mutation happen only after both owners are idle.
   if(checks==2)for(auto* f:{&source,&reference}){
    auto plugin=find_plugin(*s,"urn:ardour:a-delay",LV2);need(bool(plugin),"mutation plugin");
    auto insert=std::make_shared<PluginInsert>(*s,*f->route,plugin);need(f->route->add_processor(insert,PreFader)==0,"mutation insert");insert->activate();f->endpoint=insert;
   }
   for(auto* f:{&source,&reference}){f->route->non_realtime_locate(target);f->route->flush_processors();}
   for(auto& route:*s->get_routes())route->flush_processors();bridge->reset(target);reference_bridge->reset(target);tap->k=reference_tap->k=0;
   bridge->start_worker();while(bridge->write.load()<bridge->capacity)std::this_thread::sleep_for(std::chrono::milliseconds(1));
   enabled.store(true,std::memory_order_release);s->request_roll();engine->set_session(s);
   const auto deadline=Clock::now()+std::chrono::seconds(15);
   while(bridge->completed_blocks.load()<512&&Clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
   {PBD::Mutex::Lock lock(engine->process_lock());enabled.store(false,std::memory_order_release);}
   s->request_stop();bridge->stop_worker();
   const auto stop_deadline=Clock::now()+std::chrono::seconds(5);bool stopped=false;
   while(!stopped&&Clock::now()<stop_deadline){
    {PBD::Mutex::Lock lock(engine->process_lock());stopped=s->transport_stopped()&&!s->global_locate_pending();}
    if(!stopped)std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
   engine->set_session(nullptr);std::cerr<<"SEEK stopped="<<stopped<<" rolling="<<s->transport_rolling()<<" pending="<<s->global_locate_pending()<<" processed="<<bridge->k<<" taps="<<tap->k<<","<<reference_tap->k<<std::endl;need(stopped&&bridge->k>=512&&tap->k==reference_tap->k,"epoch incomplete");
   double epoch_error=0;for(unsigned i=0;i<tap->k*2*block;++i)epoch_error=std::max(epoch_error,double(std::abs(tap->captured[i]-reference_tap->captured[i])));
   const auto abandoned=bridge->write.load()-bridge->read.load();discarded+=abandoned;error=std::max(error,epoch_error);misses+=bridge->misses;late+=bridge->late;++checks;
   std::cerr<<"SEEK generation="<<checks<<" target="<<target<<" consumed="<<tap->k<<" discarded_future_blocks="<<abandoned<<" error="<<epoch_error<<std::endl;
   need(epoch_error<1e-7&&abandoned>0&&tap->peak>1e-4,"generation leaked stale audio");
  }
 }
 delete s;engine->stop();PBD::microseconds_t minimum,maximum;double average,deviation;
 need(engine->dsp_stats[AudioEngine::ProcessCallback].get_stats(minimum,maximum,average,deviation),"timing");
 need(checks==3&&misses==0,"seek matrix incomplete");
 std::cout<<"RESULT {\"passed\":true,\"normal_session_callback\":true,\"blocks\":["<<block<<"],\"generation_cases\":"<<checks<<",\"discarded_future_blocks\":"<<discarded<<",\"plugin_mutation_checked\":true,\"max_sample_error\":"<<error<<",\"cache_underruns\":"<<misses<<",\"bridge_deadline_overruns\":"<<late<<",\"engine_mean_us\":"<<average<<",\"engine_max_us\":"<<maximum<<",\"seek_gap_measured\":false,\"hardware_latency_measured\":false}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
