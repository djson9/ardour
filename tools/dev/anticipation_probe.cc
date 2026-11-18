// Feasibility probe, not an audio engine implementation.
#include <atomic>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>
#include <stdexcept>
#include <glib.h>
#include "temporal/tempo.h"
#include "ardour/ardour.h"
#include "ardour/audioengine.h"
#include "ardour/audio_backend.h"
#include "ardour/session.h"
#include "ardour/route.h"
#include "ardour/processor.h"
#include "ardour/process_thread.h"
#include "ardour/buffer_set.h"
#include "ardour/audio_buffer.h"
#include "ardour/internal_send.h"
#include "ardour/internal_return.h"
#include "ardour/plugin.h"
#include "ardour/plugin_insert.h"
#include "ardour/io.h"
#include "ardour/audioplaylist.h"
#include "ardour/playlist_factory.h"
#include "ardour/source_factory.h"
#include "ardour/sndfilesource.h"
#include "ardour/region_factory.h"
#include "ardour/region.h"
#include "ardour/automation_list.h"
#include "ardour/midi_buffer.h"
#include "ardour/audio_track.h"
#include "ardour/monitor_control.h"
#include "ardour/rc_configuration.h"
#include "ardour/disk_reader.h"
#include "ardour/butler.h"
#include "ardour/amp.h"
#include "ardour/pannable.h"
#include "ardour/port.h"
#include "test_ui.h"
using namespace ARDOUR;
static void need(bool b, const char* s) { if (!b) throw std::runtime_error(s); }
struct FixtureSession : Session {
 using Session::Session;
 // Inspect only while holding the engine process lock. Locate completion alone
 // does not mean deferred startup buffer-adjustment/stop events have finished.
 bool setup_settled(){return pending_events.read_space()==0&&immediate_events.empty()&&!butler()->transport_work_requested()&&!locate_pending();}
};

// A deliberately stateful in-place processor: signal depends on supplied time
// AND recursive history. Its class is hosted by the real Ardour Route chain.
struct Probe : Processor {
 double history=0; std::atomic<int> entered{0}, races{0};
 std::atomic<long long> last_start{0}, last_session{0};
 Probe(Session& s):Processor(s,"Anticipation probe",s){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out) override {out=in;return true;}
 void run(BufferSet& b,samplepos_t start,samplepos_t,double,pframes_t n,bool) override {
  if(entered.fetch_add(1)) ++races;
  last_start=start;last_session=_session.transport_sample();
  for(unsigned i=0;i<n;++i){history=.997*history+.003*std::sin((start+i)*.017);
   for(unsigned c=0;c<b.count().n_audio();++c)b.get_audio(c).data()[i]=history;}
  --entered;
 }
};
struct RouteAccess : Route { static auto bounce() { return &RouteAccess::render_process; } };
struct Fixture {
 std::shared_ptr<Route> route;std::shared_ptr<Probe> probe;std::shared_ptr<Processor> endpoint;
 std::function<void(BufferSet&,samplepos_t,unsigned)> render_override;
 Fixture(Session& s,const char* name,bool real_plugins=false,bool generator=true,bool registered=true,unsigned inserts=4,bool audio_track=false){
  if(registered)route=s.new_audio_route(2,2,{},1,name,PresentationInfo::AudioBus,0).front();
  else {
   if(audio_track)route=std::make_shared<AudioTrack>(s,name,Normal);
   else route=std::make_shared<Route>(s,name,PresentationInfo::AudioBus,DataType::AUDIO);
   need(route->init()==0,"private route init");
   {PBD::Mutex::Lock lock(AudioEngine::instance()->process_lock());
    need(route->input()->ensure_io(ChanCount(DataType::AUDIO,2),false)==0,"private input");
    need(route->output()->ensure_io(ChanCount(DataType::AUDIO,2),false)==0,"private output");}
   route->add_internal_return();
  }
  if(generator){probe=std::make_shared<Probe>(s);need(route->add_processor(probe,PreFader)==0,"add processor");endpoint=probe;}
  else endpoint=route->internal_return();
  if(real_plugins) for(unsigned k=0;k<inserts;++k){
   auto p=find_plugin(s,"urn:ardour:a-delay",LV2);need(bool(p),"find actual LV2 delay");
   auto insert=std::make_shared<PluginInsert>(s,*route,p);
   need(route->add_processor(insert,PreFader)==0,"add actual plugin");insert->activate();endpoint=insert;
   for(auto parameter:std::vector<std::pair<unsigned,double>>{{5,13.0+7*k},{7,70},{8,55}})
    insert->automation_control(Evoral::Parameter(PluginAutomation,0,parameter.first))->set_value(parameter.second,PBD::Controllable::NoGroup);
   if(getenv("ARDOUR_PROBE_AUTOMATION")){
    auto control=insert->automation_control(Evoral::Parameter(PluginAutomation,0,7));
    for(auto point:std::vector<std::pair<samplepos_t,double>>{{0,70},{2400,20},{7200,85},{24000,45},{48000,95},{96000,30},{192000,65}})
     control->alist()->add(timepos_t(point.first),point.second,false,false);
    control->alist()->set_automation_state(Play);
   }
  }
 }
 void run(BufferSet& b,long long t,unsigned n){
  if(render_override){render_override(b,t,n);return;}
  auto track=std::dynamic_pointer_cast<AudioTrack>(route);
  if(!track||!(track->monitoring_state()&MonitoringInput))b.silence(n,0);
  (route.get()->*RouteAccess::bounce())(b,t,n,endpoint,true,true,false);
 }
};
static void prepare(BufferSet& b,unsigned n){b.ensure_buffers(DataType::AUDIO,2,n);b.ensure_buffers(DataType::MIDI,1,8192);b.set_count(ChanCount(DataType::AUDIO,2));}

struct MidiNotes : Processor {
 unsigned block,events=0,calls=0;
 MidiNotes(Session& s,unsigned n):Processor(s,"Held MIDI notes",s),block(n){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;out.set(DataType::MIDI,1);return true;}
 void run(BufferSet& b,samplepos_t start,samplepos_t,double,pframes_t n,bool)override{
  need(start==samplepos_t(calls*block),"MIDI cursor discontinuity");++calls;
  auto count=b.count();count.set(DataType::MIDI,1);b.set_count(count);auto& midi=b.get_midi(0);midi.clear();
  const samplepos_t positions[]={400*block+7,850*block+29,950*block+3,1250*block+5};
  const uint8_t messages[][3]={{0x90,60,100},{0x80,60,0},{0x90,67,100},{0x80,67,0}};
  for(unsigned e=0;e<4;++e)if(positions[e]>=start&&positions[e]<start+n){
   need(midi.push_back(positions[e]-start,Evoral::MIDI_EVENT,3,messages[e]),"MIDI event buffer full");++events;
  }
 }
};
static std::shared_ptr<MidiNotes> use_midi_synth(Session& s,Fixture& f,unsigned block){
 auto notes=std::make_shared<MidiNotes>(s,block);need(f.route->replace_processor(f.probe,notes)==0,"MIDI source");
 auto plugin=find_plugin(s,"https://community.ardour.org/node/7596",LV2);need(bool(plugin),"reasonable synth");
 auto insert=std::make_shared<PluginInsert>(s,*f.route,plugin);
 std::shared_ptr<Processor> first_fx;f.route->foreach_processor([&](std::weak_ptr<Processor> weak){auto p=weak.lock();if(!first_fx&&std::dynamic_pointer_cast<PluginInsert>(p))first_fx=p;});
 need(bool(first_fx)&&f.route->add_processor(insert,first_fx)==0,"synth before effects");insert->activate();return notes;
}

// Real file/region/playlist input for timing-context experiments. Direct playlist
// reads can block on disk and are suitable for the producer, not a proposed
// monitored-input implementation. Scratch storage is allocated before playback.
struct MonitorRange {std::atomic<samplepos_t> from{max_samplepos},until{max_samplepos};};
struct DiskInput : Processor {
 std::shared_ptr<AudioPlaylist> playlist;std::vector<Sample> mix;std::vector<float> gain;
 std::shared_ptr<MonitorRange> monitor_range;std::vector<Sample> monitor_input;samplepos_t captured_start=-1;
 unsigned reads=0,monitor_blocks=0;double monitor_peak=0;
 DiskInput(Session& s,std::shared_ptr<AudioPlaylist> p):Processor(s,"Playlist input",s),playlist(p),mix(8192),gain(8192),monitor_input(2*8192){}
 bool can_support_io_configuration(const ChanCount& in,ChanCount& out)override{out=in;return true;}
 void capture_monitor_input(const BufferSet& b,samplepos_t start,unsigned n){
  need(n<=8192&&b.count().n_audio()==2,"monitor input size");captured_start=start;
  for(unsigned c=0;c<2;++c)std::copy_n(b.get_audio(c).data(),n,monitor_input.begin()+c*8192);
 }
 void run(BufferSet& b,samplepos_t start,samplepos_t,double,pframes_t n,bool)override{
  need(n<=mix.size(),"playlist scratch size");
  if(monitor_range&&start>=monitor_range->from.load(std::memory_order_acquire)&&start<monitor_range->until){
   need(captured_start==start,"stale monitored input timestamp");
   for(unsigned c=0;c<2;++c){b.get_audio(c).read_from(monitor_input.data()+c*8192,n);for(unsigned i=0;i<n;++i)monitor_peak=std::max(monitor_peak,double(std::abs(monitor_input[c*8192+i])));}
   ++monitor_blocks;return;
  }
  for(unsigned c=0;c<b.count().n_audio();++c)
   need(playlist->read(b.get_audio(c).data(),mix.data(),gain.data(),timepos_t(start),timecnt_t(n),0).samples()==n,"playlist read");
  ++reads;
 }
};
static std::shared_ptr<AudioPlaylist> make_disk_playlist(Session& s,const std::string& path){
 auto source=SourceFactory::createWritable(DataType::AUDIO,s,path,48000);
 auto snd=std::dynamic_pointer_cast<SndFileSource>(source);need(bool(snd),"file source");
 std::vector<Sample> samples(48000*10);
 for(unsigned i=0;i<samples.size();++i)samples[i]=.1*std::sin(i*.017)+.03*std::cos(i*.003);
 need(snd->write(samples.data(),samples.size())==samples.size(),"write source WAV");
 PBD::PropertyList properties;properties.add(Properties::start,timepos_t(0));properties.add(Properties::length,timecnt_t(samples.size()));
 auto region=RegionFactory::create(source,properties);need(bool(region),"audio region");
 auto playlist=std::dynamic_pointer_cast<AudioPlaylist>(PlaylistFactory::create(DataType::AUDIO,s,"Disk fixture"));
 need(bool(playlist),"audio playlist");playlist->add_region(region,timepos_t(0));return playlist;
}
static std::shared_ptr<DiskInput> use_disk_input(Session& s,Fixture& fixture,std::shared_ptr<AudioPlaylist> playlist){
 auto input=std::make_shared<DiskInput>(s,playlist);need(fixture.route->replace_processor(fixture.probe,input)==0,"replace generated source with playlist");return input;
}
static std::shared_ptr<AudioTrack> use_real_disk_reader(Fixture& f,std::shared_ptr<AudioPlaylist> playlist){
 auto track=std::dynamic_pointer_cast<AudioTrack>(f.route);need(bool(track),"real audio track");
 need(track->remove_processor(f.probe)==0,"remove synthetic input");
 need(track->use_playlist(DataType::AUDIO,playlist)==0,"assign actual track playlist");
 track->monitoring_control()->set_value(MonitorDisk,PBD::Controllable::NoGroup);
 need(track->seek(0,true)==0,"prefill actual DiskReader");return track;
}
int main(int argc,char**argv){
 try {
  need(argc==2,"session path argument required");need(std::string(g_get_home_dir())==getenv("ARDOUR_PROBE_HOME"),"preferences isolation failed");need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();
  auto ui=std::make_unique<TestUI>();auto* e=AudioEngine::create();auto backend=e->set_backend("None (Dummy)","AnticipationProbe","");
  need(bool(backend),"dummy backend");need(backend->set_sample_rate(48000)==0,"sample rate");
  need(e->start()==0,"start");
  auto* s=new Session(*e,argv[1],"anticipation-probe",nullptr,"",true);
  // Session construction attaches itself to the engine. Explicitly detach
  // before creating fixture routes, otherwise the dummy callback races them.
  e->set_session(nullptr);need(e->session()==nullptr,"fixture session still attached");
  // The dummy backend now supplies only port services.
  {
  Fixture direct(*s,"Direct"),ahead(*s,"Ahead"),live(*s,"Live"),live_ref(*s,"LiveRef"),handoff_ref(*s,"HandoffRef");
  ProcessThread main_thread;main_thread.get_buffers();
  double max_error=0,live_error=0,naive_error=0,replay_error=0;
  int total_races=0;long long observed_start=0,observed_session=0;
  for(unsigned block:{64u,128u}){
   direct.probe->history=ahead.probe->history=live.probe->history=live_ref.probe->history=handoff_ref.probe->history=0;
   constexpr unsigned blocks=400,capacity=96;
   struct Slot{std::vector<float> samples;};std::vector<Slot> q(capacity);
   for(auto&slot:q)slot.samples.resize(block);
   std::atomic<unsigned> read{0},write{0};std::atomic<bool> ready{false};
   std::thread producer([&]{Temporal::TempoMap::fetch();ProcessThread pt;pt.get_buffers();BufferSet b;prepare(b,block);
    for(unsigned k=0;k<blocks;++k){while(k-read.load(std::memory_order_acquire)>=capacity)std::this_thread::yield();
     ahead.run(b,k*block,block);std::copy_n(b.get_audio(0).data(),block,q[k%capacity].samples.begin());
     write.store(k+1,std::memory_order_release);if(k==capacity-1)ready=true;}
    pt.drop_buffers();});
   while(!ready)std::this_thread::yield();
   BufferSet d,l,lr;prepare(d,block);prepare(l,block);prepare(lr,block);
   for(unsigned k=0;k<blocks;++k){
    // Waiting is permitted ONLY in this deterministic correctness harness.
    // This is explicitly not a hardware deadline benchmark.
    while(write.load(std::memory_order_acquire)<=k)std::this_thread::yield();
    direct.run(d,k*block,block);live.run(l,k*block,block);live_ref.run(lr,k*block,block);
    for(unsigned i=0;i<block;++i){
     max_error=std::max(max_error,double(std::abs(d.get_audio(0).data()[i]-q[k%capacity].samples[i])));
     // A mixed output oracle, not a claim that InternalReturn was exercised.
     live_error=std::max(live_error,double(std::abs((q[k%capacity].samples[i]+l.get_audio(0).data()[i])-(d.get_audio(0).data()[i]+lr.get_audio(0).data()[i]))));
    }
    read.store(k+1,std::memory_order_release);
   }
   producer.join();observed_start=ahead.probe->last_start;observed_session=ahead.probe->last_session;
   // Producer state is at 400 blocks; audible state is at 300. Same instance,
   // serialized ownership, but its recursive history belongs to the future.
   BufferSet reference;prepare(reference,block);
   for(unsigned k=0;k<300;++k)handoff_ref.run(reference,k*block,block);
   handoff_ref.run(reference,300*block,block);ahead.run(d,300*block,block);
   for(unsigned i=0;i<block;++i)naive_error=std::max(naive_error,double(std::abs(d.get_audio(0).data()[i]-reference.get_audio(0).data()[i])));
   // Exact replay from a known initial state demonstrates what state recovery
   // requires; it does NOT establish bounded arbitrary-plugin replay cost.
   ahead.probe->history=0;
   for(unsigned k=0;k<=300;++k)ahead.run(d,k*block,block);
   for(unsigned i=0;i<block;++i)replay_error=std::max(replay_error,double(std::abs(d.get_audio(0).data()[i]-reference.get_audio(0).data()[i])));
   total_races+=ahead.probe->races+live.probe->races;
  }
  // Real internal-send experiment: ordinary send data has one current buffer,
  // so publishing a future block overwrites the earlier range.
  auto senders=std::make_shared<RouteList>();senders->push_back(ahead.route);
  s->add_internal_sends(live.route,PostFader,senders);
  auto send=std::dynamic_pointer_cast<InternalSend>(ahead.route->internal_send_for(live.route));need(bool(send),"internal send");send->gain_control()->set_value(1.0,PBD::Controllable::NoGroup);send->activate();
  BufferSet b;prepare(b,128);ahead.run(b,0,128);send->cycle_start(128);send->run(b,0,128,1,128,true);
  std::vector<float> previous(128);std::copy_n(send->get_buffers().get_audio(0).data(),128,previous.begin());
  ahead.run(b,128,128);send->cycle_start(128);send->run(b,128,256,1,128,true);
  double send_overwrite=0;for(unsigned i=0;i<128;++i)send_overwrite=std::max(send_overwrite,double(std::abs(previous[i]-send->get_buffers().get_audio(0).data()[i])));
  BufferSet returned;prepare(returned,128);returned.silence(128,0);
  live.route->internal_return()->run(returned,0,128,1,128,true);
  double stale_return_error=0;for(unsigned i=0;i<128;++i)stale_return_error=std::max(stale_return_error,double(std::abs(previous[i]-returned.get_audio(0).data()[i])));
  need(stale_return_error>1e-4,"return unexpectedly retained historical send");
  need(max_error<1e-6&&live_error<1e-6&&replay_error<1e-6,"equivalence failed");
  need(naive_error>1e-4,"naive handoff unexpectedly equal");need(send_overwrite>1e-4,"send overwrite not observed");need(total_races==0,"concurrent instance access");
  // Real LV2 processing through PluginInsert, with a single instance owner.
  // Match REAPER's observed arm transition: stop producing, drain the queue,
  // and transfer the same chain at its existing render cursor. No rewind.
  Fixture fx_direct(*s,"FX reference",true),fx_ahead(*s,"FX ahead",true);
  Fixture bus_direct(*s,"Bus reference",true,false),bus_live(*s,"Bus live",true,false);
  Fixture live_source(*s,"Live source"),live_source_ref(*s,"Live source reference");
  auto connect=[&](Fixture& source,Fixture& dest){
   auto list=std::make_shared<RouteList>();list->push_back(source.route);s->add_internal_sends(dest.route,PostFader,list);
   auto send=std::dynamic_pointer_cast<InternalSend>(source.route->internal_send_for(dest.route));need(bool(send),"bus send");
   send->gain_control()->set_value(.37,PBD::Controllable::NoGroup);send->activate();return send;
  };
  auto cached_send=connect(fx_ahead,bus_live),reference_send=connect(fx_direct,bus_direct);
  auto live_send=connect(live_source,bus_live),live_reference_send=connect(live_source_ref,bus_direct);
  double fx_error=0,fx_effect=0,bus_error=0,bus_peak=0;unsigned drained=0;
  for(unsigned block:{64u,128u}){
   constexpr unsigned count=600,capacity=150,request=200;
   struct Slot {std::vector<float> samples;};std::vector<Slot> q(capacity);
   for(auto& slot:q)slot.samples.resize(2*block);
   std::atomic<unsigned> read{0},write{0};std::atomic<bool> stop{false},done{false};
   std::thread producer([&]{Temporal::TempoMap::fetch();ProcessThread pt;pt.get_buffers();BufferSet b;prepare(b,block);
    for(unsigned k=0;k<count&&!stop.load();++k){
     while(k-read.load(std::memory_order_acquire)>=capacity&&!stop.load())std::this_thread::yield();
     if(stop.load())break;
     fx_ahead.run(b,k*block,block);
     for(unsigned c=0;c<2;++c)std::copy_n(b.get_audio(c).data(),block,q[k%capacity].samples.begin()+c*block);
     write.store(k+1,std::memory_order_release);
    }
    pt.drop_buffers();done.store(true,std::memory_order_release);
   });
   while(write.load()<capacity)std::this_thread::yield();
   BufferSet reference,actual,live,live_ref,bus,bus_ref;
   for(auto* buffer:{&reference,&actual,&live,&live_ref,&bus,&bus_ref})prepare(*buffer,block);
   unsigned transfer=count;
   for(unsigned k=0;k<count;++k){
    if(k==request){stop=true;producer.join();transfer=write.load();drained+=transfer-k;}
    fx_direct.run(reference,k*block,block);
    if(k<transfer){
     while(write.load(std::memory_order_acquire)<=k)std::this_thread::yield();
     for(unsigned c=0;c<2;++c)std::copy_n(q[k%capacity].samples.begin()+c*block,block,actual.get_audio(c).data());
     read.store(k+1,std::memory_order_release);
    }else fx_ahead.run(actual,k*block,block);
    if(fx_error==0&&std::abs(actual.get_audio(0).data()[0]-reference.get_audio(0).data()[0])>1e-6)
     std::cerr<<"FIRST mismatch block="<<block<<" k="<<k<<" transfer="<<transfer<<" actual="<<actual.get_audio(0).data()[0]<<" reference="<<reference.get_audio(0).data()[0]<<std::endl;
    for(unsigned c=0;c<2;++c)for(unsigned i=0;i<block;++i)
     fx_error=std::max(fx_error,double(std::abs(actual.get_audio(c).data()[i]-reference.get_audio(c).data()[i])));
    fx_effect=std::max(fx_effect,std::abs(double(actual.get_audio(0).data()[block-1])-fx_direct.probe->history));
    // Replay ordinary post-FX sends at consumption time. Their state and the
    // destination's InternalReturn stay on the live side; only expensive FX
    // run ahead. This needs no historical InternalSend buffer or second Graph.
    live_source.run(live,k*block,block);live_source_ref.run(live_ref,k*block,block);
    auto deliver=[&](std::shared_ptr<InternalSend> send,BufferSet& b){send->cycle_start(block);send->run(b,k*block,(k+1)*block,1,block,true);};
    deliver(cached_send,actual);deliver(reference_send,reference);
    deliver(live_send,live);deliver(live_reference_send,live_ref);
    bus_live.run(bus,k*block,block);bus_direct.run(bus_ref,k*block,block);
    for(unsigned c=0;c<2;++c)for(unsigned i=0;i<block;++i){
     bus_error=std::max(bus_error,double(std::abs(bus.get_audio(c).data()[i]-bus_ref.get_audio(c).data()[i])));
     bus_peak=std::max(bus_peak,double(std::abs(bus.get_audio(c).data()[i])));
    }
   }
   need(done.load(),"producer not handed off");
  }
  std::cerr<<"FX diagnostic error="<<fx_error<<" bus_error="<<bus_error<<" bus_peak="<<bus_peak<<std::endl;
  need(fx_error<1e-6,"actual plugin drain handoff mismatch");need(fx_effect>1e-4,"actual plugin did not process");
  need(bus_error<1e-6&&bus_peak>1e-4,"cached plus live through actual bus mismatch or silent");
  std::cout<<"REAL_PLUGIN_RESULT {\"plugin\":\"urn:ardour:a-delay\",\"inserts_per_route\":4,\"blocks\":[64,128],\"cache_and_drain_handoff_error\":"<<fx_error<<",\"effect_vs_dry_difference\":"<<fx_effect<<",\"drained_blocks\":"<<drained<<",\"cached_and_live_bus_error\":"<<bus_error<<",\"bus_peak\":"<<bus_peak<<",\"realtime_benchmark\":false}"<<std::endl;
  std::cout<<"RESULT {\"passed\":true,\"blocks\":[64,128],\"route_cache_max_error\":"<<max_error<<",\"mixed_output_max_error\":"<<live_error<<",\"naive_handoff_max_error\":"<<naive_error<<",\"replay_handoff_max_error\":"<<replay_error<<",\"internal_send_overwrite\":"<<send_overwrite<<",\"historical_return_error\":"<<stale_return_error<<",\"concurrent_instance_calls\":"<<total_races<<",\"render_position\":"<<observed_start<<",\"session_position\":"<<observed_session<<",\"realtime_benchmark\":false}"<<std::endl;
  main_thread.drop_buffers();
  }
  delete s;e->stop();ui.reset();ARDOUR::cleanup();return 0;
 }catch(const std::exception&x){std::cerr<<"FAIL "<<x.what()<<std::endl;return 1;}
}
