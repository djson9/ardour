/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../../../gtk2_ardour/automation_clip_curve.h"
#include <cassert>
#include <iostream>
using namespace AutomationClipCurve;
int main () {
 double worst = 0; size_t cases = 0;
 for (double bend : {-1., -.75, -.1, 0., .1, .75, 1.}) for (bool reverse : {false,true}) for (int64_t duration : {1,2,17,48000,384000}) {
  Point a {0, reverse?1.:0., bend}, b {duration, reverse?0.:1.,0};
  auto points = compile ({a,b});
  assert (points.front ().when==0 && points.back ().when==duration);
  assert (evaluate (a,b,0)==a.value && evaluate (a,b,duration)==b.value);
  size_t segment=1;
  for (int64_t t=0;t<=duration;++t) {
   while (segment+1<points.size () && t>points[segment].when) ++segment;
   auto l=points[segment-1],r=points[segment];
   assert (r.when>l.when);
   const double compiled=l.value+(r.value-l.value)*double(t-l.when)/(r.when-l.when);
   const double exact=evaluate(a,b,t); assert (exact>=0 && exact<=1);
   worst=std::max(worst,std::abs(exact-compiled));
   assert (std::abs(exact-compiled)<.0001);
   if (t>0) assert (reverse ? exact<=evaluate(a,b,t-1) : exact>=evaluate(a,b,t-1));
  }
  if (bend==0) assert (points.size()==2);
  ++cases;
 }
 std::cout << cases << " curve cases; worst normalized playback error " << worst << "\n";
}
