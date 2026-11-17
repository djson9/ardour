// Feasibility probe, not an audio engine implementation.
#include <atomic>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>
#include <stdexcept>
#include <glib.h>
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
#include "test_ui.h"
using namespace ARDOUR;
static void need(bool b, const char* s) { if (!b) throw std::runtime_error(s); }

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
struct RouteAccess : Route { static auto bounce() { return &RouteAccess::bounce_process; } };
struct Fixture {
 std::shared_ptr<Route> route;std::shared_ptr<Probe> probe;
 Fixture(Session& s,const char* name){
  route=s.new_audio_route(2,2,{},1,name,PresentationInfo::AudioBus,0).front();
  probe=std::make_shared<Probe>(s);need(route->add_processor(probe,PreFader)==0,"add processor");
 }
 void run(BufferSet& b,long long t,unsigned n){
  b.silence(n,0);(route.get()->*RouteAccess::bounce())(b,t,n,probe,true,true,false);
 }
};
static void prepare(BufferSet& b,unsigned n){b.ensure_buffers(DataType::AUDIO,2,n);b.set_count(ChanCount(DataType::AUDIO,2));}
int main(int argc,char**argv){
 try {
  need(argc==2,"session path argument required");need(std::string(g_get_home_dir())==getenv("ARDOUR_PROBE_HOME"),"preferences isolation failed");need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();
  auto ui=std::make_unique<TestUI>();auto* e=AudioEngine::create();auto backend=e->set_backend("None (Dummy)","AnticipationProbe","");
  need(bool(backend),"dummy backend");need(backend->set_sample_rate(48000)==0,"sample rate");
  need(e->start()==0,"start");
  auto* s=new Session(*e,argv[1],"anticipation-probe",nullptr,"",true);
  // Session is intentionally NOT attached to the hardware callback. The dummy
  // backend supplies port services; only this harness processes these routes.
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
   std::thread producer([&]{ProcessThread pt;pt.get_buffers();BufferSet b;prepare(b,block);
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
  std::cout<<"RESULT {\"passed\":true,\"blocks\":[64,128],\"route_cache_max_error\":"<<max_error<<",\"mixed_output_max_error\":"<<live_error<<",\"naive_handoff_max_error\":"<<naive_error<<",\"replay_handoff_max_error\":"<<replay_error<<",\"internal_send_overwrite\":"<<send_overwrite<<",\"historical_return_error\":"<<stale_return_error<<",\"concurrent_instance_calls\":"<<total_races<<",\"render_position\":"<<observed_start<<",\"session_position\":"<<observed_session<<",\"realtime_benchmark\":false}"<<std::endl;
  main_thread.drop_buffers();
  }
  delete s;e->stop();ui.reset();ARDOUR::cleanup();return 0;
 }catch(const std::exception&x){std::cerr<<"FAIL "<<x.what()<<std::endl;return 1;}
}
