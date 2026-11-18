// Deterministic contracts for the engine queue, including adversarial ownership.
#define ANTICIPATION_CALLBACK_HELPERS_ONLY
#include "anticipation_callback_probe.cc"
#include "ardour/anticipation_queue.h"
#include <future>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

using Queue=AnticipationQueue;
static void wait_for(std::function<bool()> ready){auto end=Clock::now()+std::chrono::seconds(5);while(!ready()&&Clock::now()<end)std::this_thread::sleep_for(std::chrono::microseconds(100));need(ready(),"wait timeout");}
static void buffers(BufferSet& b){b.ensure_buffers(DataType::AUDIO,2,64);b.ensure_buffers(DataType::MIDI,1,8192);ChanCount c(DataType::AUDIO,2);c.set(DataType::MIDI,1);b.set_count(c);}

int main(int argc,char** argv){try{
 need(argc==2,"path");need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();if(getenv("ARDOUR_PROBE_RT_THREADS"))Config->set_processor_usage(std::stoi(getenv("ARDOUR_PROBE_RT_THREADS")));auto ui=std::make_unique<TestUI>();
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","QueueContracts","");
 need(bool(backend)&&backend->set_driver(getenv("ARDOUR_PROBE_DRIVER")?getenv("ARDOUR_PROBE_DRIVER"):"Realtime")==0&&backend->set_sample_rate(48000)==0&&backend->set_buffer_size(64)==0&&engine->start()==0,"engine");
 auto* s=new FixtureSession(*engine,argv[1],"queue-contracts",nullptr,"",true);engine->set_session(nullptr);
 ChanCount count(DataType::AUDIO,2);count.set(DataType::MIDI,1);Queue::Configuration config{count,64,8,8192};
 unsigned checks=0;BufferSet out;buffers(out);

#ifdef __APPLE__
 // Pause only this fixture's idle worker, after an acknowledged handoff. This
 // makes scheduler starvation deterministic without adding production hooks.
 for(unsigned quantum : {64u,128u}) {
  std::atomic<thread_t> worker{MACH_PORT_NULL};std::atomic<unsigned> active{0},races{0};
  samplepos_t dsp_next=0;unsigned rendered=0;
  Queue q({count,quantum,8,8192},[&](BufferSet& b,Queue::Range const& r,bool live){
   if(!live)worker.store(pthread_mach_thread_np(pthread_self()));
   if(active.fetch_add(1))++races;
   need(r.continuous==dsp_next,"demotion DSP cursor skipped or duplicated");dsp_next+=r.frames;++rendered;
   b.get_midi(0).clear();
   for(unsigned i=0;i<r.frames;++i){
    for(unsigned c=0;c<2;++c)b.get_audio(c).data()[i]=float(r.continuous+i)/65536.f;
    if((r.continuous+i)%17==0){uint8_t event[]={0x90,60,90};need(b.get_midi(0).push_back(i,Evoral::MIDI_EVENT,3,event),"demotion MIDI capacity");}
   }
   --active;
  });
  auto& reader=q.add_reader();BufferSet current,peer;
  for(auto b:{&current,&peer}){b->ensure_buffers(DataType::AUDIO,2,quantum);b->ensure_buffers(DataType::MIDI,1,8192);b->set_count(count);}
  q.start({0,0,quantum},30);wait_for([&]{return q.primed();});
  samplepos_t cursor=0;
  auto consume=[&](bool live,Queue::Result expected){
   auto result=q.process(current,{cursor,cursor,quantum},live,30);
   need(result==expected,"unclaimed demotion must keep rendering live");
   for(unsigned c=0;c<2;++c)for(unsigned i=0;i<quantum;++i)need(current.get_audio(c).data()[i]==float(cursor+i)/65536.f,"demotion audio continuity");
   unsigned seen=0;for(auto e:current.get_midi(0)){need(e.time()>=0&&e.time()<quantum&&(cursor+e.time())%17==0,"demotion MIDI offset");++seen;}
   unsigned expected_events=0;for(unsigned i=0;i<quantum;++i)if((cursor+i)%17==0)++expected_events;
   need(seen==expected_events,"demotion MIDI lost or duplicated");cursor+=quantum;q.release_until(reader,cursor);
  };
  consume(true,Queue::Result::Cached);wait_for([&]{return q.state()==Queue::State::Drained;});
  while(q.queued_blocks())consume(true,Queue::Result::Cached);
  consume(true,Queue::Result::Live);
  struct PausedWorker {
   thread_t port;bool held=false;
   explicit PausedWorker(thread_t p):port(p){need(port!=MACH_PORT_NULL&&thread_suspend(port)==KERN_SUCCESS,"pause fixture worker");held=true;}
   void resume(){if(held){auto result=thread_resume(port);held=false;need(result==KERN_SUCCESS,"resume fixture worker");}}
   ~PausedWorker(){if(held)thread_resume(port);}
  } paused(worker.load());
  const unsigned before=rendered;auto began=Clock::now();
  for(unsigned i=0;i<1536/quantum;++i)consume(false,Queue::Result::Live);
  // A repeated live request can cancel an unclaimed offer without another drain.
  consume(true,Queue::Result::Live);consume(false,Queue::Result::Live);
  need(Clock::now()-began<std::chrono::milliseconds(20)&&rendered-before==1536/quantum+2&&q.underruns()==0,"scheduler delay must not wait or drop audio");
  paused.resume();wait_for([&]{return q.primed();});
  need(q.peek(reader,peer,{cursor,cursor,quantum},30)&&peer.get_audio(0).data()[0]==float(cursor)/65536.f,"future reader after deferred demotion");
  consume(false,Queue::Result::Cached);q.stop();
  need(races==0&&q.live_handoffs()==1&&q.discontinuities()==0,"exclusive deferred demotion");
  checks+=4;
 }
#endif
 {
  // A non-aligned loop and partial consumer calls preserve audio, event offsets
  // and the unwrapped render clock across reused project positions.
  std::atomic<unsigned> active{0},races{0};uint64_t calls=0;
  Queue q(config,[&](BufferSet& b,Queue::Range const& r,bool){
   if(active.fetch_add(1))++races;
   auto clock=ProcessThread::render_time();need(clock&&clock->continuous_at(r.project)==r.continuous,"clock");
   for(unsigned c=0;c<2;++c)for(unsigned i=0;i<r.frames;++i)b.get_audio(c).data()[i]=float(r.continuous+i)/65536.f;
   b.get_midi(0).clear();for(unsigned i=0;i<r.frames;++i)if((r.continuous+i)%17==0){uint8_t event[]={0x90,60,90};need(b.get_midi(0).push_back(i,Evoral::MIDI_EVENT,3,event),"event capacity");}
   ++calls;--active;
  },[](Queue::Range const& r){auto p=(r.project+r.frames)%157;return Queue::Range{p,r.continuous+r.frames,pframes_t(std::min<samplepos_t>(64,157-p))};});
  q.start({0,0,64},11);wait_for([&]{return q.primed();});
  samplepos_t t=0;unsigned events=0;
  for(unsigned cycle=0;cycle<700;++cycle){
   // This is a range/ownership oracle, not a wall-clock throughput test.
   // Wait in the harness for publication or drain acknowledgement; the separate
   // held-producer case below tests that process() itself never waits.
   wait_for([&]{auto state=q.state();return state==Queue::State::Live||state==Queue::State::Drained||(state==Queue::State::Ahead&&q.queued_blocks()>=2);});
   auto p=t%157;auto n=pframes_t(std::min<samplepos_t>({31,157-p,64}));
   auto result=q.process(out,{p,t,n},cycle>=100&&cycle<350,11);
   need(result==Queue::Result::Cached||result==Queue::Result::Live,"loop range consumption");
   for(unsigned c=0;c<2;++c)for(unsigned i=0;i<n;++i)need(out.get_audio(c).data()[i]==float(t+i)/65536.f,"partial audio");
   unsigned seen=0;for(auto it=out.get_midi(0).begin();it!=out.get_midi(0).end();++it){auto e=*it;if(!(e.time()>=0&&e.time()<n&&(t+e.time())%17==0))std::cerr<<"MIDI BAD cycle="<<cycle<<" t="<<t<<" n="<<n<<" event="<<e.time()<<" state="<<int(q.state())<<std::endl;need(e.time()>=0&&e.time()<n&&(t+e.time())%17==0,"MIDI offset");++seen;}
   unsigned expected=0;for(unsigned i=0;i<n;++i)if((t+i)%17==0)++expected;need(seen==expected,"missing/duplicate MIDI");events+=seen;t+=n;
   need(!ProcessThread::render_time(),"render scope leaked");
  }
  q.stop();need(races==0&&q.underruns()==0&&q.live_handoffs()==1&&events>100,"loop/handoff");checks+=4;
  // Generation mismatch is an explicit fault; start after stop is reusable.
  q.start({0,0,64},12);wait_for([&]{return q.primed();});
  need(q.process(out,{0,0,64},false,11)==Queue::Result::Discontinuity,"stale generation accepted");
  q.stop();q.start({0,0,64},13);wait_for([&]{return q.primed();});
  need(q.process(out,{0,0,64},false,13)==Queue::Result::Cached,"generation restart");q.stop();checks+=2;
 }
 {
  // Hold a producer inside DSP. The callback must return without acquiring the
  // producer's ownership; stop must wait on the control thread before mutation.
  std::atomic<bool> entered{false},release{false};std::atomic<unsigned> calls{0};
  Queue q(config,[&](BufferSet& b,Queue::Range const& r,bool){++calls;entered=true;while(!release.load())std::this_thread::sleep_for(std::chrono::microseconds(100));b.silence(r.frames,0);});
  q.start({0,0,64},1);wait_for([&]{return entered.load();});auto began=Clock::now();
  need(q.process(out,{0,0,64},true,1)==Queue::Result::Underrun,"missing block fallback");
  need(Clock::now()-began<std::chrono::milliseconds(20)&&calls==1,"callback waited or concurrent DSP");
  auto stopping=std::async(std::launch::async,[&]{q.stop();});need(stopping.wait_for(std::chrono::milliseconds(5))==std::future_status::timeout,"stop released live producer");
  release=true;need(stopping.wait_for(std::chrono::seconds(2))==std::future_status::ready,"stop did not join");stopping.get();checks+=3;
 }
 {
  Queue q(config,[](BufferSet& b,Queue::Range const& r,bool){
   b.get_midi(0).clear();for(unsigned c=0;c<2;++c)for(unsigned i=0;i<r.frames;++i)b.get_audio(c).data()[i]=float(r.continuous+i);
  });
  auto& fast=q.add_reader();auto& slow=q.add_reader();BufferSet peer;buffers(peer);
  q.start({0,0,64},20);wait_for([&]{return q.primed();});
  for(unsigned k=0;k<8;++k)need(q.process(out,{k*64,k*64,64},false,20)==Queue::Result::Cached,"consume retained block");
  need(q.peek(fast,peer,{0,0,64},20)&&peer.get_audio(0).data()[63]==63,"fast reader historical range");
  q.release_until(fast,512);
  need(q.process(out,{512,512,64},false,20)==Queue::Result::Underrun,"slow reader did not retain slots");
  need(q.peek(slow,peer,{0,0,64},20)&&peer.get_audio(0).data()[63]==63,"slow reader data overwritten");
  q.release_until(slow,512);wait_for([&]{return q.queued_blocks()>=2;});
  need(q.process(out,{576,576,64},false,20)==Queue::Result::Cached&&out.get_audio(0).data()[0]==576,"recover late data at audible cursor");
  need(!q.peek(slow,peer,{0,0,64},20),"released range still accepted");
  q.stop();checks+=4;
 }
 {
  Queue q(config,[](BufferSet&,Queue::Range const&,bool){throw std::runtime_error("simulated plugin failure");});
  q.start({0,0,64},1);wait_for([&]{return q.state()==Queue::State::Fault;});need(q.process(out,{0,0,64},false,1)==Queue::Result::Discontinuity,"worker exception not diagnosed");q.stop();++checks;
 }
 delete s;engine->stop();std::cout<<"RESULT {\"passed\":true,\"contract_checks\":"<<checks<<",\"audio_midi_partial_loop\":true,\"exclusive_handoff\":true,\"generation_reuse\":true,\"nonblocking_stalled_producer\":true,\"joined_before_mutation\":true}"<<std::endl;
 ui.reset();ARDOUR::cleanup();return 0;
}catch(std::exception const& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
