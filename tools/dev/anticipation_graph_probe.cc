// Cached source -> cached bus -> live mix, with source fan-out and handoffs.
#define ANTICIPATION_CALLBACK_HELPERS_ONLY
#include "anticipation_callback_probe.cc"

struct SidechainInput {
 std::shared_ptr<TapCache> cache;std::shared_ptr<InternalSend> current;
 std::atomic<unsigned> consumed{0};unsigned k=0,misses=0,block;double peak=0;
 SidechainInput(unsigned n,std::shared_ptr<TapCache> c,std::shared_ptr<InternalSend> send):cache(c),current(send),block(n){}
 static void read(void* context,BufferSet& b,ChanCount const& offset,samplepos_t start,pframes_t n){
  auto& self=*static_cast<SidechainInput*>(context);
  need(n==self.block&&start==samplepos_t(self.k*n),"sidechain timestamp");
  auto& out=b.get_audio(offset.n_audio());
  if(self.cache){
   if(self.cache->write.load(std::memory_order_acquire)>self.k)out.read_from(self.cache->slots[self.k%self.cache->capacity].data(),n);
   else {out.silence(n);++self.misses;}
  }else out.read_from(self.current->get_buffers().get_audio(0),n);
  for(unsigned i=0;i<n;++i)self.peak=std::max(self.peak,double(std::abs(static_cast<const AudioBuffer&>(out).data()[i])));
  self.consumed.store(++self.k,std::memory_order_release);
 }
};
static std::shared_ptr<PluginInsert> add_compressor(Session& s,Fixture& f){
 auto plugin=find_plugin(s,"urn:ardour:a-comp#stereo",LV2);need(bool(plugin),"stereo compressor");
 auto insert=std::make_shared<PluginInsert>(s,*f.route,plugin);
 need(f.route->add_processor(insert,PreFader)==0,"add compressor");insert->activate();
 need(f.route->add_sidechain(insert),"configure actual sidechain");f.endpoint=insert;
 for(auto parameter:std::vector<std::pair<unsigned,double>>{{0,.1},{1,20},{3,10},{4,-55},{9,1}})
  insert->automation_control(Evoral::Parameter(PluginAutomation,0,parameter.first))->set_value(parameter.second,PBD::Controllable::NoGroup);
 return insert;
}

int main(int argc,char** argv){try{
 need(argc==2,"session path");need(std::string(g_get_home_dir())==getenv("ARDOUR_PROBE_HOME"),"preferences isolation");
 need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","AnticipationGraph","");
 need(bool(backend)&&backend->set_driver("Realtime")==0,"backend");
 need(backend->set_sample_rate(48000)==0,"sample rate");
 const unsigned block=getenv("ARDOUR_PROBE_BLOCK")?std::stoul(getenv("ARDOUR_PROBE_BLOCK")):64;
 need(backend->set_buffer_size(block)==0&&engine->start()==0,"start");
 auto* s=new Session(*engine,argv[1],"graph-probe",nullptr,"",true);engine->set_session(nullptr);
 double error=0,peak=0;unsigned misses=0,late=0,source_cut=0,bus_cut=0;
 {
  Fixture a(*s,"Private source A",true,true,false),ar(*s,"Private source reference",true,true,false);
  Fixture b(*s,"Private bus B",true,false,false),br(*s,"Private bus reference",true,false,false);
  Fixture live(*s,"Live input"),mix(*s,"Live master",true,false),reference(*s,"Reference master",true,false);
  auto route=[&](const char* name){return s->new_audio_route(2,2,{},1,name,PresentationInfo::AudioBus,0).front();};
  auto ao=route("A delivery"),aro=route("A reference delivery"),bo=route("B delivery"),bro=route("B reference delivery");
  auto ab=std::make_shared<Bridge>(*s,a,block),arb=std::make_shared<Bridge>(*s,ar,block,false);
  auto bb=std::make_shared<Bridge>(*s,b,block),brb=std::make_shared<Bridge>(*s,br,block,false);
  need(ao->add_processor(ab,PreFader)==0&&aro->add_processor(arb,PreFader)==0&&bo->add_processor(bb,PreFader)==0&&bro->add_processor(brb,PreFader)==0,"bridges");
  auto send=configure_midchain_send(*s,a,b.route);
  auto refsend=configure_midchain_send(*s,ar,br.route);
  auto cache=std::make_shared<TapCache>(block,ab->capacity);
  std::shared_ptr<PluginInsert> comp,refcomp;
  double reduction_peak=0,reference_reduction_peak=0;
  std::unique_ptr<SidechainInput> sc,rsc;
  if(getenv("ARDOUR_PROBE_SIDECHAIN")){
   comp=add_compressor(*s,b);refcomp=add_compressor(*s,br);
   sc.reset(new SidechainInput(block,cache,nullptr));rsc.reset(new SidechainInput(block,nullptr,refsend));
   comp->sidechain()->set_cached_input_reader(&SidechainInput::read,sc.get());
   refcomp->sidechain()->set_cached_input_reader(&SidechainInput::read,rsc.get());
   ab->extra_reader2=&sc->consumed;s->ensure_buffers_unlocked(ChanCount(DataType::AUDIO,3));
   bb->after_render=[&](unsigned){reduction_peak=std::max(reduction_peak,double(comp->plugin(0)->get_parameter(6)));};
   brb->after_render=[&](unsigned){reference_reduction_peak=std::max(reference_reduction_peak,double(refcomp->plugin(0)->get_parameter(6)));};
  }
  CachedTapReturn reader(*s,send.get(),cache,false);
  b.route->internal_return()->set_cached_input_reader(&CachedTapReturn::read,&reader);
  ab->extra_reader=&cache->read;
  ab->after_render=[cache,send](unsigned k){cache->publish(k,send->get_buffers());};
  bb->ready_for_render=[cache](unsigned k){return cache->write.load(std::memory_order_acquire)>k;};
  // Mirror the private DSP edge in the ordinary callback graph. Delivery here
  // uses zero-gain sends because the bridge obtains that edge's audio from its
  // private InternalReturn. These nodes are fixture adapters, not extra DAW tracks.
  connect_send(*s,ao,bo)->gain_control()->set_value(0,PBD::Controllable::NoGroup);
  connect_send(*s,aro,bro)->gain_control()->set_value(0,PBD::Controllable::NoGroup);
  for(auto p:{ao,bo,live.route})connect_send(*s,p,mix.route);
  for(auto p:{aro,bro,live.route})connect_send(*s,p,reference.route);
  auto tap=std::make_shared<Tap>(*s,block),rtap=std::make_shared<Tap>(*s,block);
  need(mix.route->add_processor(tap,PostFader)==0&&reference.route->add_processor(rtap,PostFader)==0,"output capture");
  s->resort_routes();ab->start_worker();bb->start_worker();
  const auto fill_deadline=Clock::now()+std::chrono::seconds(10);
  while(bb->write.load()<bb->capacity&&Clock::now()<fill_deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  need(bb->write.load()==bb->capacity,"dependency prefill failed");
  engine->dsp_stats[AudioEngine::ProcessCallback].queue_reset();s->request_roll();engine->set_session(s);
  const auto deadline=Clock::now()+std::chrono::seconds(45);
  while(!bb->done.load(std::memory_order_acquire)&&Clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(2));
  engine->set_session(nullptr);ab->stop_worker();bb->stop_worker();
  b.route->internal_return()->set_cached_input_reader(nullptr,nullptr);
  if(comp){comp->sidechain()->set_cached_input_reader(nullptr,nullptr);refcomp->sidechain()->set_cached_input_reader(nullptr,nullptr);}
  need(ab->done&&bb->done&&tap->k==total_blocks&&rtap->k==total_blocks,"incomplete graph");
  for(unsigned i=0;i<tap->captured.size();++i)error=std::max(error,double(std::abs(tap->captured[i]-rtap->captured[i])));
  misses=ab->misses+bb->misses+reader.misses;late=ab->late+bb->late;peak=tap->peak;
  source_cut=ab->cutover;bus_cut=bb->cutover;
  std::cerr<<"CACHED GRAPH source_cutover="<<source_cut<<" bus_cutover="<<bus_cut<<" cached_return_blocks="<<reader.k<<" error="<<error<<" misses="<<misses<<std::endl;
  std::cerr<<"ENGINE observed_max_us="<<std::max(ab->observed_engine_max,bb->observed_engine_max)<<std::endl;
  if(sc){
   std::cerr<<"SIDECHAIN blocks="<<sc->k<<" reference_blocks="<<rsc->k<<" misses="<<sc->misses<<" peak="<<sc->peak<<" peak_gain_reduction_db="<<reduction_peak<<","<<reference_reduction_peak<<std::endl;
   need(sc->k==total_blocks&&rsc->k==total_blocks&&sc->misses==0&&sc->peak>1e-4&&reduction_peak>1,"sidechain not exercised");
  }
  need(reader.k==total_blocks&&bus_cut<=source_cut&&bus_cut>=arm_block,"invalid dependency handoff");
 }
 delete s;engine->stop();
 PBD::microseconds_t minimum,maximum;double average,deviation;
 need(engine->dsp_stats[AudioEngine::ProcessCallback].get_stats(minimum,maximum,average,deviation),"timing");
 need(error<1e-5&&misses==0&&peak>1e-4,"cached graph differs from direct graph");
 std::cout<<"RESULT {\"passed\":true,\"normal_session_callback\":true,\"backend\":\"Dummy Realtime\",\"blocks\":["<<block<<"],\"verified_bus_blocks\":"<<total_blocks<<",\"max_sample_error\":"<<error<<",\"cache_underruns\":"<<misses<<",\"bridge_deadline_overruns\":"<<late<<",\"engine_mean_us\":"<<average<<",\"engine_max_us\":"<<maximum<<",\"source_cutover\":"<<source_cut<<",\"bus_cutover\":"<<bus_cut<<",\"peak\":"<<peak<<",\"hardware_latency_measured\":false}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
