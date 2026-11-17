// Host adapter for separately installed, locally extracted warp engines.
// This file contains no vendor DSP implementation.
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

static std::string number (double value) {
 std::ostringstream s; s.imbue (std::locale::classic ()); s << std::setprecision (17) << value; return s.str ();
}
int main (int argc, char** argv) {
 try {
  if (argc != 15) throw std::runtime_error ("mode input output rate channels ratio semitones formants envelope grain flux preserve loop transient-envelope");
  const std::string mode = argv[1];
  if (mode != "complex" && mode != "complex-pro" && mode != "texture" && mode != "repitch" && mode != "tones" && mode != "beats") throw std::runtime_error ("Original " + mode + " engine is not installed");
  const double semitones = std::stod (argv[7]);
  const double formants = std::stod (argv[8]);
  if (!std::isfinite (semitones) || std::abs (semitones) > 24. || !std::isfinite (formants) || formants < 0. || formants > 100.) throw std::runtime_error ("Invalid pitch/formant setting");
  const float pitch = std::pow (1.0594631433486938f, float (semitones));
  const double envelope = std::pow (double (pitch), formants / 100.);
  const auto directory = std::filesystem::absolute (argv[0]).parent_path ();
  std::vector<std::string> args = {(directory / "OriginalEngineRender").string (), (directory / (mode + ".dylib")).string (), argv[2], argv[3], argv[4], argv[5], argv[6], number (pitch), number (envelope), argv[9], mode == "complex" ? "--engine-version=1" : "--engine-version=0"};
  if (mode == "texture" || mode == "repitch" || mode == "tones" || mode == "beats") {
   args = {(directory / "OriginalTextureRender").string (), (directory / (mode + ".dylib")).string (), argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[10], argv[11]};
   if (mode == "beats") { args.push_back (argv[12]); args.push_back (argv[13]); args.push_back (argv[14]); }
   args.push_back ("--mode=" + mode);
  }
  std::vector<char*> pointers; for (auto& s : args) pointers.push_back (const_cast<char*> (s.c_str ())); pointers.push_back (nullptr);
  execv (pointers[0], pointers.data ());
  throw std::runtime_error ("Cannot execute original warp worker");
 } catch (std::exception const& e) { std::cerr << e.what () << '\n'; return 1; }
}
