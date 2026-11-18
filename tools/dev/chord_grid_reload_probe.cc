/* SPDX-License-Identifier: GPL-2.0-or-later */
#define main unused_anticipation_main
#include "anticipation_probe.cc"
#undef main
#include "../../gtk2_ardour/chord_grid_reload.h"
#include "pluginterfaces/base/ipluginbase.h"

static std::string version (PluginPtr const& plugin) {
 auto info=std::dynamic_pointer_cast<VST3PluginInfo>(plugin->get_info());
 Steinberg::FUnknownPtr<Steinberg::IPluginFactory2> f(info->m->factory());
 need(bool(f),"factory2");
 for(int i=0;i<f->countClasses();++i){Steinberg::PClassInfo2 c{};if(f->getClassInfo2(i,&c)==Steinberg::kResultOk&&std::string(c.name)=="Chord Grid MIDI")return c.version;}
 throw std::runtime_error("version unavailable");
}
static std::string chunk (PluginPtr const& plugin) {
 std::unique_ptr<XMLNode> state(&plugin->get_state());
 auto nodes=state->children("chunk");need(nodes.size()==1&&!nodes.front()->children().empty(),"plugin state chunk");return nodes.front()->children().front()->content();
}
int main(int argc,char** argv){try {
 namespace fs=std::filesystem;
 need(argc==2&&getenv("RELOAD_OLD_PLUGIN")&&getenv("RELOAD_NEW_PLUGIN"),"fixture arguments");
 need(ARDOUR::init(true,"/tmp"),"init");Temporal::reset();auto ui=std::make_unique<TestUI>();
 auto* engine=AudioEngine::create();auto backend=engine->set_backend("None (Dummy)","ReloadTest","");
 need(backend&&backend->set_driver("Realtime")==0&&backend->set_sample_rate(48000)==0&&backend->set_buffer_size(128)==0&&engine->start()==0,"dummy engine");
 auto* s=new Session(*engine,argv[1],"reload",nullptr,"",true);engine->set_session(nullptr);
 {
  auto route=s->new_audio_route(2,2,{},1,"Reload fixture",PresentationInfo::AudioBus,0).front();
  fs::path installed=fs::path(argv[1]).parent_path()/"installed.vst3";
  fs::copy(getenv("RELOAD_OLD_PLUGIN"),installed,fs::copy_options::recursive|fs::copy_options::copy_symlinks);
  auto info=std::make_shared<VST3PluginInfo>();info->name="Chord Grid MIDI";info->creator="Jay";info->unique_id="ABCDEF019182FAEB4A61797343676D69";info->path=installed.string();info->n_inputs=ChanCount(DataType::AUDIO,2);info->n_outputs=ChanCount(DataType::AUDIO,2);
  auto original=info->load(*s);need(bool(original),"load old plugin");need(version(original)=="1.3.2","old binary version");
  auto peer=info->load(*s);auto current=std::make_shared<PluginInsert>(*s,*route,original);
  need(route->add_processor(current,PreFader)==0,"insert");current->set_name("My chord part");current->enable(false);
  auto id=current->id();const auto saved=chunk(original);
  auto positions=[&]{std::vector<std::string> ids;route->foreach_processor([&](std::weak_ptr<Processor> p){ids.push_back(p.lock()->id().to_s());});return ids;};auto order=positions();
  fs::rename(installed,installed.string()+".old");
  bool rejected=false;try{ChordGridReload::replace(current);}catch(...){rejected=true;}need(rejected&&positions()==order&&current->owner()==route.get(),"missing binary rollback");
  fs::copy(getenv("RELOAD_NEW_PLUGIN"),installed,fs::copy_options::recursive|fs::copy_options::copy_symlinks);
  current=ChordGridReload::replace(current);
  need(version(current->plugin())=="1.3.3"&&version(peer)=="1.3.2","fresh binary with old peer retained");
  need(chunk(current->plugin())==saved,"chord state round trip");need(current->id()==id&&current->name()=="My chord part"&&!current->enabled()&&positions()==order,"insert identity, name, bypass and chain order");
  current=ChordGridReload::replace(current);need(version(current->plugin())=="1.3.3"&&chunk(current->plugin())==saved&&positions()==order,"repeat reload");
  s->request_roll();engine->set_session(s);
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!s->transport_rolling()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(5));
  need(s->transport_rolling(),"rolling fixture");
  rejected=false;try{ChordGridReload::replace(current);}catch(...){rejected=true;}
  need(rejected&&positions()==order,"reload blocked during playback");
  s->request_stop(false,false);while(s->transport_rolling()&&std::chrono::steady_clock::now()<deadline+std::chrono::seconds(2))std::this_thread::sleep_for(std::chrono::milliseconds(5));
  need(!s->transport_rolling(),"stopped fixture");engine->set_session(nullptr);
  need(s->save_state("")==0,"save reloaded session");
  std::cout<<"RESULT {\"passed\":true,\"old_version\":\"1.3.2\",\"new_version\":\"1.3.3\",\"old_peer_preserved\":true,\"state_preserved\":true,\"repeat_reload\":true,\"playback_guard\":true,\"missing_binary_rollback\":true,\"chain_identity_preserved\":true}"<<std::endl;
 }
 delete s;engine->stop();ui.reset();ARDOUR::cleanup();return 0;
}catch(std::exception const& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
