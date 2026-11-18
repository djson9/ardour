#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
extern int hal_main(int,char**);
int main(){@autoreleasepool{
 NSString* uid=[NSString stringWithFormat:@"dev.ardour.startup-output.%d",getpid()];NSString* name=[NSString stringWithFormat:@"Ardour Startup Output %d",getpid()];
 NSDictionary* sub=@{@kAudioSubDeviceUIDKey:@"BlackHole2ch_UID",@kAudioSubDeviceInputChannelsKey:@0,@kAudioSubDeviceOutputChannelsKey:@2};
 NSDictionary* spec=@{@kAudioAggregateDeviceUIDKey:uid,@kAudioAggregateDeviceNameKey:name,@kAudioAggregateDeviceSubDeviceListKey:@[sub],@kAudioAggregateDeviceMainSubDeviceKey:@"BlackHole2ch_UID",@kAudioAggregateDeviceIsPrivateKey:@YES};
 AudioDeviceID device=0;OSStatus e=AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)spec,&device);fprintf(stderr,"CREATE %d id=%u\n",e,device);if(e)return 1;
 AudioObjectPropertyAddress p={kAudioDevicePropertyStreamConfiguration,kAudioDevicePropertyScopeInput,0};UInt32 size=0;AudioObjectGetPropertyDataSize(device,&p,0,NULL,&size);AudioBufferList* b=calloc(1,size);e=AudioObjectGetPropertyData(device,&p,0,NULL,&size,b);unsigned channels=0;if(!e)for(unsigned i=0;i<b->mNumberBuffers;i++)channels+=b->mBuffers[i].mNumberChannels;free(b);fprintf(stderr,"INPUT_CHANNELS %u error=%d\n",channels,e);
 // Do not open the aggregate unless the API actually accepted an output-only layout.
 int status=1;if(!e&&!channels){char* args[]={"hal-startup",(char*)[name UTF8String],NULL};status=hal_main(2,args);}e=AudioHardwareDestroyAggregateDevice(device);fprintf(stderr,"DESTROY %d\n",e);return status;
}}
