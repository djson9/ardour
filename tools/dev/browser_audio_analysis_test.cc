/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../../gtk2_ardour/browser_audio_analysis.h"
#include <sndfile.h>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

static void write_audio (std::string const& path, float amplitude, int channels=2, double seconds=2, bool nan=false)
{
 SF_INFO info={};info.samplerate=48000;info.channels=channels;info.format=SF_FORMAT_WAV|SF_FORMAT_FLOAT;
 SNDFILE* file=sf_open(path.c_str(),SFM_WRITE,&info);assert(file);
 size_t frames=size_t(seconds*info.samplerate);std::vector<float> samples(frames*channels);
 for(size_t i=0;i<frames;++i)for(int c=0;c<channels;++c)samples[i*channels+c]=amplitude*std::sin(2*3.141592653589793*1000*i/info.samplerate)/(c+1);
 if(nan)samples[0]=std::numeric_limits<float>::quiet_NaN();
 assert(sf_writef_float(file,samples.data(),frames)==sf_count_t(frames));sf_close(file);
}
static std::string contents(std::string const& path) {std::ifstream f(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});}
int main(int argc,char** argv)
{
 assert(argc==2);std::string dir=argv[1];auto cancel=[] {return false;};
 write_audio(dir+"/quiet.wav",.05f);write_audio(dir+"/loud.wav",.4f);
 auto original=contents(dir+"/quiet.wav");
 auto quiet=BrowserAudio::analyze(dir+"/quiet.wav",cancel),loud=BrowserAudio::analyze(dir+"/loud.wav",cancel);
 assert(quiet&&loud&&quiet->error.empty()&&loud->error.empty());
 assert(quiet->loudness_measured&&loud->loudness_measured);
 assert(std::abs(quiet->seconds-2)<1e-6&&quiet->channels==2);
 assert(quiet->peaks.size()==2048&&quiet->sample_peak>.049&&quiet->sample_peak<.051);
 assert(std::abs(quiet->gain/loud->gain-8)<.08);
 assert(std::abs(quiet->sample_peak*quiet->gain-loud->sample_peak*loud->gain)<.002);
 assert(quiet->sample_peak*quiet->gain<=.891251f);
 assert(contents(dir+"/quiet.wav")==original);
 write_audio(dir+"/silent.wav",0);auto silent=BrowserAudio::analyze(dir+"/silent.wav",cancel);assert(silent&&silent->error.empty()&&silent->gain==1&&silent->sample_peak==0);
 write_audio(dir+"/short.wav",.05f,1,.05);auto short_clip=BrowserAudio::analyze(dir+"/short.wav",cancel);assert(short_clip&&short_clip->error.empty()&&std::isfinite(short_clip->gain)&&short_clip->gain<=15.848933f&&short_clip->sample_peak*short_clip->gain<=.891251f);
 write_audio(dir+"/near-silence.wav",.00001f);auto near=BrowserAudio::analyze(dir+"/near-silence.wav",cancel);assert(near&&near->error.empty()&&near->gain<=15.848933f);
 write_audio(dir+"/nan.wav",.1f,1,.1,true);auto invalid=BrowserAudio::analyze(dir+"/nan.wav",cancel);assert(invalid&&!invalid->error.empty()&&invalid->peaks.empty());
 auto missing=BrowserAudio::analyze(dir+"/missing.wav",cancel);assert(missing&&!missing->error.empty());
 // A 12 kHz sine sampled at 45 degrees peaks between samples: sample=.9,
 // reconstructed peak about 1.27. LUFS is unavailable for this 50 ms clip.
 SF_INFO info={};info.samplerate=48000;info.channels=1;info.format=SF_FORMAT_WAV|SF_FORMAT_FLOAT;
 auto* tpfile=sf_open((dir+"/intersample.wav").c_str(),SFM_WRITE,&info);assert(tpfile);
 std::vector<float> tp(2400);for(size_t n=0;n<tp.size();++n)tp[n]=(n%4<2?.9f:-.9f);
 assert(sf_write_float(tpfile,tp.data(),tp.size())==sf_count_t(tp.size()));sf_close(tpfile);
 auto intersample=BrowserAudio::analyze(dir+"/intersample.wav",cancel);
 assert(intersample&&intersample->error.empty()&&!intersample->loudness_measured&&intersample->gain<.8f&&intersample->gain>.5f);
 write_audio(dir+"/surround.wav",.2f,4,.05);auto surround=BrowserAudio::analyze(dir+"/surround.wav",cancel);
 assert(surround&&surround->error.empty()&&surround->channels==4&&!surround->loudness_measured&&surround->gain>1&&surround->sample_peak*surround->gain<=.891251f);
 int checks=0;assert(!BrowserAudio::analyze(dir+"/quiet.wav",[&] {return ++checks>=4;}));
 std::cout<<"PASS: perceived loudness, stereo peaks, duration, true/sample-peak ceilings, multichannel fallback, silence, short clips, gain cap, malformed input, cancellation, unchanged source bytes\n";
 std::cout<<"quiet gain="<<quiet->gain<<" loud gain="<<loud->gain<<" short gain="<<short_clip->gain<<"\n";
}
