#define ANTICIPATION_CALLBACK_HELPERS_ONLY
#include "anticipation_callback_probe.cc"
#include "ardour/anticipative_process.h"
#include "ardour/location.h"

struct NativeSession : FixtureSession {
 using FixtureSession::FixtureSession;
 void calculate_latency(){update_latency_compensation(true,false);}
};
struct NativeDelay : Processor {
 unsigned cursor=0;std::vector<float> history=std::vector<float>(514,0);
 NativeDelay(Session& s):Processor(s,"Native timestamp-sensitive delay",s){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 samplecnt_t signal_latency()const override{return 257;}
 void run(BufferSet& b,samplepos_t start,samplepos_t,double,pframes_t n,bool)override{
  for(unsigned i=0;i<n;++i){for(unsigned c=0;c<2;++c){auto& sample=b.get_audio(c).data()[i];std::swap(history[c*257+cursor],sample);sample*=.75+.25*std::sin((start+i)*.003);}cursor=(cursor+1)%257;}
 }
};

struct NativeTap : Processor {
 std::vector<float> captured;unsigned frames=0,block,k=0;double peak=0;
 NativeTap(Session& s,unsigned n):Processor(s,"Native output capture",s),captured(2*total_blocks*n),block(n){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void run(BufferSet& b,samplepos_t,samplepos_t,double speed,pframes_t n,bool)override{
  if(speed!=1||frames>=total_blocks*block)return;
  n=std::min(n,total_blocks*block-frames);
  for(unsigned i=0;i<n;++i)for(unsigned c=0;c<2;++c){float v=b.get_audio(c).data()[i];captured[2*(frames+i)+c]=v;peak=std::max(peak,double(std::abs(v)));}
  frames+=n;k=frames/block;
 }
};

struct NativeInput : Probe {
 NativeInput(Session& s):Probe(s){}
 void run(BufferSet& b,samplepos_t start,samplepos_t end,double speed,pframes_t n,bool more)override{
  if(speed==0){b.silence(n,0);return;}Probe::run(b,start,end,speed,n,more);
 }
};
static void add_effects(Session& s,std::shared_ptr<Route> route,bool generator){
 if(generator)need(route->add_processor(std::make_shared<NativeInput>(s),PreFader)==0,"native generator");
 for(unsigned k=0;k<4;++k){auto plugin=find_plugin(s,"urn:ardour:a-delay",LV2);need(bool(plugin),"delay");auto insert=std::make_shared<PluginInsert>(s,*route,plugin);need(route->add_processor(insert,PreFader)==0,"native insert");insert->activate();
  for(auto value:std::vector<std::pair<unsigned,double>>{{5,13.0+7*k},{7,70},{8,55}})insert->automation_control(Evoral::Parameter(PluginAutomation,0,value.first))->set_value(value.second,PBD::Controllable::NoGroup);
 }
}
#ifndef ANTICIPATION_NATIVE_HELPERS_ONLY
int main(int argc,char** argv){try{
 need(argc==2,"path");need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","NativeAnticipation","");
 unsigned block=getenv("ARDOUR_PROBE_BLOCK")?std::stoul(getenv("ARDOUR_PROBE_BLOCK")):64;
 need(bool(backend)&&backend->set_driver("Realtime")==0&&backend->set_sample_rate(48000)==0&&backend->set_buffer_size(block)==0&&engine->start()==0,"engine");
 auto* s=new NativeSession(*engine,argv[1],"native-anticipation",nullptr,"",true);engine->set_session(nullptr);
 if(getenv("ARDOUR_PROBE_DISK_READER")){Config->set_buffering_preset(Custom);Config->set_audio_playback_buffer_seconds(.5);DiskReader::set_chunk_samples(8192);}
 double error=0,peak=0;uint64_t handoffs=0,misses=0;
 {
  auto a=s->new_audio_track(2,2,{},1,"Native A",0,Normal,false).front();
  auto ra=s->new_audio_track(2,2,{},1,"Reference A",0,Normal,false).front();
  auto b=s->new_audio_route(2,2,{},1,"Native B",PresentationInfo::AudioBus,0).front();
  auto rb=s->new_audio_route(2,2,{},1,"Reference B",PresentationInfo::AudioBus,0).front();
  add_effects(*s,a,!getenv("ARDOUR_PROBE_DISK_READER"));add_effects(*s,ra,!getenv("ARDOUR_PROBE_DISK_READER"));add_effects(*s,b,false);add_effects(*s,rb,false);
  if(getenv("ARDOUR_PROBE_PDC"))for(auto route:{a,ra})need(route->add_processor(std::make_shared<NativeDelay>(*s),PreFader)==0,"native latency");
  if(getenv("ARDOUR_PROBE_DISK_READER")){auto playlist=make_disk_playlist(*s,std::string(argv[1])+"/native-source.wav");need(a->use_playlist(DataType::AUDIO,playlist)==0&&ra->use_playlist(DataType::AUDIO,playlist)==0,"native playlists");need(s->butler()->audio_playback_buffer_size()==24000,"half-second real Butler buffer");}
  if(getenv("ARDOUR_PROBE_SIDECHAIN"))for(auto pair:std::vector<std::pair<std::shared_ptr<Route>,std::shared_ptr<Route>>>{{a,b},{ra,rb}}){
   auto plugin=find_plugin(*s,"urn:ardour:a-comp#stereo",LV2);need(bool(plugin),"compressor");auto insert=std::make_shared<PluginInsert>(*s,*pair.second,plugin);
   need(pair.second->add_processor(insert,PreFader)==0&&pair.second->add_sidechain(insert),"sidechain");insert->activate();
   for(auto value:std::vector<std::pair<unsigned,double>>{{0,.1},{1,20},{3,10},{4,-55},{9,1}})insert->automation_control(Evoral::Parameter(PluginAutomation,0,value.first))->set_value(value.second,PBD::Controllable::NoGroup);
   {PBD::Mutex::Lock lock(engine->process_lock());auto input=insert->sidechain_input();need(input->connect(input->nth(0),pair.first->output()->nth(0)->name())==0,"sidechain routing");}
  }
  auto sink=s->new_audio_route(2,2,{},1,"Native capture",PresentationInfo::AudioBus,0).front();
  auto rsink=s->new_audio_route(2,2,{},1,"Reference capture",PresentationInfo::AudioBus,0).front();
  if(getenv("ARDOUR_PROBE_SEND")){connect_send(*s,a,b);connect_send(*s,ra,rb);}
  {PBD::Mutex::Lock lock(engine->process_lock());std::vector<std::pair<std::shared_ptr<Route>,std::shared_ptr<Route>>> edges{{b,sink},{rb,rsink}};if(!getenv("ARDOUR_PROBE_SEND")){edges.push_back({a,b});edges.push_back({ra,rb});}for(auto edge:edges)for(unsigned c=0;c<2;++c)need(edge.second->input()->connect(edge.second->input()->nth(c),edge.first->output()->nth(c)->name())==0,"main routing");}
  auto tap=std::make_shared<NativeTap>(*s,block),reference=std::make_shared<NativeTap>(*s,block);
  need(sink->add_processor(tap,PreFader)==0&&rsink->add_processor(reference,PreFader)==0,"capture");
  a->monitoring_control()->set_value(MonitorDisk,PBD::Controllable::NoGroup);ra->monitoring_control()->set_value(MonitorDisk,PBD::Controllable::NoGroup);
  samplepos_t loop_start=0,loop_end=0;
  if(getenv("ARDOUR_PROBE_LOOP")){
   Config->set_loop_is_mode(true);auto* location=new Location(*s,timepos_t(samplepos_t(48000)),timepos_t(samplepos_t(57973)),"Native nonaligned loop",Location::IsAutoLoop);
   s->locations()->add(location);s->set_auto_loop_location(location);loop_start=location->start_sample();loop_end=location->end_sample();s->request_play_loop(true);
  }
  s->resort_routes();s->calculate_latency();engine->set_session(s);bool ready=false;auto until=Clock::now()+std::chrono::seconds(5);
  while(!ready&&Clock::now()<until){{PBD::Mutex::Lock lock(engine->process_lock());ready=s->setup_settled()&&!s->global_locate_pending()&&(!loop_end||(s->get_play_loop()&&s->transport_stopped()&&s->transport_sample()==loop_start));}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  need(ready,"setup");engine->set_session(nullptr);
  for(auto route:std::vector<std::shared_ptr<Route>>{a,ra,b,rb})route->foreach_processor([](std::weak_ptr<Processor> weak){auto insert=std::dynamic_pointer_cast<PluginInsert>(weak.lock());if(insert)for(unsigned i=0;i<insert->get_count();++i)insert->plugin(i)->flush();});
  AnticipativeProcess::Timeline timeline(*s,loop_start);auto next=[timeline](AnticipationQueue::Range const& r){return timeline.next(r);};
  auto& plan=s->anticipative_process();
  if(getenv("ARDOUR_PROBE_AUTOMATIC")){
   for(auto route:std::vector<std::shared_ptr<Route>>{ra,rb,sink,rsink})route->set_anticipative_processing_disabled(true);
   s->set_anticipative_processing(true,200);s->request_roll();engine->set_session(s);
   until=Clock::now()+std::chrono::seconds(10);bool ready=false;
   while(!ready&&Clock::now()<until){{PBD::Mutex::Lock lock(engine->process_lock());ready=plan.running()&&plan.find_node(a.get())&&plan.find_node(b.get())&&tap->frames>0;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
   need(ready,"automatic native plan");
  }else{
   auto& an=plan.add_route(a,timeline.first(*a),(horizon_samples+block-1)/block,next);auto& bn=plan.add_route(b,timeline.first(*b),(horizon_samples+block-1)/block,next);
   plan.start(1);until=Clock::now()+std::chrono::seconds(10);
   while(!(an.queue->primed()&&bn.queue->primed())&&Clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
   need(an.queue->primed()&&bn.queue->primed(),"native prefill");s->request_roll();engine->set_session(s);
  }
  auto const& an=*plan.find_node(a.get());auto const& bn=*plan.find_node(b.get());
  std::cerr<<"NATIVE TIMELINE preroll="<<s->worst_latency_preroll_buffer_size_ceil()<<" source="<<an.first.project<<","<<an.first.continuous<<","<<an.first.frames<<" bus="<<bn.first.project<<","<<bn.first.continuous<<","<<bn.first.frames<<std::endl;
  until=Clock::now()+std::chrono::seconds(30);bool armed=false,released=false;
  while(Clock::now()<until){unsigned k;{PBD::Mutex::Lock lock(engine->process_lock());k=tap->k;}
   if(k>=total_blocks)break;if(!armed&&k>=arm_block){a->rec_enable_control()->set_value(1,PBD::Controllable::NoGroup);armed=true;}
   if(armed&&!released&&k>=demote_block){a->rec_enable_control()->set_value(0,PBD::Controllable::NoGroup);released=true;}
   std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  engine->set_session(nullptr);plan.stop();need(tap->k==total_blocks&&reference->k==total_blocks,"complete");
  unsigned first=tap->captured.size();for(unsigned i=0;i<tap->captured.size();++i){auto d=double(std::abs(tap->captured[i]-reference->captured[i]));error=std::max(error,d);if(d>1e-7&&first==tap->captured.size())first=i;}
  peak=tap->peak;handoffs=an.queue->live_handoffs()+bn.queue->live_handoffs();misses=an.queue->underruns()+bn.queue->underruns();
  std::cerr<<"NATIVE ROUTES error="<<error<<" first="<<first<<" peak="<<peak<<" handoffs="<<handoffs<<" misses="<<misses<<" faults="<<an.queue->discontinuities()<<","<<bn.queue->discontinuities()<<std::endl;
  need(error==0&&peak>1e-4&&handoffs==2&&misses==0&&released,"native route equivalence");plan.clear();
 }
 delete s;engine->stop();std::cout<<"RESULT {\"passed\":true,\"original_registered_route_dsp\":true,\"cached_bus\":true,\"automatic_arm_handoffs\":"<<handoffs<<",\"block\":"<<block<<",\"max_sample_error\":"<<error<<",\"cache_underruns\":"<<misses<<"}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(std::exception const& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}

#endif
