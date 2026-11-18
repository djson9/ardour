/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <glib.h>
#include <sys/stat.h>
#include "browser_file_filter.h"

/* Session/GTK-independent search. One worker, latest request wins. Never follow
 * directory symlinks: sample libraries can contain cycles or links outside root. */
class BrowserFileSearch {
public:
 struct Match { std::string relative, folded; int rank; };
 struct Result { std::vector<Match> files; bool limited=false, unreadable=false; };
 BrowserFileSearch () : _thread ([this] { run (); }) {}
 ~BrowserFileSearch () { { std::lock_guard<std::mutex> lock (_mutex); _stop=true; ++_generation; } _changed.notify_one (); _thread.join (); }
 void cancel () { std::lock_guard<std::mutex> lock (_mutex); ++_generation; _root.clear (); _result.reset (); }
 void request (std::string const& root, std::string const& query, std::set<std::string> const& extensions) {
  { std::lock_guard<std::mutex> lock (_mutex); ++_generation; _root=root; _query=query; _extensions=extensions; _result.reset (); }
  _changed.notify_one ();
 }
 std::shared_ptr<Result> take () { std::lock_guard<std::mutex> lock (_mutex); auto result=_result; _result.reset (); return result; }
 static std::string fold (std::string const& text) {
  char* valid=g_utf8_validate (text.c_str (), text.size (), nullptr) ? g_strdup (text.c_str ()) : g_filename_display_name (text.c_str ());
  char* normalized=g_utf8_normalize (valid,-1,G_NORMALIZE_ALL_COMPOSE); g_free (valid);
  char* folded=g_utf8_casefold (normalized,-1); g_free (normalized);
  std::string result=folded; g_free (folded); return result;
 }
 template<class Cancelled> static std::shared_ptr<Result> scan (std::string const& root, std::string const& query, std::set<std::string> const& extensions, Cancelled cancelled, size_t limit=1000, size_t visit_limit=100000) {
  auto result=std::make_shared<Result> ();
  std::vector<std::string> tokens; std::istringstream words (fold (query)); std::string word;
  while (words >> word) tokens.push_back (word);
  if (tokens.empty () || extensions.empty ()) return result;
  std::vector<std::string> pending {""}; size_t visited=0;
  while (!pending.empty () && !result->limited) {
   if (cancelled ()) return nullptr;
   auto relative=pending.back (); pending.pop_back ();
   auto path=root+"/"+relative;
   GDir* dir=g_dir_open (path.c_str (),0,nullptr);
   if (!dir) { result->unreadable=true; continue; }
   while (const char* name=g_dir_read_name (dir)) {
    if (cancelled ()) { g_dir_close (dir); return nullptr; }
    if (++visited > visit_limit) { result->limited=true; break; }
    if (name[0]=='.') continue;
    auto child=relative.empty () ? std::string (name) : relative+"/"+name;
    struct stat info;
    if (lstat ((root+"/"+child).c_str (),&info)) { result->unreadable=true; continue; }
    if (S_ISDIR (info.st_mode)) { pending.push_back (child); continue; }
    if (!S_ISREG (info.st_mode) || !BrowserFileFilter::matches (name,extensions)) continue;
    auto folded=fold (child);
    if (!std::all_of (tokens.begin (),tokens.end (),[&] (std::string const& token) { return folded.find (token)!=std::string::npos; })) continue;
    auto basename=fold (name); auto phrase=fold (query);
    int rank=basename==phrase ? 0 : basename.find (phrase)==0 ? 1 : basename.find (phrase)!=std::string::npos ? 2 : 3;
    result->files.push_back ({child,folded,rank});
    if (result->files.size ()>=limit) { result->limited=true; break; }
   }
   g_dir_close (dir);
  }
  if (cancelled ()) return nullptr;
  std::sort (result->files.begin (),result->files.end (),[] (Match const& a,Match const& b) { return a.rank!=b.rank ? a.rank<b.rank : a.folded<b.folded; });
  return result;
 }
private:
 std::mutex _mutex;
 std::condition_variable _changed;
 std::atomic<uint64_t> _generation {0};
 bool _stop=false;
 std::string _root,_query;
 std::set<std::string> _extensions;
 std::shared_ptr<Result> _result;
 std::thread _thread;
 void run () {
  uint64_t seen=0;
  for (;;) {
   std::string root,query; std::set<std::string> extensions; uint64_t version;
   { std::unique_lock<std::mutex> lock (_mutex); _changed.wait (lock,[&] { return _stop || _generation!=seen; });
    if (_stop) return;
    seen=version=_generation; root=_root; query=_query; extensions=_extensions;
   }
   if (root.empty ()) continue;
   auto result=scan (root,query,extensions,[&] { return _generation!=version; });
   { std::lock_guard<std::mutex> lock (_mutex); if (_generation==version) _result=result; }
  }
 }
};
