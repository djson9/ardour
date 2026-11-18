/* SPDX-License-Identifier: GPL-2.0-or-later */
// Reuse native session/backend setup helpers, never the feasibility probe main.
#define main unused_anticipation_main
#include "anticipation_probe.cc"
#undef main
#include "ardour/group_tracks.h"
#include "ardour/gain_control.h"
#include "ardour/mute_control.h"
#include "ardour/route_group.h"

struct Constant : Processor {
	float value;
	Constant (Session& s, float v) : Processor (s, "Group test signal", s), value (v) {}
	bool can_support_io_configuration (ChanCount const& in, ChanCount& out) override { out = in; return true; }
	void run (BufferSet& b, samplepos_t, samplepos_t, double, pframes_t n, bool) override {
		for (unsigned c = 0; c < b.count ().n_audio (); ++c) std::fill_n (b.get_audio (c).data (), n, value);
	}
};
struct Scale : Processor {
	Scale (Session& s) : Processor (s, "Group test effect", s) {}
	bool can_support_io_configuration (ChanCount const& in, ChanCount& out) override { out = in; return true; }
	void run (BufferSet& b, samplepos_t, samplepos_t, double, pframes_t n, bool) override {
		for (unsigned c = 0; c < b.count ().n_audio (); ++c) for (unsigned i = 0; i < n; ++i) b.get_audio (c).data ()[i] *= .25f;
	}
};
struct Capture : Processor {
	std::atomic<float> sample {0};
	std::atomic<unsigned> blocks {0};
	Capture (Session& s) : Processor (s, "Group test capture", s) {}
	bool can_support_io_configuration (ChanCount const& in, ChanCount& out) override { out = in; return true; }
	void run (BufferSet& b, samplepos_t, samplepos_t, double, pframes_t n, bool) override {
		if (n && b.count ().n_audio ()) sample.store (b.get_audio (0).data ()[n-1]);
		++blocks;
	}
};

static bool connected (std::shared_ptr<Route> a, std::shared_ptr<Route> b)
{
	return a->output ()->connected_to (b->input ());
}

int main (int argc, char** argv)
{
	try {
		need (argc == 2, "session path");
		need (std::string (g_get_home_dir ()) == getenv ("ARDOUR_PROBE_HOME"), "private preferences");
		need (ARDOUR::init (true, "/tmp"), "init");
		Temporal::reset ();
		auto ui = std::make_unique<TestUI> ();
		auto* engine = AudioEngine::create ();
		auto backend = engine->set_backend ("None (Dummy)", "GroupTracksTest", "");
		need (backend && backend->set_driver ("Realtime") == 0 && backend->set_sample_rate (48000) == 0 && backend->set_buffer_size (128) == 0 && engine->start () == 0, "dummy engine");
		auto* s = new Session (*engine, argv[1], "group-tracks", nullptr, "", true);
		engine->set_session (nullptr);
		std::string error, group_id, outer_id, child_id;
		const bool audio = getenv ("GROUP_TRACK_AUDIO");
		{
			auto sink = s->new_audio_route (2, 2, {}, 1, "Sink", PresentationInfo::AudioBus, 0).front ();
			auto a = s->new_audio_route (2, 2, {}, 1, "A", PresentationInfo::AudioBus, 1).front ();
			auto b = s->new_audio_route (2, 2, {}, 1, "B", PresentationInfo::AudioBus, 2).front ();
			auto c = s->new_audio_route (2, 2, {}, 1, "C", PresentationInfo::AudioBus, 3).front ();
			for (auto r : {a, b, c}) {
				r->input ()->disconnect ();
				r->output ()->disconnect ();
				need (r->output ()->connect_ports_to_bundle (sink->input ()->bundle (), false, true) == 0, "fixture outputs");
			}
			auto linked = s->new_route_group ("Existing linked controls");
			s->add_route_group (linked); linked->set_gain (false); linked->add (a); linked->add (b);
			auto g = GroupTracks::create (*s, {a, b}, error);
			need (bool (g), error.c_str ());
			need (GroupTracks::is_group (g) && GroupTracks::parent (*s, a) == g, "group metadata");
			need (a->route_group () == linked && b->route_group () == linked, "linked controls preserved");
			need (connected (a, g) && connected (b, g) && connected (g, sink) && !connected (a, sink), "exclusive group routing");
			if (audio) {
				for (auto r : {a, b, c, g, sink}) r->gain_control ()->set_value (1, PBD::Controllable::NoGroup);
				c->output ()->disconnect ();
				need (a->add_processor (std::make_shared<Constant> (*s, .1f), PreFader) == 0, "source A");
				need (b->add_processor (std::make_shared<Constant> (*s, .2f), PreFader) == 0, "source B");
				need (g->add_processor (std::make_shared<Scale> (*s), PreFader) == 0, "group effect");
				auto capture = std::make_shared<Capture> (*s);
				need (sink->add_processor (capture, PreFader) == 0, "capture");
				g->gain_control ()->set_value (.5, PBD::Controllable::NoGroup);
				s->request_roll (); engine->set_session (s);
				auto expect = [&] (float value) {
					const auto start = std::chrono::steady_clock::now ();
					unsigned stable = 0, last = capture->blocks.load ();
					while (std::chrono::steady_clock::now () - start < std::chrono::seconds (2)) {
						auto count = capture->blocks.load ();
						if (count != last) { stable = std::abs (capture->sample.load () - value) < 1e-5 ? stable + 1 : 0; last = count; }
						if (stable >= 8) return;
						std::this_thread::sleep_for (std::chrono::milliseconds (2));
					}
					std::cerr << "observed " << capture->sample.load () << " expected " << value << "\n";
					need (false, "native graph audio mismatch");
				};
				expect (.0375f); // (A+B) * group effect * group gain, exactly once.
				g->mute_control ()->set_value (1, PBD::Controllable::NoGroup); expect (0);
				g->mute_control ()->set_value (0, PBD::Controllable::NoGroup); expect (.0375f);
				need (GroupTracks::drop (*s, {b}, {}, {}, error), error.c_str ()); expect (.2125f);
				need (GroupTracks::drop (*s, {b}, g, {}, error), error.c_str ()); expect (.0375f);
				need (a->gain_control ()->get_value () == 1 && b->gain_control ()->get_value () == 1, "independent child gains");
				engine->set_session (nullptr);
			} else {
				GroupTracks::set_collapsed (*s, g, true);
				need (GroupTracks::folded (*s, a) && !a->presentation_info ().hidden (), "fold is not hidden/muted");
				auto outer = GroupTracks::create (*s, {g, a, c}, error);
				need (bool (outer), error.c_str ()); // Selected descendants are not grouped twice.
				need (GroupTracks::parent (*s, g) == outer && GroupTracks::parent (*s, a) == g, "nested parents");
				need (GroupTracks::children (*s, outer, true).size () == 4, "nested descendants");
				need (connected (g, outer) && connected (outer, sink), "nested audio routing");
				GroupTracks::set_collapsed (*s, outer, true);
				GroupTracks::set_collapsed (*s, outer, false);
				need (GroupTracks::folded (*s, a) && !GroupTracks::folded (*s, c), "inner fold survives outer toggle");
				auto count = s->get_routes ()->size ();
				need (!GroupTracks::create (*s, {a, c}, error) && s->get_routes ()->size () == count, "cross-parent grouping rejected without mutation");
				GroupTracks::move (*s, {g}, false);
				need (c->presentation_info ().order () < g->presentation_info ().order () && g->presentation_info ().order () < a->presentation_info ().order (), "group moves with children");
				need (!GroupTracks::drop (*s, {outer}, g, {}, error), "reject descendant cycle");
				need (!GroupTracks::drop (*s, {sink}, g, {}, error) && !GroupTracks::parent (*s, sink), "reject downstream feedback without mutation");
				need (GroupTracks::drop (*s, {a, b}, outer, g, error), error.c_str ());
				need (GroupTracks::parent (*s, a) == outer && connected (a, outer) && !connected (a, g), "multi-track reparent rewires outputs");
				need (a->presentation_info ().order () < b->presentation_info ().order () && b->presentation_info ().order () < g->presentation_info ().order (), "drop preserves selection order at insertion");
				need (GroupTracks::drop (*s, {a, b}, g, {}, error), error.c_str ());
				need (GroupTracks::collapsed (g) && GroupTracks::folded (*s, a), "drop into collapsed group preserves fold");
				need (GroupTracks::drop (*s, {a}, {}, {}, error), error.c_str ());
				need (!GroupTracks::parent (*s, a) && connected (a, sink) && !connected (a, outer) && !connected (a, g), "leave all nested groups to outer destination");
				need (GroupTracks::drop (*s, {a}, g, b, error), error.c_str ());
				need (GroupTracks::drop (*s, {g, a}, {}, {}, error), error.c_str ());
				need (!GroupTracks::parent (*s, g) && GroupTracks::parent (*s, a) == g && connected (a, g) && connected (g, sink), "move group subtree exactly once");
				need (GroupTracks::drop (*s, {g}, outer, c, error), error.c_str ());
				auto mono = s->new_audio_route (1, 1, {}, 1, "Mono", PresentationInfo::AudioBus, PresentationInfo::max_order).front ();
				need (!GroupTracks::drop (*s, {c, mono}, g, {}, error) && GroupTracks::parent (*s, c) == outer && connected (c, outer), "validate all layouts before any changes");
				s->remove_route (mono);
				// Reordering within a group must not overwrite a manual output.
				b->output ()->disconnect (); b->output ()->connect_ports_to_bundle (sink->input ()->bundle (), false, true);
				need (GroupTracks::drop (*s, {b}, g, a, error) && connected (b, sink) && !connected (b, g), "same-parent reorder preserves manual routing");
				b->output ()->disconnect (); b->output ()->connect_ports_to_bundle (g->input ()->bundle (), false, true);
				group_id = g->id ().to_s (); outer_id = outer->id ().to_s (); child_id = a->id ().to_s ();
				need (s->save_state ("") == 0, "save hierarchy");
			}
		}
		delete s;
		if (!audio) {
			s = new Session (*engine, argv[1], "group-tracks", nullptr, "", true);
			engine->set_session (nullptr);
			{
				auto g = s->route_by_id (PBD::ID (group_id)), outer = s->route_by_id (PBD::ID (outer_id)), a = s->route_by_id (PBD::ID (child_id));
				need (GroupTracks::parent (*s, a) == g && GroupTracks::parent (*s, g) == outer && GroupTracks::collapsed (g), "saved hierarchy and collapse restored");
				need (connected (a, g) && connected (g, outer), "saved audio graph restored");
				need (GroupTracks::ungroup (*s, outer, error), error.c_str ());
				need (!GroupTracks::parent (*s, g) && GroupTracks::parent (*s, a) == g && GroupTracks::collapsed (g), "ungroup outer preserves inner");
				need (GroupTracks::ungroup (*s, g, error), error.c_str ());
				need (!GroupTracks::parent (*s, a) && !GroupTracks::folded (*s, a), "ungroup restores child visibility");
				need (connected (a, s->route_by_name ("Sink")), "ungroup restores destination");
			}
			delete s;
		}
		engine->stop (); ui.reset (); ARDOUR::cleanup ();
		std::cout << "RESULT {\"passed\":true,\"case\":\"" << (audio ? "audio" : "hierarchy-roundtrip") << "\"}\n";
		return 0;
	} catch (std::exception const& e) { std::cerr << "FAIL " << e.what () << "\n"; return 1; }
}
