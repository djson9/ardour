/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../../gtk2_ardour/browser_file_search.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <chrono>
#include <unistd.h>
static void file(std::string const& p){std::ofstream(p)<<"fixture";}
int main(int argc,char** argv) {
 assert(argc==2);std::string root=argv[1];mkdir((root+"/Drums").c_str(),0700);mkdir((root+"/.hidden").c_str(),0700);
 file(root+"/Kick.WAV");file(root+"/Kick.mid");file(root+"/Drums/Deep Kick.wav");file(root+"/Drums/other.wav");file(root+"/Kick.txt");file(root+"/.hidden/Kick.wav");file(root+"/Été.wav");
 symlink(root.c_str(),(root+"/Drums/cycle").c_str());mkfifo((root+"/Kick.pipe.wav").c_str(),0600);
 std::set<std::string> extensions;BrowserFileFilter::parse("wav mid",extensions);auto no=[] {return false;};
 auto result=BrowserFileSearch::scan(root,"kIcK",extensions,no);
 assert(result&&result->files.size()==3&&!result->limited);assert(result->files.front().relative=="Kick.mid");assert(result->files.back().relative=="Drums/Deep Kick.wav");
 auto words=BrowserFileSearch::scan(root,"kick drums",extensions,no);assert(words->files.size()==1&&words->files[0].relative=="Drums/Deep Kick.wav");
 auto unicode=BrowserFileSearch::scan(root,"E\xcc\x81TE\xcc\x81",extensions,no);assert(unicode->files.size()==1&&unicode->files[0].relative=="Été.wav");
 assert(BrowserFileSearch::scan(root,"kick",{},no)->files.empty());
 assert(BrowserFileSearch::scan(root,"absent",extensions,no)->files.empty());
 assert(BrowserFileSearch::scan(root," ",extensions,no)->files.empty());
 assert(BrowserFileSearch::scan(root+"/missing","kick",extensions,no)->unreadable);
 assert(BrowserFileSearch::scan(root,"kick",extensions,no,1)->limited);
 assert(BrowserFileSearch::scan(root,"kick",extensions,no,1000,1)->limited);
 int checks=0;assert(!BrowserFileSearch::scan(root,"kick",extensions,[&] {return ++checks>=3;}));
 BrowserFileSearch worker;worker.request(root,"kick",extensions);worker.request(root,"été",extensions);
 auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);std::shared_ptr<BrowserFileSearch::Result> latest;
 while(!(latest=worker.take())&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 assert(latest&&latest->files.size()==1&&latest->files[0].relative=="Été.wav");
 worker.request(root,"kick",extensions);worker.cancel();assert(!worker.take());
 std::cout<<"PASS: recursive matching/ranking, Unicode, multiple words, whitelist, hidden files, symlink cycle, FIFO, limits, missing folder, cancellation, latest request\n";
}
