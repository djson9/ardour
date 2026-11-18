/* Match the GUI's latency-critical activity for standalone macOS audio probes. */
#import <Foundation/Foundation.h>
#include <cstdio>
#include <cstdlib>
static id activity;
__attribute__((constructor)) static void begin_audio_activity () {
 if (getenv("ARDOUR_PROBE_ALLOW_APP_NAP")) { return; }
 @autoreleasepool {
  activity = [[[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityLatencyCritical reason:@"realtime audio probe"] retain];
  fprintf(stderr,"NATIVE_PROBE latency_critical_activity=%s\n",activity?"active":"unavailable");
 }
}
__attribute__((destructor)) static void end_audio_activity () {
 if (activity) {
  [[NSProcessInfo processInfo] endActivity:activity];
  [activity release];
 }
}
