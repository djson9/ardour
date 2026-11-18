/* SPDX-License-Identifier: GPL-2.0-or-later */
#define main unused_anticipation_main
#include "anticipation_probe.cc"
#undef main
#include "ardour/location.h"
#include "ardour/anticipative_process.h"

struct LocateWitness : Processor {
	std::atomic<samplepos_t> last {-1};
	LocateWitness (Session& s) : Processor (s, "Audible locate witness", s) {}
	bool can_support_io_configuration (ChanCount const& in, ChanCount& out) override { out = in; return true; }
	void run (BufferSet&, samplepos_t start, samplepos_t, double speed, pframes_t, bool) override {
		if (speed == 1.) last.store (start);
	}
};

int main (int argc, char** argv)
{
	try {
		need (argc == 2 && std::string (g_get_home_dir ()) == getenv ("ARDOUR_PROBE_HOME"), "private session path");
		need (ARDOUR::init (true, "/tmp"), "init"); Temporal::reset ();
		auto ui = std::make_unique<TestUI> ();
		Config->set_loop_is_mode (true);
		auto* engine = AudioEngine::create ();
		auto backend = engine->set_backend ("None (Dummy)", "LoopLocate", "");
		need (backend && backend->set_driver ("Realtime") == 0 && backend->set_sample_rate (48000) == 0 && backend->set_buffer_size (128) == 0 && engine->start () == 0, "engine");
		auto* s = new FixtureSession (*engine, argv[1], "loop-locate", nullptr, "", true);
		engine->set_session (nullptr);
		auto track = s->new_audio_track (2, 2, {}, 1, "Locate track", 0, Normal, false).front ();
		track->set_anticipative_processing_disabled (true); // Observe device-time DSP, not lookahead.
		auto ahead = s->new_audio_track (2, 2, {}, 1, "Lookahead track", 1, Normal, false).front ();
		auto witness = std::make_shared<LocateWitness> (*s);
		need (track->add_processor (witness, PreFader) == 0, "witness");
		constexpr samplepos_t lo = 48000, hi = 480000;
		auto* loop = new Location (*s, timepos_t (lo), timepos_t (hi), "Loop", Location::IsAutoLoop);
		s->locations ()->add (loop); s->set_auto_loop_location (loop);
		engine->set_session (s);
		auto wait = [&] (auto predicate, const char* what) {
			const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (1200);
			while (std::chrono::steady_clock::now () < deadline) {
				{ PBD::Mutex::Lock lock (engine->process_lock ()); if (predicate ()) return; }
				std::this_thread::sleep_for (std::chrono::milliseconds (1));
			}
			std::cerr << "position=" << s->transport_sample () << " DSP=" << witness->last.load () << "\n";
			need (false, what);
		};
		auto at = [&] (samplepos_t target) {
			return s->transport_rolling () && s->setup_settled () && !s->global_locate_pending () &&
			       s->transport_sample () >= target && s->transport_sample () < target + 4800 &&
			       witness->last.load () >= target && witness->last.load () < target + 4800;
		};
		auto locate = [&] (samplepos_t target, LocateTransportDisposition disposition, const char* what) {
			witness->last.store (-1); s->request_locate (target, false, disposition);
			wait ([&] { return at (target); }, what);
			const auto device = engine->processed_samples ();
			wait ([&] { return engine->processed_samples () >= device + 2048; }, "post-seek callback progress");
			{ PBD::Mutex::Lock lock (engine->process_lock ()); need (at (target), what); }
			need (s->get_play_loop () && loop->start_sample () == lo && loop->end_sample () == hi, "loop remains enabled and unchanged");
		};
		wait ([&] { return s->setup_settled (); }, "setup");
		s->request_play_loop (true, false);
		wait ([&] { return s->get_play_loop () && s->setup_settled (); }, "enable loop");
		s->request_roll (); wait ([&] { return at (lo); }, "initial loop playback");
		wait ([&] { auto* node = s->anticipative_process ().find_node (ahead.get ()); return node && node->queue->state () == AnticipationQueue::State::Ahead; }, "active lookahead coverage");
		locate (240000, RollIfAppropriate, "arrangement click must play at requested point, not loop start");
		s->request_stop (false, false); wait ([&] { return s->transport_stopped () && s->setup_settled (); }, "cursor drag stop");
		locate (336000, MustRoll, "cursor release must resume at requested point");
		locate (lo - 4800, MustRoll, "locate before loop");
		locate (hi + 48000, MustRoll, "locate after loop");
		witness->last.store (-1); s->request_locate (hi, false, MustRoll);
		wait ([&] { return at (lo); }, "locate exactly at loop end wraps");
		locate (hi - 4800, MustRoll, "locate near loop end");
		wait ([&] { return at (lo); }, "loop still wraps after locate");
		s->request_stop (false, false); wait ([&] { return s->transport_stopped () && s->setup_settled (); }, "stop");
		s->request_locate (240000, false, MustStop);
		wait ([&] { return s->transport_stopped () && s->transport_sample () == 240000 && s->setup_settled (); }, "stopped locate stays stopped");
		s->request_roll (); wait ([&] { return at (lo); }, "ordinary Play retains loop-start behavior");
		s->request_stop (false, false); wait ([&] { return s->transport_stopped () && s->setup_settled (); }, "final stop");
		engine->set_session (nullptr); witness.reset (); track.reset (); ahead.reset (); engine->stop (); delete s;
		ui.reset (); ARDOUR::cleanup ();
		std::cout << "RESULT {\"passed\":true,\"rolling_click\":true,\"cursor_resume\":true,\"outside_loop\":true,\"wrap_preserved\":true,\"stopped_click\":true}\n";
		return 0;
	} catch (std::exception const& e) { std::cerr << "FAIL " << e.what () << "\n"; return 1; }
}
