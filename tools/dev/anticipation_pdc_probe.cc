// Compare a cached processing slice with a normal Route under automatic PDC.
#define ANTICIPATION_CALLBACK_HELPERS_ONLY
#include "anticipation_callback_probe.cc"
#include "pbd/debug.h"
#include <execinfo.h>
#include "ardour/butler.h"
struct SessionAccess : Session {
 using Session::Session;
 static auto update_latency(){return &SessionAccess::update_latency_compensation;}
 bool setup_settled(){return pending_events.read_space()==0&&immediate_events.empty()&&!butler()->transport_work_requested();}
 void stop_transport(bool abort=false,bool clear=false)override{
  if(getenv("ARDOUR_PROBE_DEBUG")){std::cerr<<"STOP REQUEST"<<std::endl;void* frames[30];int count=backtrace(frames,30);backtrace_symbols_fd(frames,count,2);}
  Session::stop_transport(abort,clear);
 }
};
struct TimelineInput : Processor {
 std::vector<samplepos_t> starts;
 std::vector<std::shared_ptr<PluginInsert>> reset_inserts;
 TimelineInput(Session& s):Processor(s,"Timeline input",s){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void run(BufferSet& b,samplepos_t start,samplepos_t,double speed,pframes_t n,bool)override{
  // Give the two independent oracle instances the same initial DSP state.
  // A normal route can receive more stopped callbacks before request_roll is
  // consumed; those calls otherwise advance plugin smoothing on just one side.
  // PluginInsert::flush only sets its existing atomic pending-flush flag.
  if(speed==1&&starts.empty())for(auto& insert:reset_inserts)insert->flush();
  if(speed==1&&starts.size()<8)starts.push_back(start);
  for(unsigned c=0;c<2;++c)for(unsigned i=0;i<n;++i)b.get_audio(c).data()[i]=speed==1?.1*std::sin((start+i)*.017):0;
 }
};
struct SampleTap : Processor {
 std::vector<float> captured;unsigned cursor=0,limit,calls=0;double peak=0;
 SampleTap(Session& s,unsigned frames):Processor(s,"Variable block capture",s),captured(2*frames),limit(frames){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void run(BufferSet& b,samplepos_t,samplepos_t,double speed,pframes_t n,bool)override{
  if(speed!=1||cursor>=limit)return;
  need(cursor+n<=limit,"unexpected partial capture end");const BufferSet& input=b;
  for(unsigned i=0;i<n;++i)for(unsigned c=0;c<2;++c){auto value=input.get_audio(c).data()[i];captured[2*(cursor+i)+c]=value;peak=std::max(peak,double(std::abs(value)));}
  cursor+=n;++calls;
 }
};

struct ReportedDelay : Processor {
 unsigned delay,cursor=0;std::vector<float> history;
 std::vector<float> trace;
 ReportedDelay(Session& s,unsigned n):Processor(s,"Declared sample delay",s),delay(n),history(2*n,0){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 samplecnt_t signal_latency()const override{return delay;}
 void run(BufferSet& b,samplepos_t start,samplepos_t,double speed,pframes_t n,bool)override{
  for(unsigned i=0;i<n;++i){for(unsigned c=0;c<2;++c){auto& memory=history[c*delay+cursor];auto& sample=b.get_audio(c).data()[i];std::swap(memory,sample);}cursor=(cursor+1)%delay;}
  if(auto* time=ProcessThread::render_time())need(time->continuous_at(start)==start,"latent processor continuous clock offset");
  // Deliberately depend on the host-provided output timestamp as well as the
  // audio delay. An ordinary delay alone cannot detect the wrong time context.
  if(getenv("ARDOUR_PROBE_HOST_TIME"))for(unsigned i=0;i<n;++i)for(unsigned c=0;c<2;++c)b.get_audio(c).data()[i]*=.75+.25*std::sin((start+i)*.003);
  if(speed==1)trace.insert(trace.end(),b.get_audio(0).data(),b.get_audio(0).data()+n);
 }
};
int main(int argc,char** argv){try{
 need(argc==2,"session path");need(std::string(g_get_home_dir())==getenv("ARDOUR_PROBE_HOME"),"preferences isolation");
 need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();
 if(getenv("ARDOUR_PROBE_DEBUG"))PBD::parse_debug_options("transport,tfsm");
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","PDCProbe","");
 const unsigned block=getenv("ARDOUR_PROBE_BLOCK")?std::stoul(getenv("ARDOUR_PROBE_BLOCK")):64;
 need(bool(backend)&&backend->set_driver("Realtime")==0&&backend->set_sample_rate(48000)==0&&backend->set_buffer_size(block)==0&&engine->start()==0,"engine");
 auto* s=new SessionAccess(*engine,argv[1],"pdc-probe",nullptr,"",true);engine->set_session(nullptr);
 PBD::ScopedConnection debug_connection;
 if(getenv("ARDOUR_PROBE_DEBUG"))s->TransportStateChange.connect_same_thread(debug_connection,[&]{std::cerr<<"TRANSPORT speed="<<s->transport_speed(true)<<" sample="<<s->transport_sample()<<std::endl;void* frames[30];int count=backtrace(frames,30);backtrace_symbols_fd(frames,count,2);});
 double error=0,peak=0;unsigned misses=0,late=0;samplepos_t origin=0;samplecnt_t preroll=0,route_latency=0,live_delay=0;
 {
  constexpr unsigned latency=257;
  std::vector<std::shared_ptr<ReportedDelay>> delays;
  std::vector<std::shared_ptr<TimelineInput>> inputs;
  Fixture source(*s,"Private cached DSP",true,true,false),reference(*s,"Normal reference route",true,true,true);
  for(auto* f:{&source,&reference}){auto input=std::make_shared<TimelineInput>(*s);input->starts.reserve(8);inputs.push_back(input);need(f->route->replace_processor(f->probe,input)==0,"timeline source");auto delay=std::make_shared<ReportedDelay>(*s,latency);delay->trace.reserve(300000);delays.push_back(delay);need(f->route->add_processor(delay,PreFader)==0,"latent processor");f->endpoint=delay;}
  source.route->foreach_processor([&](std::weak_ptr<Processor> p){auto insert=std::dynamic_pointer_cast<PluginInsert>(p.lock());if(insert)inputs[0]->reset_inserts.push_back(insert);});
  reference.route->foreach_processor([&](std::weak_ptr<Processor> p){auto insert=std::dynamic_pointer_cast<PluginInsert>(p.lock());if(insert)inputs[1]->reset_inserts.push_back(insert);});
  auto output=s->new_audio_route(2,2,{},1,"Cached route",PresentationInfo::AudioBus,0).front();
  auto bridge=std::make_shared<Bridge>(*s,source,block);bridge->reported_latency=latency;bridge->check_position=true;
  need(output->add_processor(bridge,PreFader)==0,"cached DSP slice");
  Fixture live(*s,"Live source"),bus(*s,"Cached mix",true,false),bus_reference(*s,"Reference mix",true,false);
  connect_send(*s,output,bus.route);connect_send(*s,reference.route,bus_reference.route);
  auto live_send=connect_send(*s,live.route,bus.route);connect_send(*s,live.route,bus_reference.route);
  s->resort_routes();(s->*SessionAccess::update_latency())(true,false);
  preroll=s->worst_latency_preroll_buffer_size_ceil();route_latency=output->signal_latency();
  bridge->render_origin=origin=-preroll+output->playback_latency();live_delay=live_send->get_delay_in()-live_send->get_delay_out();
  std::cerr<<"PDC setup preroll="<<preroll<<" route_latency="<<route_latency<<" reference_latency="<<reference.route->signal_latency()<<" render_origin="<<origin<<" live_send_delay="<<live_delay<<std::endl;
  need(route_latency==latency&&reference.route->signal_latency()==latency&&preroll>=latency,"automatic latency calculation failed");
  // Build an immutable chunk plan using the same existing route latency
  // boundaries that Session::calc_preroll_subcycle uses in its callback.
  unsigned elapsed=0,cycle_left=block;samplecnt_t remaining=preroll;
  for(unsigned k=0;k<total_blocks;++k){
   samplecnt_t ns=remaining>0?std::min(samplecnt_t(cycle_left),remaining):cycle_left;
   if(remaining>0)for(auto& route:*s->get_routes())if(route->active()){
    const auto offset=route->playback_latency();
    if(remaining>offset&&remaining<=offset+ns)ns=std::min(ns,remaining-offset);
   }
   need(ns>0,"empty preroll chunk");bridge->render_plan.push_back({origin+elapsed,pframes_t(ns)});
   elapsed+=ns;cycle_left-=ns;if(!cycle_left)cycle_left=block;
   remaining=std::max(samplecnt_t(0),remaining-ns);
  }
  std::cerr<<"PDC chunks="<<bridge->render_plan[0].frames<<","<<bridge->render_plan[1].frames<<","<<bridge->render_plan[2].frames<<" frames="<<elapsed<<std::endl;
  auto tap=std::make_shared<SampleTap>(*s,elapsed),reference_tap=std::make_shared<SampleTap>(*s,elapsed);
  need(bus.route->add_processor(tap,PostFader)==0&&bus_reference.route->add_processor(reference_tap,PostFader)==0,"capture");s->resort_routes();
  std::atomic<bool> located{false};PBD::ScopedConnection locate_connection;
  s->Located.connect_same_thread(locate_connection,[&]{located=true;});s->request_locate(0,true,MustStop);engine->set_session(s);
  bool ready=false;for(unsigned wait=0;wait<5000;++wait){{PBD::Mutex::Lock lock(engine->process_lock());ready=located&&s->setup_settled()&&!s->locate_pending()&&!s->global_locate_pending();}if(ready)break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  engine->set_session(nullptr);locate_connection.disconnect();need(ready,"initial locate incomplete");
  source.route->flush_processors();reference.route->flush_processors();
  bridge->start_worker();while(bridge->write.load()<bridge->capacity)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  s->request_locate(0,true,MustRoll);engine->set_session(s);const auto deadline=Clock::now()+std::chrono::seconds(12);
  while(!bridge->done.load(std::memory_order_acquire)&&Clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(2));
  engine->set_session(nullptr);bridge->stop_worker();
  std::cerr<<"PDC completed="<<bridge->k<<" published="<<bridge->write.load()<<" bad_size="<<bridge->bad_size<<" transport="<<s->transport_sample()<<" rolling="<<s->transport_rolling()<<" end="<<s->current_end_sample()<<" stop_at_end="<<Config->get_stop_at_session_end()<<std::endl;
  need(bridge->done&&bridge->bad_size==0&&tap->cursor==elapsed&&reference_tap->cursor==elapsed,"PDC callback incomplete");
  unsigned first=0;for(unsigned i=0;i<tap->captured.size();++i){auto difference=double(std::abs(tap->captured[i]-reference_tap->captured[i]));if(difference>1e-6&&!first)first=i/2;error=std::max(error,difference);}
  std::cerr<<"PDC first_difference_frame="<<first<<" cache="<<tap->captured[2*first]<<" reference="<<reference_tap->captured[2*first]<<std::endl;
  double slice_error=0;for(unsigned i=0;i<std::min(delays[0]->trace.size(),delays[1]->trace.size());++i)slice_error=std::max(slice_error,double(std::abs(delays[0]->trace[i]-delays[1]->trace[i])));
  std::cerr<<"PDC slice_error="<<slice_error<<" lengths="<<delays[0]->trace.size()<<","<<delays[1]->trace.size()<<std::endl;
  for(auto& input:inputs){std::cerr<<"PDC input_starts=";for(auto value:input->starts)std::cerr<<value<<",";std::cerr<<std::endl;}
  peak=tap->peak;misses=bridge->misses;late=bridge->late;
  std::cerr<<"PDC output error="<<error<<" peak="<<peak<<" cache_misses="<<misses<<" cutover="<<bridge->cutover<<std::endl;
  std::cerr<<"ENGINE observed_max_us="<<bridge->observed_engine_max<<std::endl;
 }
 delete s;engine->stop();PBD::microseconds_t minimum,maximum;double average,deviation;
 need(engine->dsp_stats[AudioEngine::ProcessCallback].get_stats(minimum,maximum,average,deviation),"timing");
 need(error<1e-5&&misses==0&&peak>1e-4,"PDC output mismatch");
 std::cout<<"RESULT {\"passed\":true,\"normal_session_callback\":true,\"normal_route_reference\":true,\"blocks\":["<<block<<"],\"reported_latency\":"<<route_latency<<",\"preroll\":"<<preroll<<",\"render_origin\":"<<origin<<",\"max_sample_error\":"<<error<<",\"cache_underruns\":"<<misses<<",\"bridge_deadline_overruns\":"<<late<<",\"engine_mean_us\":"<<average<<",\"engine_max_us\":"<<maximum<<",\"peak\":"<<peak<<",\"hardware_latency_measured\":false}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
