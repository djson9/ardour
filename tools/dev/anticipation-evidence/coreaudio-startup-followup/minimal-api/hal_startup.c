#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
static atomic_uint callbacks;
static OSStatus render(void* c,AudioUnitRenderActionFlags* f,const AudioTimeStamp* t,UInt32 bus,UInt32 n,AudioBufferList* b){
 for(UInt32 i=0;i<b->mNumberBuffers;++i)if(b->mBuffers[i].mData)memset(b->mBuffers[i].mData,0,b->mBuffers[i].mDataByteSize);
 atomic_fetch_add(&callbacks,1);return 0;
}
#define STEP(name,call) do{fprintf(stderr,"BEGIN %s\n",name);OSStatus e=(call);fprintf(stderr,"END %s %d\n",name,(int)e);if(e)return 1;}while(0)
int main(int argc,char**argv){alarm(15);const char* target=argc>1?argv[1]:"BlackHole 2ch";UInt32 size=0;AudioObjectPropertyAddress a={kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,0};
 CFRunLoopRef loop=NULL;a.mSelector=kAudioHardwarePropertyRunLoop;STEP("runloop",AudioObjectSetPropertyData(kAudioObjectSystemObject,&a,0,NULL,sizeof(loop),&loop));a.mSelector=kAudioHardwarePropertyDevices;
 STEP("device size",AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&a,0,NULL,&size));AudioDeviceID ids[size/sizeof(AudioDeviceID)],device=0;
 STEP("devices",AudioObjectGetPropertyData(kAudioObjectSystemObject,&a,0,NULL,&size,ids));
 for(unsigned i=0;i<size/sizeof(AudioDeviceID);++i){CFStringRef s=NULL;UInt32 bytes=sizeof(s);AudioObjectPropertyAddress name={kAudioObjectPropertyName,kAudioObjectPropertyScopeGlobal,0};if(!AudioObjectGetPropertyData(ids[i],&name,0,NULL,&bytes,&s)){char value[256];CFStringGetCString(s,value,sizeof(value),kCFStringEncodingUTF8);CFRelease(s);if(!strcmp(value,target))device=ids[i];}}
 if(!device)return 2;fprintf(stderr,"DEVICE %u %s\n",device,target);
 AudioComponentDescription d={kAudioUnitType_Output,kAudioUnitSubType_HALOutput,kAudioUnitManufacturer_Apple,0,0};AudioUnit unit;
 STEP("new",AudioComponentInstanceNew(AudioComponentFindNext(NULL,&d),&unit));UInt32 input=0,output=1;
 STEP("input off",AudioUnitSetProperty(unit,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Input,1,&input,sizeof(input)));
 STEP("output on",AudioUnitSetProperty(unit,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Output,0,&output,sizeof(output)));
 STEP("select device",AudioUnitSetProperty(unit,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&device,sizeof(device)));
 AURenderCallbackStruct cb={render,NULL};STEP("callback",AudioUnitSetProperty(unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&cb,sizeof(cb)));
 STEP("initialize",AudioUnitInitialize(unit));STEP("start",AudioOutputUnitStart(unit));sleep(2);STEP("stop",AudioOutputUnitStop(unit));STEP("uninitialize",AudioUnitUninitialize(unit));STEP("dispose",AudioComponentInstanceDispose(unit));printf("RESULT callbacks=%u\n",atomic_load(&callbacks));return !atomic_load(&callbacks);
}
