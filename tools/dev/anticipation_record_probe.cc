// Actual DiskWriter stays on the callback before the cache boundary. Verify
// captured samples independently from delayed activation of input monitoring.
#define ANTICIPATION_CALLBACK_HELPERS_ONLY
#include "anticipation_callback_probe.cc"
#include "ardour/audiosource.h"
struct RawInputTap : Tap {
 std::shared_ptr<IO> input;BufferSet raw;
 RawInputTap(Session& s,unsigned n,std::shared_ptr<IO> io):Tap(s,n),input(io){prepare(raw,n);}
 void run(BufferSet&,samplepos_t start,samplepos_t end,double speed,pframes_t n,bool more)override{
  input->collect_input(raw,n,ChanCount());Tap::run(raw,start,end,speed,n,more);
 }
};

int main(int argc,char** argv){try{
 need(argc==2,"session path");need(std::string(g_get_home_dir())==getenv("ARDOUR_PROBE_HOME"),"preferences isolation");
 need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","RecordProbe","");
 const unsigned block=getenv("ARDOUR_PROBE_BLOCK")?std::stoul(getenv("ARDOUR_PROBE_BLOCK")):64;
 need(bool(backend)&&backend->set_driver("Realtime")==0&&backend->set_sample_rate(48000)==0&&backend->set_buffer_size(block)==0&&engine->start()==0,"engine");
 auto* s=new FixtureSession(*engine,argv[1],"record-probe",nullptr,"",true);engine->set_session(nullptr);
 double error=0,record_error=0;unsigned misses=0,late=0,recorded_channels=0,cutover=0;
 {
  Fixture source(*s,"Future playback DSP",true,true,false),reference(*s,"Direct reference DSP",true,true,false);
  auto playlist=make_disk_playlist(*s,std::string(argv[1])+"/source.wav");
  auto disk=use_disk_input(*s,source,playlist),reference_disk=use_disk_input(*s,reference,playlist);
  auto record_track=s->new_audio_track(2,2,{},1,"Actual recording track",0,Normal,false).front();
  Fixture reference_output(*s,"Reference output",false,false),live(*s,"Current input"),bus(*s,"Actual mix",true,false),reference_bus(*s,"Reference mix",true,false);
  auto bridge=std::make_shared<Bridge>(*s,source,block),reference_bridge=std::make_shared<Bridge>(*s,reference,block,false);
  auto range=std::make_shared<MonitorRange>();range->until=demote_block*block;
  disk->monitor_range=reference_disk->monitor_range=range;bridge->monitor_range=reference_bridge->monitor_range=range;
  bridge->disk_input=disk;reference_bridge->disk_input=reference_disk;
  need(record_track->add_processor(bridge,PreFader)==0&&reference_output.route->add_processor(reference_bridge,PreFader)==0,"bridges after callback disk I/O");
  auto record_input=std::make_shared<RawInputTap>(*s,block,record_track->input());
  need(record_track->add_processor(record_input,bridge)==0,"capture input oracle");
  {PBD::Mutex::Lock lock(engine->process_lock());
   for(unsigned c=0;c<2;++c)for(auto destination:{std::static_pointer_cast<Route>(record_track),reference_output.route})
    need(destination->input()->connect(destination->input()->nth(c),live.route->output()->nth(c)->name())==0,"current input route");
  }
  connect_send(*s,record_track,bus.route);connect_send(*s,reference_output.route,reference_bus.route);
  auto tap=std::make_shared<Tap>(*s,block),reference_tap=std::make_shared<Tap>(*s,block);
  need(bus.route->add_processor(tap,PostFader)==0&&reference_bus.route->add_processor(reference_tap,PostFader)==0,"mix capture");s->resort_routes();
  record_track->monitoring_control()->set_value(MonitorInput,PBD::Controllable::NoGroup);
  record_track->rec_enable_control()->set_value(1,PBD::Controllable::NoGroup);
  engine->set_session(s);const auto setup_deadline=Clock::now()+std::chrono::seconds(5);bool ready=false;
  while(!ready&&Clock::now()<setup_deadline){
   {PBD::Mutex::Lock lock(engine->process_lock());ready=s->setup_settled()&&!s->global_locate_pending()&&record_track->rec_enable_control()->get_value()==1&&record_track->monitoring_control()->get_value()==MonitorInput;}
   if(!ready)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  engine->set_session(nullptr);need(ready,"record controls did not settle");
  bridge->start_worker();while(bridge->write.load()<bridge->capacity)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  s->maybe_enable_record();need(s->get_record_enabled(),"global record enable");s->request_roll();engine->set_session(s);
  const auto deadline=Clock::now()+std::chrono::seconds(30);
  while(!bridge->done.load()&&Clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  need(bridge->done,"record callback incomplete");s->request_stop();
  const auto stop_deadline=Clock::now()+std::chrono::seconds(5);bool stopped=false;
  while(!stopped&&Clock::now()<stop_deadline){
   {PBD::Mutex::Lock lock(engine->process_lock());stopped=s->transport_stopped()&&s->setup_settled()&&!s->global_locate_pending();}
   if(!stopped)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  engine->set_session(nullptr);bridge->stop_worker();need(stopped,"recording did not finalize");
  need(tap->k==total_blocks&&reference_tap->k==total_blocks&&record_input->k==total_blocks,"capture length");
  for(unsigned i=0;i<tap->captured.size();++i)error=std::max(error,double(std::abs(tap->captured[i]-reference_tap->captured[i])));
  std::vector<float> samples(total_blocks*block);
  for(auto& captured:record_track->last_capture_sources()){
   auto audio=std::dynamic_pointer_cast<AudioSource>(captured);need(bool(audio)&&recorded_channels<2,"record source type");
   need(audio->length().samples()>=samplecnt_t(samples.size())&&audio->read(samples.data(),0,samples.size())==samplecnt_t(samples.size()),"recorded file too short");
   for(unsigned k=0;k<total_blocks;++k)for(unsigned i=0;i<block;++i)record_error=std::max(record_error,double(std::abs(samples[k*block+i]-record_input->captured[(k*2+recorded_channels)*block+i])));
   ++recorded_channels;
  }
  misses=bridge->misses;late=bridge->late;cutover=bridge->cutover;
  std::cerr<<"RECORD channels="<<recorded_channels<<" compared_frames="<<samples.size()<<" file_error="<<record_error<<" mix_error="<<error<<" monitor_cutover="<<cutover<<" monitored_blocks="<<disk->monitor_blocks<<" input_peak="<<record_input->peak<<std::endl;
  need(recorded_channels==2&&record_error<1e-6&&record_input->peak>1e-4&&error<1e-7&&misses==0&&disk->monitor_blocks>0,"record or monitoring mismatch");
 }
 delete s;engine->stop();PBD::microseconds_t minimum,maximum;double average,deviation;
 need(engine->dsp_stats[AudioEngine::ProcessCallback].get_stats(minimum,maximum,average,deviation),"timing");
 std::cout<<"RESULT {\"passed\":true,\"normal_session_callback\":true,\"blocks\":["<<block<<"],\"actual_recorded_channels\":"<<recorded_channels<<",\"recorded_sample_error\":"<<record_error<<",\"monitor_cutover\":"<<cutover<<",\"max_sample_error\":"<<error<<",\"cache_underruns\":"<<misses<<",\"bridge_deadline_overruns\":"<<late<<",\"engine_mean_us\":"<<average<<",\"engine_max_us\":"<<maximum<<",\"hardware_latency_measured\":false}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
