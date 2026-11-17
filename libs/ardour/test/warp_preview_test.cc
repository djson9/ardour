#include "ardour/warp_preview.h"
#include <cassert>
#include <cmath>
#include <thread>
#include <cstdlib>
#include <new>
#include <cstdio>
static thread_local bool realtime=false;
void* operator new(std::size_t n) { assert(!realtime);if(void*p=malloc(n))return p;throw std::bad_alloc(); }
void operator delete(void*p) noexcept { assert(!realtime);free(p); }
void operator delete(void*p,std::size_t) noexcept { assert(!realtime);free(p); }
using ARDOUR::WarpPreview;
static std::vector<std::vector<float>> pcm(float value) { return std::vector<std::vector<float>>(2,std::vector<float>(4096,value)); }
int main(){
 WarpPreview p;float left[128],right[128];
 auto run=[&](int start,bool forward=true){
  std::fill_n(left,128,0.f);std::fill_n(right,128,0.f);
  realtime=true;p.begin(start,128,forward,256);p.apply(left,0);p.apply(right,1);p.end();realtime=false;
  for(int i=0;i<128;++i){assert(left[i]==right[i]);assert(std::isfinite(left[i]));}
 };
 run(0);assert(left[127]==0);
 auto old=p.request();auto generation=p.request();assert(!p.publish(old,0,pcm(9)));
 assert(p.publish(generation,0,pcm(1)));run(0);assert(left[0]==1.f/256&&left[127]==.5f);
 run(128);assert(left[127]==1);run(256);assert(left[0]==1);
 generation=p.request();assert(p.publish(generation,0,pcm(2)));run(384);assert(left[0]==1+1.f/256&&left[127]==1.5f);
 run(512);assert(left[127]==2);assert(p.cancel());run(640);assert(left[127]==1);run(768);assert(left[127]==0);
 assert(p.publish(p.request(),0,pcm(3)));run(0,false);assert(left[0]==0);run(0);run(128);assert(left[127]==3);
 // Future windows cannot replace current audio before they cover playback.
 assert(p.publish(p.request(),1024,pcm(4)));run(256);assert(left[127]==3);run(1024);assert(left[127]==3.5f);run(1152);assert(left[127]==4);
 // A published window carries the end of a loop followed by its beginning.
 WarpPreview looped;auto window=pcm(2);for(auto& ch:window)std::fill_n(ch.begin(),100,1.f);
 assert(looped.publish(looped.request(),900,std::move(window),0,1000));
 std::fill_n(left,128,0.f);realtime=true;looped.begin(950,128,true,1);looped.apply(left,0);looped.end();realtime=false;
 assert(left[0]==1 && left[49]==1 && left[50]==2);
 std::fill_n(left,128,0.f);realtime=true;looped.begin(78,128,true,1);looped.apply(left,0);looped.end();realtime=false;
 assert(left[0]==2 && left[127]==2 && looped.statistics().missed_windows==0);
 // Race publication against audio callbacks. No allocation/free is permitted
 // in begin/apply/end, including retirement and cancelled generations.
 WarpPreview concurrent;std::atomic<bool> done{false};
 std::thread producer([&]{for(int i=0;i<10000;++i){auto g=concurrent.request();concurrent.publish(g,0,pcm(float(i%8)));if(i%7==0)concurrent.cancel();}done=true;});
 unsigned blocks=0;
 while(!done.load()||blocks<10000){std::fill_n(left,128,0.f);realtime=true;concurrent.begin(0,128,true,64);concurrent.apply(left,0);concurrent.end();realtime=false;for(float f:left)assert(std::isfinite(f)&&f>=0&&f<=7);++blocks;}
 producer.join();printf("PASS: crossfades, stale generations, cancellation, timeline coverage, stereo, %u concurrent callbacks, no audio-thread allocation/free\n",blocks);
}
