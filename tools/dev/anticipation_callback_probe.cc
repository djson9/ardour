// Normal Session/Graph/Route callback experiment, using the dummy backend.
// Reuse the correctness probe's real-Route fixtures, not its main function.
#define main correctness_main_unused
#include "anticipation_probe.cc"
#undef main
#include <chrono>
using Clock=std::chrono::steady_clock;
constexpr unsigned total_blocks=2048,horizon_samples=9600,arm_block=512,demote_block=1024;

struct Bridge : Processor {
 enum Mode {Ahead,Drain,Live};
 Fixture& source;unsigned block,capacity;bool cache_enabled,transitions;
 std::vector<std::vector<float>> slots;
 std::atomic<unsigned> read{0},write{0},mode{Ahead};
 std::atomic<bool> acknowledged{false},quit{false},done{false};
 unsigned k=0,misses=0,late=0,cutover=0,bad_size=0;double max_us=0;
 double source_peak=0,tail_peak=0;
 double observed_engine_max=0,steady_engine_max=0;unsigned observed_engine_max_block=0,steady_engine_max_block=0;
 std::thread worker;
 char callback_thread_name[64]={0};
 std::shared_ptr<DiskInput> disk_input;std::shared_ptr<MonitorRange> monitor_range;
 std::shared_ptr<AudioTrack> reader_track;bool follow_record_arm=false;
 std::atomic<unsigned> completed_blocks{0};unsigned request_block=0,release_block=0,reader_monitor_blocks=0;
 std::function<void(unsigned)> after_render;
 std::function<bool(unsigned)> ready_for_render;
 std::atomic<unsigned>* extra_reader=nullptr;
 std::atomic<unsigned>* extra_reader2=nullptr;
 Bridge(Session& s,Fixture& f,unsigned n,bool cached=true,bool handoffs=true):Processor(s,"Cache bridge",s),source(f),block(n),capacity((horizon_samples+n-1)/n),cache_enabled(cached),transitions(handoffs),slots(capacity,std::vector<float>(2*n)){if(!cached)mode=Live;}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out) override {out=in;return true;}
 void start_worker(){worker=std::thread([&]{
  Temporal::TempoMap::fetch();ProcessThread pt;pt.get_buffers();BufferSet b;prepare(b,block);
  b.ensure_buffers(DataType::AUDIO,std::max(2u,source.route->max_processor_streams().n_audio()),block);
  while(!quit.load(std::memory_order_acquire)){
   auto m=mode.load(std::memory_order_acquire);
   if(m==Drain){acknowledged.store(true,std::memory_order_release);std::this_thread::sleep_for(std::chrono::microseconds(50));continue;}
   if(m!=Ahead){std::this_thread::sleep_for(std::chrono::microseconds(50));continue;}
   const auto w=write.load();auto r=read.load(std::memory_order_acquire);
   if(extra_reader)r=std::min(r,extra_reader->load(std::memory_order_acquire));
   if(extra_reader2)r=std::min(r,extra_reader2->load(std::memory_order_acquire));
   if(w>=total_blocks||w-r>=capacity){std::this_thread::sleep_for(std::chrono::microseconds(50));continue;}
   if(ready_for_render&&!ready_for_render(w)){std::this_thread::sleep_for(std::chrono::microseconds(50));continue;}
   source.run(b,w*block,block);
   if(after_render)after_render(w);
   for(unsigned c=0;c<2;++c)std::copy_n(b.get_audio(c).data(),block,slots[w%capacity].begin()+c*block);
   write.store(w+1,std::memory_order_release);
  }
  pt.drop_buffers();
 });}
 void stop_worker(){quit.store(true,std::memory_order_release);worker.join();}
 void run(BufferSet& b,samplepos_t start,samplepos_t,double speed,pframes_t n,bool) override {
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
  const bool wants_live=follow_record_arm?reader_track->rec_enable_control()->get_value()!=0:k>=arm_block&&k<demote_block;
  if(cache_enabled&&transitions&&mode.load()==Ahead&&wants_live){request_block=k;mode.store(Drain,std::memory_order_release);}
  if(monitor_range&&cache_enabled&&mode.load()==Drain&&acknowledged.load(std::memory_order_acquire)){
   samplepos_t unset=max_samplepos;monitor_range->from.compare_exchange_strong(unset,write.load(std::memory_order_acquire)*block);
  }
  if(mode.load()==Drain&&acknowledged.load(std::memory_order_acquire)&&read.load()==write.load(std::memory_order_acquire)){
   mode.store(Live,std::memory_order_release);cutover=k;
  }
  if(mode.load()==Live){
   if(reader_track&&monitor_range){
    const bool input=samplepos_t(k*block)>=monitor_range->from.load()&&samplepos_t(k*block)<monitor_range->until.load();
    const auto desired=input?MonitorInput:MonitorDisk;
    if(reader_track->monitoring_control()->get_value()!=desired)reader_track->monitoring_control()->set_value(desired,PBD::Controllable::NoGroup);
    if(input)++reader_monitor_blocks;
   }
   if(disk_input)disk_input->capture_monitor_input(b,start,block);source.run(b,k*block,block);if(after_render)after_render(k);
  }
  else if(write.load(std::memory_order_acquire)>k){
   for(unsigned c=0;c<2;++c)b.get_audio(c).read_from(slots[k%capacity].data()+c*block,block);
   read.store(k+1,std::memory_order_release);
  }else {++misses;b.silence(n,0);read.store(k+1,std::memory_order_release);}
  for(unsigned c=0;c<2;++c)for(unsigned i=0;i<block;++i){const auto value=double(std::abs(b.get_audio(c).data()[i]));source_peak=std::max(source_peak,value);if(k>=total_blocks-128)tail_peak=std::max(tail_peak,value);}
  ++k;
  if(cache_enabled&&transitions&&mode.load()==Live&&(follow_record_arm?!wants_live:k==demote_block)){
   release_block=k;if(monitor_range)monitor_range->until.store(k*block,std::memory_order_release);
   if(reader_track)reader_track->monitoring_control()->set_value(MonitorDisk,PBD::Controllable::NoGroup);
   read.store(k);write.store(k);acknowledged.store(false);
   mode.store(Ahead,std::memory_order_release);
  }
  const double us=std::chrono::duration<double,std::micro>(Clock::now()-begin).count();
  max_us=std::max(max_us,us);if(us>1e6*block/48000.)++late;
  if(k==total_blocks)done.store(true,std::memory_order_release);
  completed_blocks.store(k,std::memory_order_release);
 }
};
struct Tap : Processor {
 std::vector<float> captured;unsigned block,k=0;double peak=0;
 Tap(Session& s,unsigned n):Processor(s,"Bus comparison",s),captured(total_blocks*2*n),block(n){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out) override{out=in;return true;}
 void run(BufferSet& b,samplepos_t,samplepos_t,double speed,pframes_t n,bool)override{
  if(speed!=1||k>=total_blocks||n!=block)return;
  const BufferSet& input=b;
  for(unsigned c=0;c<2;++c)for(unsigned i=0;i<block;++i){
   captured[(k*2+c)*block+i]=input.get_audio(c).data()[i];
   peak=std::max(peak,double(std::abs(input.get_audio(c).data()[i])));
  }++k;
 }
};
static std::shared_ptr<InternalSend> connect_send(Session& s,std::shared_ptr<Route> from,std::shared_ptr<Route> to){
 auto list=std::make_shared<RouteList>();list->push_back(from);s.add_internal_sends(to,PostFader,list);
 auto send=std::dynamic_pointer_cast<InternalSend>(from->internal_send_for(to));need(bool(send),"send");
 send->gain_control()->set_value(.37,PBD::Controllable::NoGroup);send->activate();return send;
}
// A timestamp-ordered boundary cache, retained until both the route output and
// send consumer have advanced. Publish live send output too: the worker may
// resume before a downstream live bus has consumed this cycle's send.
struct TapCache {
 unsigned block,capacity;std::vector<std::vector<float>> slots;std::vector<float> capture;
 std::atomic<unsigned> read{0},write{0};
 TapCache(unsigned b,unsigned c):block(b),capacity(c),slots(c,std::vector<float>(2*b)),capture(total_blocks*2*b){}
 void publish(unsigned k,const BufferSet& b){
  for(unsigned c=0;c<2;++c)std::copy_n(b.get_audio(c).data(),block,slots[k%capacity].begin()+c*block);
  for(unsigned c=0;c<2;++c)std::copy_n(b.get_audio(c).data(),block,capture.begin()+(k*2+c)*block);
  write.store(k+1,std::memory_order_release);
 }
};
struct CachedTapReturn {
 Session& session;InternalSend const* send;std::shared_ptr<TapCache> cache;unsigned k=0,misses=0;bool require_rolling;
 CachedTapReturn(Session& s,InternalSend const* input,std::shared_ptr<TapCache> c,bool rolling=true):session(s),send(input),cache(c),require_rolling(rolling){}
 static bool read(void* context,InternalSend const* input,BufferSet& b,samplepos_t start,pframes_t n){
  auto& self=*static_cast<CachedTapReturn*>(context);
  if(input!=self.send)return false;
  self.mix(b,start,n);return true;
 }
 void mix(BufferSet& b,samplepos_t start,pframes_t n){
  if((require_rolling&&!session.transport_rolling())||k>=total_blocks)return;
  need(n==cache->block&&start==samplepos_t(k*n),"tap timeline mismatch");
  if(cache->write.load(std::memory_order_acquire)>k){
   for(unsigned c=0;c<2;++c)b.get_audio(c).accumulate_from(cache->slots[k%cache->capacity].data()+c*n,n);
  }else ++misses;
  cache->read.store(++k,std::memory_order_release);
 }
};
static std::shared_ptr<InternalSend> configure_midchain_send(Session& s,Fixture& fixture,std::shared_ptr<Route> dest){
 auto send=connect_send(s,fixture.route,dest);send->gain_control()->set_value(.23,PBD::Controllable::NoGroup);
 send->set_manual_cycle_start(true);
 Route::ProcessorList ordered;std::vector<std::shared_ptr<Processor>> plugins;
 fixture.route->foreach_processor([&](std::weak_ptr<Processor> weak){auto p=weak.lock();ordered.push_back(p);if(std::dynamic_pointer_cast<PluginInsert>(p))plugins.push_back(p);});
 need(plugins.size()==4,"midchain fixture requires four inserts");
 ordered.remove(send);auto where=std::find(ordered.begin(),ordered.end(),plugins[1]);ordered.insert(std::next(where),send);
 need(fixture.route->reorder_processors(ordered)==0,"move send between actual inserts");
 auto route=fixture.route;auto prefix=plugins[1];std::vector<std::shared_ptr<Processor>> suffix{plugins[2],plugins[3]};
 fixture.render_override=[route,prefix,suffix,send](BufferSet& b,samplepos_t start,unsigned n){
  auto track=std::dynamic_pointer_cast<AudioTrack>(route);
  if(!track||!(track->monitoring_state()&MonitoringInput))b.silence(n,0);
  (route.get()->*RouteAccess::bounce())(b,start,n,prefix,true,true,false);
  send->cycle_start(n);send->run(b,start,start+n,1,n,true);
  for(auto& p:suffix){p->run(b,start,start+n,1,n,true);b.set_count(p->output_streams());}
 };
 return send;
}
#ifndef ANTICIPATION_CALLBACK_HELPERS_ONLY
int main(int argc,char** argv){try{
 need(argc==2,"session path");need(std::string(g_get_home_dir())==getenv("ARDOUR_PROBE_HOME"),"preferences isolation");
 need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();
 if(getenv("ARDOUR_PROBE_DISK_READER"))Config->set_audio_playback_buffer_seconds(12);
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
   const bool reader_test=getenv("ARDOUR_PROBE_DISK_READER");
   Fixture source(*s,"Private worker source",true,true,false,4,reader_test),reference(*s,"Private reference",true,true,false,4,reader_test);
   std::shared_ptr<AudioTrack> reader_source,reader_reference;
   std::shared_ptr<Tap> source_input,reference_input;
   if(reader_test){
    need(!getenv("ARDOUR_PROBE_DISK")&&!getenv("ARDOUR_PROBE_MIDI"),"exclusive disk source options");
    auto playlist=make_disk_playlist(*s,std::string(argv[1])+"-"+std::to_string(block)+"/source.wav");
    reader_source=use_real_disk_reader(source,playlist);reader_reference=use_real_disk_reader(reference,playlist);
    auto trace_input=[&](Fixture& f){std::shared_ptr<Processor> first;f.route->foreach_processor([&](std::weak_ptr<Processor> w){auto p=w.lock();if(!first&&std::dynamic_pointer_cast<PluginInsert>(p))first=p;});auto tap=std::make_shared<Tap>(*s,block);need(f.route->add_processor(tap,first)==0,"source input trace");return tap;};
    source_input=trace_input(source);reference_input=trace_input(reference);
   }
   std::shared_ptr<MidiNotes> midi_source,midi_reference;
   if(getenv("ARDOUR_PROBE_MIDI")){
    need(!getenv("ARDOUR_PROBE_DISK")&&!getenv("ARDOUR_PROBE_TAP"),"MIDI fixture excludes disk/midchain setup");
    midi_source=use_midi_synth(*s,source,block);midi_reference=use_midi_synth(*s,reference,block);
   }
   std::shared_ptr<DiskInput> disk_source,disk_reference;
   if(getenv("ARDOUR_PROBE_DISK")){
    auto playlist=make_disk_playlist(*s,std::string(argv[1])+"-"+std::to_string(block)+"/source.wav");
    disk_source=use_disk_input(*s,source,playlist);disk_reference=use_disk_input(*s,reference,playlist);
   }
   auto output=s->new_audio_route(2,2,{},1,"Cached output",PresentationInfo::AudioBus,0).front();
   auto ref_output=s->new_audio_route(2,2,{},1,"Direct output",PresentationInfo::AudioBus,0).front();
   Fixture live(*s,"Live callback source"),bus(*s,"Shared callback bus",true,false),bus_ref(*s,"Reference callback bus",true,false);
   auto bridge=std::make_shared<Bridge>(*s,source,block);need(output->add_processor(bridge,PreFader)==0,"bridge");
   auto ref_bridge=std::make_shared<Bridge>(*s,reference,block,false);need(ref_output->add_processor(ref_bridge,PreFader)==0,"reference bridge");
   connect_send(*s,output,bus.route);connect_send(*s,live.route,bus.route);
   connect_send(*s,ref_output,bus_ref.route);connect_send(*s,live.route,bus_ref.route);
   if(getenv("ARDOUR_PROBE_MONITOR")){
    need(bool(disk_source)||reader_test,"monitor probe requires disk source");
    auto range=std::make_shared<MonitorRange>();if(!getenv("ARDOUR_PROBE_RECORD_CONTROL"))range->until=demote_block*block;
    if(disk_source)disk_source->monitor_range=disk_reference->monitor_range=range;
    bridge->disk_input=disk_source;ref_bridge->disk_input=disk_reference;
    bridge->reader_track=reader_source;ref_bridge->reader_track=reader_reference;
    bridge->follow_record_arm=getenv("ARDOUR_PROBE_RECORD_CONTROL");
    if(bridge->follow_record_arm)need(reader_test,"record control requires actual AudioTrack");
    bridge->monitor_range=ref_bridge->monitor_range=range;
    // A normal live route supplies this cycle's input to both paths. The
    // producer never receives that buffer; it only reads predictable media.
    connect_send(*s,live.route,output);connect_send(*s,live.route,ref_output);
   }
   std::shared_ptr<CachedTapReturn> cached_return;
   std::shared_ptr<InternalSend> mid_send,reference_mid_send;
   std::shared_ptr<TapCache> mid_cache,reference_mid_cache;
   std::shared_ptr<Tap> mid_input,reference_mid_input;
   if(getenv("ARDOUR_PROBE_TAP")){
    auto send=configure_midchain_send(*s,source,bus.route);
    mid_send=send;reference_mid_send=configure_midchain_send(*s,reference,bus_ref.route);
    // Both use the ordinary InternalReturn, preserving send ordering and trim.
    auto cache=std::make_shared<TapCache>(block,bridge->capacity);
    mid_cache=cache;reference_mid_cache=std::make_shared<TapCache>(block,ref_bridge->capacity);
    ref_bridge->after_render=[reference_mid_cache,reference_mid_send](unsigned k){reference_mid_cache->publish(k,reference_mid_send->get_buffers());};
    bridge->extra_reader=&cache->read;bridge->after_render=[cache,send](unsigned k){cache->publish(k,send->get_buffers());};
    cached_return=std::make_shared<CachedTapReturn>(*s,send.get(),cache);
    bus.route->internal_return()->set_cached_input_reader(&CachedTapReturn::read,cached_return.get());
    auto insert_input_tap=[&](Fixture& f){
     std::shared_ptr<Processor> first_plugin;
     f.route->foreach_processor([&](std::weak_ptr<Processor> w){auto p=w.lock();std::cerr<<p->name()<<" | ";if(!first_plugin&&std::dynamic_pointer_cast<PluginInsert>(p))first_plugin=p;});std::cerr<<std::endl;
     auto tap=std::make_shared<Tap>(*s,block);need(f.route->add_processor(tap,first_plugin)==0,"input capture");return tap;
    };
    mid_input=insert_input_tap(bus);reference_mid_input=insert_input_tap(bus_ref);
   }
   auto tap=std::make_shared<Tap>(*s,block);need(bus.route->add_processor(tap,PostFader)==0,"tap");
   auto ref_tap=std::make_shared<Tap>(*s,block);need(bus_ref.route->add_processor(ref_tap,PostFader)==0,"reference tap");
   s->resort_routes();
   if(reader_test){
    // Finish queued initial monitoring/locate work before publishing a render
    // plan. global_locate_pending is refreshed by the Session callback; merely
    // waiting with the Session detached can leave a stale true value forever.
    engine->set_session(s);bool ready=false;const auto setup_deadline=Clock::now()+std::chrono::seconds(5);
    while(!ready&&Clock::now()<setup_deadline){
     {PBD::Mutex::Lock lock(engine->process_lock());ready=!s->global_locate_pending()&&!DiskReader::no_disk_output()&&reader_source->monitoring_control()->get_value()==MonitorDisk&&reader_reference->monitoring_control()->get_value()==MonitorDisk;}
     if(!ready)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    engine->set_session(nullptr);need(ready,"initial track controls/locate did not settle");
    need(reader_source->seek(0,true)==0&&reader_reference->seek(0,true)==0,"prefill after setup barrier");
   }
   if(reader_test)std::cerr<<"READER PREFILL monitoring="<<reader_source->monitoring_state()<<","<<reader_reference->monitoring_state()<<" choice="<<reader_source->monitoring_control()->get_value()<<","<<reader_reference->monitoring_control()->get_value()<<std::endl;
   if(getenv("ARDOUR_PROBE_SEND_DELAY")){
    need(bool(mid_send),"send delay requires midchain tap");
    // Exercise the actual InternalSend compensation delay line. Full automatic
    // graph latency propagation is a separate test from this boundary test.
    const auto delay=std::stoul(getenv("ARDOUR_PROBE_SEND_DELAY"));
    mid_send->set_delay_in(delay);reference_mid_send->set_delay_in(delay);
   }
   bridge->start_worker();while(bridge->write.load()<bridge->capacity)std::this_thread::sleep_for(std::chrono::milliseconds(1));
   engine->dsp_stats[AudioEngine::ProcessCallback].queue_reset();s->request_roll();engine->set_session(s);
   const auto run_start=Clock::now();auto deadline=run_start+std::chrono::seconds(45);
   bool requested_arm=false,requested_disarm=false;
   while(!bridge->done.load(std::memory_order_acquire)&&Clock::now()<deadline){
    if(bridge->follow_record_arm){
     const auto completed=bridge->completed_blocks.load(std::memory_order_acquire);
     if(!requested_arm&&completed>=arm_block){reader_source->rec_enable_control()->set_value(1,PBD::Controllable::NoGroup);requested_arm=true;}
     if(!requested_disarm&&completed>=demote_block){reader_source->rec_enable_control()->set_value(0,PBD::Controllable::NoGroup);requested_disarm=true;}
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
   }
   engine->set_session(nullptr);bridge->stop_worker();
   if(cached_return)bus.route->internal_return()->set_cached_input_reader(nullptr,nullptr);
   std::cerr<<"CALLBACK wall_seconds="<<std::chrono::duration<double>(Clock::now()-run_start).count()<<" sample_rate="<<backend->sample_rate()<<" buffer="<<backend->buffer_size()<<" backend="<<backend_name<<std::endl;
   double error=0;unsigned error_index=0;double phases[4]={0,0,0,0};
   for(unsigned i=0;i<tap->captured.size();++i){double diff=std::abs(tap->captured[i]-ref_tap->captured[i]);phases[i/(512*2*block)]=std::max(phases[i/(512*2*block)],diff);if(diff>error){error=diff;error_index=i;}}
   std::cerr<<"CALLBACK block="<<block<<" processed="<<bridge->k<<" taps="<<tap->k<<" misses="<<bridge->misses<<" cutover="<<bridge->cutover<<" error="<<error<<" peak="<<tap->peak<<" max_us="<<bridge->max_us<<std::endl;
   std::cerr<<"ENGINE observed_max_us="<<bridge->observed_engine_max<<" at_block="<<bridge->observed_engine_max_block<<std::endl;
   std::cerr<<"ENGINE steady_max_us="<<bridge->steady_engine_max<<" at_block="<<bridge->steady_engine_max_block<<std::endl;
   if(disk_source){
    std::cerr<<"PLAYLIST reads="<<disk_source->reads<<" reference_reads="<<disk_reference->reads<<" monitor_blocks="<<disk_source->monitor_blocks<<" monitor_peak="<<disk_source->monitor_peak<<std::endl;
    need(disk_source->reads+disk_source->monitor_blocks==total_blocks&&disk_reference->reads+disk_reference->monitor_blocks==total_blocks,"playlist cursor discontinuity");
    if(getenv("ARDOUR_PROBE_MONITOR"))need(disk_source->monitor_blocks==demote_block-bridge->cutover&&disk_reference->monitor_blocks==disk_source->monitor_blocks&&disk_source->monitor_peak>1e-4,"monitoring input was not consumed");
   }
   if(midi_source){
    std::cerr<<"MIDI source_events="<<midi_source->events<<" reference_events="<<midi_reference->events<<" calls="<<midi_source->calls<<" source_peak="<<bridge->source_peak<<" tail_peak="<<bridge->tail_peak<<std::endl;
    need(midi_source->events==4&&midi_reference->events==4&&midi_source->calls==total_blocks,"held MIDI notes incomplete");
    need(bridge->source_peak>1e-4&&bridge->tail_peak<bridge->source_peak*.05,"synth silent or note release failed");
   }
   if(reader_test){
    double input_error=0;unsigned first=source_input->captured.size();for(unsigned i=0;i<source_input->captured.size();++i){double d=std::abs(source_input->captured[i]-reference_input->captured[i]);if(d>1e-7&&first==source_input->captured.size())first=i;input_error=std::max(input_error,d);}
    std::cerr<<"READER INPUT error="<<input_error<<" first_block="<<(first/(2*block))<<" peaks="<<source_input->peak<<","<<reference_input->peak<<std::endl;
    if(input_error>1e-7)for(unsigned at:{0u,1u,16u,63u,128u,191u,1280u,19199u,19200u})std::cerr<<"INPUT SAMPLE "<<at<<" "<<source_input->captured[at]<<" "<<reference_input->captured[at]<<std::endl;
    std::cerr<<"DISK_READER source_peak="<<bridge->source_peak<<" remaining_buffer="<<reader_source->playback_buffer_load()<<" monitor_blocks="<<bridge->reader_monitor_blocks<<","<<ref_bridge->reader_monitor_blocks<<" record_request_block="<<bridge->request_block<<" release_block="<<bridge->release_block<<std::endl;
    need(bridge->source_peak>1e-4,"actual DiskReader produced silence");
    if(getenv("ARDOUR_PROBE_MONITOR"))need(bridge->reader_monitor_blocks>0&&bridge->reader_monitor_blocks==ref_bridge->reader_monitor_blocks,"actual input monitoring not exercised");
   }
   if(cached_return){
    double raw_error=0;unsigned first_raw=mid_cache->capture.size();for(unsigned i=0;i<mid_cache->capture.size();++i){auto diff=double(std::abs(mid_cache->capture[i]-reference_mid_cache->capture[i]));if(diff>1e-7&&first_raw==mid_cache->capture.size())first_raw=i;raw_error=std::max(raw_error,diff);}
    if(first_raw<mid_cache->capture.size())std::cerr<<"FIRST RAW block="<<(first_raw/(2*block))<<" actual="<<mid_cache->capture[first_raw]<<" reference="<<reference_mid_cache->capture[first_raw]<<std::endl;
    double input_error=0;for(unsigned i=0;i<mid_input->captured.size();++i)input_error=std::max(input_error,double(std::abs(mid_input->captured[i]-reference_mid_input->captured[i])));std::cerr<<"BUS INPUT ERROR "<<input_error<<std::endl;
    std::cerr<<"MIDCHAIN TAP blocks="<<cached_return->k<<" misses="<<cached_return->misses<<" max_error_block="<<(error_index/(2*block))<<" phases="<<phases[0]<<","<<phases[1]<<","<<phases[2]<<","<<phases[3]<<" delay_out="<<mid_send->get_delay_out()<<","<<reference_mid_send->get_delay_out()<<" raw_send_error="<<raw_error<<" automation_play="<<mid_send->gain_control()->automation_playback()<<std::endl;
    need(cached_return->k==total_blocks&&cached_return->misses==0,"midchain cache failed");
   }
   need(bridge->done&&tap->k==total_blocks&&ref_tap->k==total_blocks&&bridge->bad_size==0,"callback incomplete");
   need(bridge->cutover>=bridge->request_block&&bridge->cutover<=bridge->request_block+bridge->capacity+1,"invalid handoff cursor");
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
