// MIDI crosses the cache boundary; the receiving synth remains in the live graph.
#define ANTICIPATION_CALLBACK_HELPERS_ONLY
#include "anticipation_callback_probe.cc"
#include <array>
#include <climits>
struct MidiBridge : Bridge {
 std::vector<std::unique_ptr<MidiBuffer>> midi_slots;
 std::vector<unsigned> tags;std::atomic<unsigned> midi_read{0};
 std::array<samplepos_t,4> delivered{};unsigned events=0;
 MidiBridge(Session& s,Fixture& f,unsigned n,bool cached):Bridge(s,f,n,cached),tags(capacity,UINT_MAX){
  for(unsigned i=0;i<capacity;++i)midi_slots.emplace_back(new MidiBuffer(8192));
  extra_reader=&midi_read;
  after_output=[this](unsigned k,const BufferSet& b){midi_slots[k%capacity]->copy(b.get_midi(0));tags[k%capacity]=k;};
 }
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;out.set(DataType::MIDI,1);return true;}
 void run(BufferSet& b,samplepos_t start,samplepos_t end,double speed,pframes_t n,bool more)override{
  const auto index=k,previous_misses=misses;auto count=b.count();count.set(DataType::MIDI,1);b.set_count(count);
  Bridge::run(b,start,end,speed,n,more);
  count=b.count();count.set(DataType::MIDI,1);b.set_count(count);
  if(k==index+1&&misses==previous_misses){
   need(tags[index%capacity]==index,"stale MIDI queue slot");b.get_midi(0).read_from(*midi_slots[index%capacity],n);
   const MidiBuffer& midi=b.get_midi(0);for(auto event:midi){need(events<delivered.size(),"duplicate MIDI event");delivered[events++]=start+event.time();}
  }else b.get_midi(0).clear();
  midi_read.store(k,std::memory_order_release);
 }
};
int main(int argc,char** argv){try{
 need(argc==2,"session path");need(std::string(g_get_home_dir())==getenv("ARDOUR_PROBE_HOME"),"preferences isolation");
 need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","MidiBoundaryProbe","");
 const unsigned block=getenv("ARDOUR_PROBE_BLOCK")?std::stoul(getenv("ARDOUR_PROBE_BLOCK")):64;
 need(bool(backend)&&backend->set_driver("Realtime")==0&&backend->set_sample_rate(48000)==0&&backend->set_buffer_size(block)==0&&engine->start()==0,"engine");
 auto* s=new FixtureSession(*engine,argv[1],"midi-boundary-probe",nullptr,"",true);engine->set_session(nullptr);
 double error=0,peak=0,tail=0;unsigned misses=0,late=0,cutover=0;
 {
  Fixture source(*s,"Future MIDI",false,true,false),reference(*s,"Reference MIDI",false,true,false);
  auto notes=std::make_shared<MidiNotes>(*s,block),reference_notes=std::make_shared<MidiNotes>(*s,block);
  need(source.route->replace_processor(source.probe,notes)==0&&reference.route->replace_processor(reference.probe,reference_notes)==0,"MIDI sources");source.endpoint=notes;reference.endpoint=reference_notes;
  Fixture output(*s,"Live receiving synth",false,false),reference_output(*s,"Reference receiving synth",false,false);
  auto bridge=std::make_shared<MidiBridge>(*s,source,block,true),reference_bridge=std::make_shared<MidiBridge>(*s,reference,block,false);
  need(output.route->add_processor(bridge,PreFader)==0&&reference_output.route->add_processor(reference_bridge,PreFader)==0,"MIDI bridges");
  bridge->check_position=reference_bridge->check_position=true;
  for(auto* f:{&output,&reference_output}){
   auto synth=find_plugin(*s,"https://community.ardour.org/node/7596",LV2);need(bool(synth),"synth");
   auto insert=std::make_shared<PluginInsert>(*s,*f->route,synth);need(f->route->add_processor(insert,PreFader)==0,"live synth");insert->activate();
   for(unsigned j=0;j<4;++j){auto effect=find_plugin(*s,"urn:ardour:a-delay",LV2);need(bool(effect),"delay");auto fx=std::make_shared<PluginInsert>(*s,*f->route,effect);need(f->route->add_processor(fx,PreFader)==0,"live delay");fx->activate();
    for(auto p:std::vector<std::pair<unsigned,double>>{{5,13.+7*j},{7,70},{8,55}})fx->automation_control(Evoral::Parameter(PluginAutomation,0,p.first))->set_value(p.second,PBD::Controllable::NoGroup);
   }
  }
  auto tap=std::make_shared<Tap>(*s,block),reference_tap=std::make_shared<Tap>(*s,block);
  need(output.route->add_processor(tap,PostFader)==0&&reference_output.route->add_processor(reference_tap,PostFader)==0,"captures");s->resort_routes();
  engine->set_session(s);bool ready=false;const auto setup_deadline=Clock::now()+std::chrono::seconds(5);
  while(!ready&&Clock::now()<setup_deadline){{PBD::Mutex::Lock lock(engine->process_lock());ready=s->setup_settled()&&!s->global_locate_pending();}if(!ready)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  engine->set_session(nullptr);need(ready,"MIDI setup incomplete");
  bridge->start_worker();while(bridge->write.load()<bridge->capacity)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  s->request_roll();engine->set_session(s);const auto deadline=Clock::now()+std::chrono::seconds(20);
  while(!bridge->done.load()&&Clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  engine->set_session(nullptr);bridge->stop_worker();
  need(bridge->done&&tap->k==total_blocks&&reference_tap->k==total_blocks,"MIDI callbacks incomplete");
  const std::array<samplepos_t,4> expected{{400*block+7,850*block+29,950*block+3,1250*block+5}};
  need(notes->events==4&&reference_notes->events==4&&bridge->events==4&&reference_bridge->events==4&&bridge->delivered==expected&&reference_bridge->delivered==expected,"MIDI delivery time mismatch");
  for(unsigned i=0;i<tap->captured.size();++i){error=std::max(error,double(std::abs(tap->captured[i]-reference_tap->captured[i])));if(i>tap->captured.size()-256*block)tail=std::max(tail,double(std::abs(tap->captured[i])));}
  peak=tap->peak;misses=bridge->misses;late=bridge->late;cutover=bridge->cutover;
  std::cerr<<"MIDI BOUNDARY events="<<bridge->events<<" cutover="<<cutover<<" error="<<error<<" peak="<<peak<<" tail="<<tail<<std::endl;
  need(error<1e-7&&misses==0&&peak>1e-4&&tail<peak*.05,"MIDI boundary audio mismatch");
 }
 delete s;engine->stop();PBD::microseconds_t minimum,maximum;double average,deviation;
 need(engine->dsp_stats[AudioEngine::ProcessCallback].get_stats(minimum,maximum,average,deviation),"timing");
 std::cout<<"RESULT {\"passed\":true,\"normal_session_callback\":true,\"blocks\":["<<block<<"],\"midi_events\":4,\"live_synth_consumer\":true,\"monitor_cutover\":"<<cutover<<",\"max_sample_error\":"<<error<<",\"cache_underruns\":"<<misses<<",\"bridge_deadline_overruns\":"<<late<<",\"engine_mean_us\":"<<average<<",\"engine_max_us\":"<<maximum<<",\"hardware_latency_measured\":false}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
