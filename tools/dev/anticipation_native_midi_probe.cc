#define ANTICIPATION_NATIVE_HELPERS_ONLY
#include "anticipation_native_route_probe.cc"
#include "ardour/midi_track.h"

struct NativeMidiNotes : Processor {
 unsigned block,notes=0;std::atomic<unsigned> immediate{0};
 NativeMidiNotes(Session& s,unsigned n):Processor(s,"Native held-note source",s),block(n){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;out.set(DataType::MIDI,1);return true;}
 void run(BufferSet& b,samplepos_t start,samplepos_t,double speed,pframes_t n,bool)override{
  if(speed!=1)return;
  const MidiBuffer& input=b.get_midi(0);for(auto event:input)if(event.size()==3&&event.buffer()[0]==0xb0&&event.buffer()[1]==119&&event.buffer()[2]==55)++immediate;
  const samplepos_t positions[]={400*block+7,850*block+29,950*block+3,1250*block+5};
  const uint8_t messages[][3]={{0x90,60,100},{0x80,60,0},{0x90,67,100},{0x80,67,0}};
  for(unsigned i=0;i<4;++i)if(positions[i]>=start&&positions[i]<start+n){need(b.get_midi(0).push_back(positions[i]-start,Evoral::MIDI_EVENT,3,messages[i]),"MIDI capacity");++notes;}
 }
};
int main(int argc,char** argv){try{
 need(argc==2&&ARDOUR::init(true,"/tmp"),"init");Temporal::reset();if(getenv("ARDOUR_PROBE_RT_THREADS"))Config->set_processor_usage(std::stoi(getenv("ARDOUR_PROBE_RT_THREADS")));auto ui=std::make_unique<TestUI>();
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","NativeMidi","");
 const unsigned block=getenv("ARDOUR_PROBE_BLOCK")?std::stoul(getenv("ARDOUR_PROBE_BLOCK")):64;
 need(backend&&backend->set_driver("Realtime")==0&&backend->set_sample_rate(48000)==0&&backend->set_buffer_size(block)==0&&engine->start()==0,"engine");
 auto* s=new NativeSession(*engine,argv[1],"native-midi",nullptr,"",true);engine->set_session(nullptr);
 double error=0,peak=0;uint64_t misses=0,handoffs=0;unsigned delivered=0;
 {
  auto create=[&](const char* name){return s->new_midi_track(ChanCount(DataType::MIDI,1),ChanCount(DataType::AUDIO,2),false,{},nullptr,{},1,name,0,Normal,false).front();};
  auto a=create("Native synth"),reference=create("Normal synth");
  std::vector<std::shared_ptr<NativeMidiNotes>> sources;
  for(auto route:{a,reference}){
   auto source=std::make_shared<NativeMidiNotes>(*s,block);sources.push_back(source);need(route->add_processor(source,PreFader)==0,"note source");
   auto synth=find_plugin(*s,"https://community.ardour.org/node/7596",LV2);need(bool(synth),"synth");auto insert=std::make_shared<PluginInsert>(*s,*route,synth);need(route->add_processor(insert,PreFader)==0,"synth insert");insert->activate();add_effects(*s,route,false);
   route->monitoring_control()->set_value(MonitorDisk,PBD::Controllable::NoGroup);
  }
  auto sink=s->new_audio_route(2,2,{},1,"Native capture",PresentationInfo::AudioBus,0).front();auto rsink=s->new_audio_route(2,2,{},1,"Normal capture",PresentationInfo::AudioBus,0).front();
  auto tap=std::make_shared<NativeTap>(*s,block),rtap=std::make_shared<NativeTap>(*s,block);need(sink->add_processor(tap,PreFader)==0&&rsink->add_processor(rtap,PreFader)==0,"capture");
  {PBD::Mutex::Lock lock(engine->process_lock());for(auto edge:std::vector<std::pair<std::shared_ptr<Route>,std::shared_ptr<Route>>>{{a,sink},{reference,rsink}})for(unsigned c=0;c<2;++c)need(edge.second->input()->connect(edge.second->input()->nth(c),edge.first->output()->nth(c)->name())==0,"routing");}
  reference->set_anticipative_processing_disabled(true);sink->set_anticipative_processing_disabled(true);rsink->set_anticipative_processing_disabled(true);s->resort_routes();s->calculate_latency();
  auto wait=[&](auto predicate,const char* message){auto end=Clock::now()+std::chrono::seconds(10);while(Clock::now()<end){{PBD::Mutex::Lock lock(engine->process_lock());if(predicate())return;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}throw std::runtime_error(message);};
  engine->set_session(s);wait([&]{return s->setup_settled()&&!s->global_locate_pending();},"setup");engine->set_session(nullptr);
  for(auto route:{a,reference})route->foreach_processor([](std::weak_ptr<Processor> p){auto insert=std::dynamic_pointer_cast<PluginInsert>(p.lock());if(insert)for(unsigned i=0;i<insert->get_count();++i)insert->plugin(i)->flush();});
  s->set_anticipative_processing(true,200);s->request_roll();engine->set_session(s);bool armed=false,released=false,immediate=false;
  auto end=Clock::now()+std::chrono::seconds(20);
  while(Clock::now()<end){unsigned k;{PBD::Mutex::Lock lock(engine->process_lock());k=tap->k;}
   if(k>=total_blocks)break;
   if(!armed&&k>=arm_block){a->rec_enable_control()->set_value(1,PBD::Controllable::NoGroup);armed=true;}
   if(armed&&!released&&k>=demote_block){a->rec_enable_control()->set_value(0,PBD::Controllable::NoGroup);released=true;}
   if(!immediate&&k>=1500){uint8_t event[]={0xb0,119,55};need(a->write_user_immediate_event(Evoral::MIDI_EVENT,3,event),"immediate enqueue");immediate=true;}
   std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  engine->set_session(nullptr);auto& plan=s->anticipative_process();plan.stop();auto* node=plan.find_node(a.get());need(node&&tap->k==total_blocks&&rtap->k==total_blocks,"complete");
  for(unsigned i=0;i<tap->captured.size();++i)error=std::max(error,double(std::abs(tap->captured[i]-rtap->captured[i])));
  peak=tap->peak;misses=node->queue->underruns();handoffs=node->queue->live_handoffs();delivered=sources[0]->immediate.load();
  std::cerr<<"NATIVE MIDI error="<<error<<" notes="<<sources[0]->notes<<","<<sources[1]->notes<<" immediate="<<delivered<<" handoffs="<<handoffs<<" misses="<<misses<<" faults="<<node->queue->discontinuities()<<std::endl;
  need(error==0&&peak>1e-4&&sources[0]->notes==4&&sources[1]->notes==4&&delivered==1&&handoffs>=2&&misses==0,"native MIDI equivalence");plan.clear();
 }
 delete s;engine->stop();std::cout<<"RESULT {\"passed\":true,\"native_midi_track\":true,\"held_notes\":4,\"immediate_events_delivered_once\":"<<delivered<<",\"max_sample_error\":"<<error<<",\"cache_underruns\":"<<misses<<"}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(std::exception const& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
