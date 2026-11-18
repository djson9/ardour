#define ANTICIPATION_NATIVE_HELPERS_ONLY
#include "anticipation_native_route_probe.cc"
#include "ardour/audiosource.h"
#include "ardour/disk_writer.h"

struct NativeRawTap : NativeTap {
 std::shared_ptr<IO> input;BufferSet raw;
 NativeRawTap(Session& s,unsigned n,std::shared_ptr<IO> io):NativeTap(s,n),input(io){prepare(raw,n);}
 void run(BufferSet&,samplepos_t start,samplepos_t end,double speed,pframes_t n,bool more)override{input->collect_input(raw,n,ChanCount());NativeTap::run(raw,start,end,speed,n,more);}
};

int main(int argc,char** argv){try{
 need(argc==2&&ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","NativeRecord","");
 const unsigned block=getenv("ARDOUR_PROBE_BLOCK")?std::stoul(getenv("ARDOUR_PROBE_BLOCK")):64;
 need(backend&&backend->set_driver("Realtime")==0&&backend->set_sample_rate(48000)==0&&backend->set_buffer_size(block)==0&&engine->start()==0,"engine");
 auto* s=new NativeSession(*engine,argv[1],"native-record",nullptr,"",true);engine->set_session(nullptr);
 double error=0,peak=0;unsigned channels=0;uint64_t misses=0;
 {
  auto input=s->new_audio_route(2,2,{},1,"Current input",PresentationInfo::AudioBus,0).front();need(input->add_processor(std::make_shared<NativeInput>(*s),PreFader)==0,"input source");input->set_anticipative_processing_disabled(true);
  auto record=s->new_audio_track(2,2,{},1,"Recorded native track",0,Normal,false).front();add_effects(*s,record,false);
  auto playback=s->new_audio_track(2,2,{},1,"Independent playback",0,Normal,false).front();add_effects(*s,playback,false);
  auto playlist=make_disk_playlist(*s,std::string(argv[1])+"/playback.wav");need(playback->use_playlist(DataType::AUDIO,playlist)==0,"playback playlist");playback->monitoring_control()->set_value(MonitorDisk,PBD::Controllable::NoGroup);
  std::shared_ptr<Processor> writer;record->foreach_processor([&](std::weak_ptr<Processor> p){if(std::dynamic_pointer_cast<DiskWriter>(p.lock()))writer=p.lock();});need(bool(writer),"original DiskWriter");
  auto raw=std::make_shared<NativeRawTap>(*s,block,record->input());need(record->add_processor(raw,PreFader)==0,"independent raw input oracle");
  {PBD::Mutex::Lock lock(engine->process_lock());for(unsigned c=0;c<2;++c)need(record->input()->connect(record->input()->nth(c),input->output()->nth(c)->name())==0,"live input");}
  record->monitoring_control()->set_value(MonitorInput,PBD::Controllable::NoGroup);record->rec_enable_control()->set_value(1,PBD::Controllable::NoGroup);s->resort_routes();s->calculate_latency();
  auto wait=[&](auto predicate,const char* message){auto end=Clock::now()+std::chrono::seconds(15);while(Clock::now()<end){{PBD::Mutex::Lock lock(engine->process_lock());if(predicate())return;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}throw std::runtime_error(message);};
  engine->set_session(s);wait([&]{return s->setup_settled()&&!s->global_locate_pending()&&record->rec_enable_control()->get_value()!=0;},"setup");
  s->set_anticipative_processing(!getenv("ARDOUR_PROBE_DIRECT"),200);s->maybe_enable_record();need(s->get_record_enabled(),"record enable");s->request_roll();wait([&]{return raw->k==total_blocks;},"record complete");
  if(!getenv("ARDOUR_PROBE_DIRECT")){PBD::Mutex::Lock lock(engine->process_lock());auto& plan=s->anticipative_process();auto* r=plan.find_node(record.get());auto* p=plan.find_node(playback.get());need(r&&p&&r->queue->state()==AnticipationQueue::State::Live&&p->queue->state()==AnticipationQueue::State::Ahead,"simultaneous live capture and future playback");misses=r->queue->underruns()+p->queue->underruns();}
  s->request_stop(false,false);wait([&]{return s->transport_stopped()&&s->setup_settled()&&!s->global_locate_pending()&&s->anticipative_process().size()==0;},"record finalization");engine->set_session(nullptr);
  std::vector<float> samples(total_blocks*block);
  for(auto source:record->last_capture_sources()){
   auto audio=std::dynamic_pointer_cast<AudioSource>(source);need(audio&&channels<2&&audio->length().samples()>=samplecnt_t(samples.size())&&audio->read(samples.data(),0,samples.size())==samplecnt_t(samples.size()),"record source");
   for(unsigned i=0;i<samples.size();++i)error=std::max(error,double(std::abs(samples[i]-raw->captured[2*i+channels])));++channels;
  }
  peak=raw->peak;std::cerr<<"NATIVE RECORD channels="<<channels<<" file_error="<<error<<" peak="<<peak<<" misses="<<misses<<std::endl;
  need(channels==2&&error<1e-6&&peak>1e-4&&misses==0,"native record samples");
 }
 delete s;engine->stop();std::cout<<"RESULT {\"passed\":true,\"original_disk_writer\":true,\"live_capture_with_anticipated_playback\":"<<(getenv("ARDOUR_PROBE_DIRECT")?"false":"true")<<",\"recorded_sample_error\":"<<error<<",\"recorded_channels\":"<<channels<<",\"cache_underruns\":"<<misses<<"}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(std::exception const& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
