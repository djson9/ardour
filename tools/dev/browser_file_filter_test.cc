/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../../gtk2_ardour/browser_file_filter.h"
#include <cassert>
#include <iostream>
int main ()
{
 using namespace BrowserFileFilter;
 std::set<std::string> extensions;
 assert (parse (defaults (), extensions));
 for (auto name : {"Piano.WaV", "Drums.FLAC", "Chords.MID", "Chords.midi", "café.aiff", "voice.mp3", "voice.m4a"}) assert (matches (name, extensions));
 for (auto name : {"readme.txt", "song.wav.exe", "wav", "song.", ".wav"}) assert (!matches (name, extensions));
 assert (parse (" .WAV, *.MiD; wav\nflac\t.mid ", extensions));
 assert (serialize (extensions) == "flac, mid, wav");
 auto previous = extensions;
 for (auto invalid : {"*", "wav, ../mid", "wav, .", "*.wav*", "foo/bar", "wav, *.", "tar.gz"}) {
  assert (!parse (invalid, extensions)); assert (extensions == previous);
 }
 assert (parse ("; , \t\n", extensions));
 assert (extensions.empty () && !matches ("song.wav", extensions));
 assert (parse ("xyz", extensions) && matches ("custom.XYZ", extensions));
 std::cout << "PASS: defaults, case folding, normalization, exact suffixes, invalid input, empty list, custom extensions\n";
}
