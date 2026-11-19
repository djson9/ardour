/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Build a session where a kick ducks a pad through Jay Compressor's external
 * sidechain, save it, then roll it and measure the gain reduction. The meters
 * are added only after saving, so the project contains no test processors. */
#define main unused_anticipation_main
#include "anticipation_probe.cc"
#undef main
#include <ctime>
#include <glibmm/miscutils.h>
#include <map>
#include "ardour/plugin_manager.h"
#include "ardour/send.h"
#include "ardour/audio_port.h"
#include "ardour/location.h"
#include "ardour/gain_control.h"
#include "ardour/session_directory.h"
#include "ardour/mute_master.h"
#include "pbd/compose.h"

static constexpr samplecnt_t SR = 48000, BEAT = SR / 2, LEN = BEAT * 32; // 8 bars at 120 BPM

struct Meter : Processor {
	static constexpr unsigned N = 8192;
	std::vector<samplepos_t> at = std::vector<samplepos_t> (N);
	std::vector<float> rms = std::vector<float> (N);
	std::atomic<unsigned> count {0};
	std::atomic<bool> armed {false};
	Meter (Session& s, char const* n) : Processor (s, n, s) {}
	bool can_support_io_configuration (ChanCount const& in, ChanCount& out) override { out = in; return true; }
	void run (BufferSet& b, samplepos_t start, samplepos_t, double, pframes_t n, bool) override {
		if (!armed || !n || !b.count ().n_audio ()) return;
		unsigned k = count.load ();
		if (k >= N) return;
		double sum = 0;
		for (unsigned c = 0; c < b.count ().n_audio (); ++c) { auto d = b.get_audio (c).data (); for (unsigned i = 0; i < n; ++i) sum += d[i] * d[i]; }
		at[k] = start; rms[k] = std::sqrt (sum / (n * b.count ().n_audio ()));
		count.store (k + 1);
	}
};

static std::shared_ptr<AudioFileSource> write_source (Session& s, std::string const& name, std::vector<Sample> const& data)
{
	auto path = Glib::build_filename (s.session_directory ().sound_path (), name);
	auto src = std::dynamic_pointer_cast<AudioFileSource> (SourceFactory::createWritable (DataType::AUDIO, s, path, SR));
	need (bool (src), "source");
	need (src->write (data.data (), data.size ()) == (samplecnt_t) data.size (), "write source");
	time_t now = time (nullptr); struct tm tm = *localtime (&now);
	src->update_header (0, tm, now);
	src->done_with_peakfile_writes ();
	src->mark_immutable ();
	src->mark_nonremovable ();
	return src;
}

static void place (Session& s, std::shared_ptr<AudioTrack> track, SourceList const& sources, std::string const& name)
{
	PBD::PropertyList whole;
	whole.add (Properties::start, timepos_t (0)); whole.add (Properties::length, timecnt_t (LEN));
	whole.add (Properties::name, name); whole.add (Properties::whole_file, true);
	auto master = RegionFactory::create (sources, whole);
	PBD::PropertyList copy;
	copy.add (Properties::start, timepos_t (0)); copy.add (Properties::length, timecnt_t (LEN)); copy.add (Properties::name, name);
	auto region = RegionFactory::create (master, copy);
	need (bool (region), "region");
	track->playlist ()->add_region (region, timepos_t (0));
}

static std::vector<Sample> kick ()
{
	std::vector<Sample> out (LEN, 0.f);
	for (samplepos_t beat = 0; beat < LEN; beat += BEAT) {
		double phase = 0;
		for (samplepos_t i = 0; i < SR * 35 / 100 && beat + i < LEN; ++i) {
			double t = double (i) / SR;
			phase += 2 * M_PI * (48 + 110 * std::exp (-t / .028)) / SR;
			double click = i < 96 ? .25 * (1 - i / 96.) : 0;
			out[beat + i] = float (.9 * std::exp (-t / .13) * std::sin (phase) + click * std::exp (-t / .002));
		}
	}
	return out;
}

static std::vector<Sample> pad (bool right)
{
	// Am | F | C | G, two bars each: low root plus a mid triad, gently filtered saws.
	const double chords[4][4] = {{55.00, 220.00, 261.63, 329.63}, {43.65, 174.61, 220.00, 261.63},
	                             {65.41, 196.00, 261.63, 329.63}, {49.00, 196.00, 246.94, 293.66}};
	std::vector<Sample> out (LEN);
	double ph[4] = {0, .3, .6, .9}, lp = 0;
	const double detune = right ? 1.004 : .996;
	for (samplepos_t i = 0; i < LEN; ++i) {
		auto const& ch = chords[(i / (BEAT * 8)) % 4];
		double v = 0;
		for (int k = 0; k < 4; ++k) {
			ph[k] += ch[k] * (k ? detune : 1.) / SR; ph[k] -= std::floor (ph[k]);
			v += (k ? .14 : .32) * (2 * ph[k] - 1);
		}
		lp += .08 * (v - lp);
		double edge = std::min (1., std::min (i, LEN - i) / (SR * .01));
		out[i] = float (lp * edge);
	}
	return out;
}

static std::map<std::string, uint32_t> parameters (std::shared_ptr<Plugin> p)
{
	std::map<std::string, uint32_t> m;
	for (uint32_t i = 0; i < p->parameter_count (); ++i) {
		bool ok = false; uint32_t c = p->nth_parameter (i, ok);
		if (ok && p->parameter_is_input (c)) m[p->describe_parameter (Evoral::Parameter (PluginAutomation, 0, c))] = c;
	}
	return m;
}

int main (int argc, char** argv)
{
	try {
		need (argc == 3, "parent-dir session-name");
		need (std::string (g_get_home_dir ()) == getenv ("ARDOUR_PROBE_HOME"), "private preferences");
		need (ARDOUR::init (true, "/tmp"), "init");
		Temporal::reset ();
		auto ui = std::make_unique<TestUI> ();
		auto* engine = AudioEngine::create ();
		auto backend = engine->set_backend ("None (Dummy)", "KickSidechain", "");
		need (backend && backend->set_driver ("Realtime") == 0 && backend->set_sample_rate (SR) == 0 && backend->set_buffer_size (256) == 0 && engine->start () == 0, "dummy engine");

		PluginManager::instance ().refresh (false);
		PluginInfoPtr info;
		for (auto const& i : PluginManager::instance ().vst3_plugin_info ()) if (i->name == "Jay Compressor") info = i;
		need (bool (info), "Jay Compressor not discovered");
		std::cerr << "PLUGIN " << info->name << " uid=" << info->unique_id << " in=" << info->n_inputs.n_audio () << "\n";

		BusProfile bp; bp.master_out_channels = 2;
		const std::string dir = Glib::build_filename (argv[1], argv[2]);
		auto* s = new Session (*engine, dir, argv[2], &bp, "", false);
		engine->set_session (nullptr);
		std::cerr << "ANTICIPATION config=" << Config->get_anticipative_processing () << "\n";
		auto master = s->master_out ();

		auto kick_track = s->new_audio_track (1, 2, nullptr, 1, "Kick", PresentationInfo::max_order).front ();
		auto bass_track = s->new_audio_track (2, 2, nullptr, 1, "Bass Pad", PresentationInfo::max_order).front ();
		for (auto t : {kick_track, bass_track}) t->input ()->disconnect ();
		place (*s, kick_track, {write_source (*s, "Kick.wav", kick ())}, "Kick");
		place (*s, bass_track, {write_source (*s, "Bass Pad-L.wav", pad (false)), write_source (*s, "Bass Pad-R.wav", pad (true))}, "Bass Pad");
		s->set_session_extents (timepos_t (0), timepos_t (LEN));
		auto loop = new Location (*s, timepos_t (0), timepos_t (LEN), "Loop", Location::IsAutoLoop);
		s->locations ()->add (loop, true); s->set_auto_loop_location (loop);

		// Jay Compressor on the pad, keyed from the kick through a pre-fader sidechain send.
		auto plugin = info->load (*s);
		need (bool (plugin), "load Jay Compressor");
		auto comp = std::make_shared<PluginInsert> (*s, *bass_track, plugin);
		need (bass_track->add_processor (comp, PreFader) == 0, "insert compressor");
		need (bass_track->add_sidechain (comp), "add sidechain input");
		auto sc = comp->sidechain_input ();
		need (sc && sc->n_ports ().n_audio () == 2, "two sidechain ports");

		auto send = std::make_shared<Send> (*s, kick_track->pannable (), kick_track->mute_master ());
		{
			PBD::Mutex::Lock lm (AudioEngine::instance ()->process_lock ());
			send->output ()->ensure_io (kick_track->amp ()->input_streams (), false);
		}
		send->output ()->set_pretty_name ("SC Kick");
		for (uint32_t i = 0; i < sc->n_ports ().n_audio (); ++i) need (sc->audio (i)->connect (send->output ()->audio (0)->name ()) == 0, "connect kick to sidechain");
		send->set_remove_on_disconnect (true);
		need (kick_track->add_processor (send, PreFader) == 0, "add sidechain send");

		auto p = parameters (plugin);
		auto set = [&] (char const* name, float normalized) {
			need (p.count (name), name);
			ParameterDescriptor d; plugin->get_parameter_descriptor (p[name], d);
			auto c = comp->automation_control (Evoral::Parameter (PluginAutomation, 0, p[name]));
			c->set_value (d.lower + normalized * (d.upper - d.lower), PBD::Controllable::NoGroup);
			std::string text; plugin->print_parameter (p[name], text);
			std::cerr << "PARAMETER " << name << " = " << text << "\n";
		};
		const double skew = std::log (.5) / std::log (70. / 76.);
		set ("External Sidechain", 1);
		set ("Threshold", std::pow ((-26. + 70.) / 76., skew));
		set ("Ratio", 1 - 1 / 4.);
		set ("Attack", std::log (1 / .01) / std::log (1000 / .01));
		set ("Release", (std::cbrt (140.) - 1) / (std::cbrt (3000.) - 1));
		set ("Makeup", 0);
		std::cerr << "ROUTING plugin-in="  << comp->input_streams ().n_audio () << " sc-ports=" << sc->n_ports ().n_audio ()
		          << " kick feeds pad=" << kick_track->direct_feeds_according_to_reality (bass_track) << "\n";
		need (s->save_state ("") == 0, "save");
		std::cerr << "SAVED " << dir << "\n";

		// Measure only after saving, so the meters never land in the project.
		auto pre = std::make_shared<Meter> (*s, "pre"), post = std::make_shared<Meter> (*s, "post");
		pre->activate (); post->activate ();
		need (bass_track->add_processor (pre, comp) == 0 && bass_track->add_processor (post, bass_track->amp ()) == 0, "meters");
		const samplecnt_t latency = comp->effective_latency ();
		bool ok = true;
		std::vector<std::string> cases;
		auto measure = [&] (char const* label, bool expect_duck) {
			for (auto m : {pre, post}) { m->count = 0; m->armed = false; }
			s->request_locate (0, false, MustStop);
			std::this_thread::sleep_for (std::chrono::milliseconds (600));
			pre->armed = post->armed = true;
			s->request_roll ();
			std::this_thread::sleep_for (std::chrono::milliseconds (4500));
			pre->armed = post->armed = false;
			s->request_stop ();
			std::this_thread::sleep_for (std::chrono::milliseconds (300));
			std::map<samplepos_t, float> in;
			for (unsigned i = 0; i < pre->count; ++i) in[pre->at[i]] = pre->rms[i];
			double hit = 0, gap = 0; unsigned nh = 0, ng = 0;
			for (unsigned i = 0; i < post->count; ++i) {
				samplepos_t when = post->at[i];
				auto it = in.find (when);
				for (samplepos_t d : {-latency, latency}) if (it == in.end ()) { when = post->at[i] + d; it = in.find (when); }
				if (it == in.end () || it->second < 1e-4 || when < SR / 2) continue;
				const double gr = 20 * std::log10 (it->second / std::max (post->rms[i], 1e-9f));
				const samplepos_t phase = when % BEAT;
				if (phase < SR * 6 / 100) { hit += gr; ++nh; } else if (phase > SR * 32 / 100 && phase < SR * 46 / 100) { gap += gr; ++ng; }
			}
			hit /= std::max (nh, 1u); gap /= std::max (ng, 1u);
			const bool pass = nh > 10 && ng > 10 && (expect_duck ? hit > 10 && hit - gap > 8 : std::abs (hit) < 1 && std::abs (gap) < 1);
			ok &= pass;
			cases.push_back (string_compose ("{\"case\":\"%1\",\"passed\":%2,\"kick_reduction_db\":%3,\"between_reduction_db\":%4}", label, pass ? "true" : "false", hit, gap));
			std::cerr << (pass ? "PASS " : "FAIL ") << label << ": gain reduction on kicks " << hit << " dB (" << nh << " blocks), between kicks " << gap << " dB (" << ng << " blocks)";
			for (auto const& r : {kick_track, bass_track}) std::cerr << " | " << r->name () << " " << r->anticipative_status ();
			std::cerr << "\n";
		};
		engine->set_session (s);
		measure ("kick sidechain", true);
		send->gain_control ()->set_value (0, PBD::Controllable::NoGroup);
		measure ("sidechain send muted", false);
		send->gain_control ()->set_value (1, PBD::Controllable::NoGroup);
		measure ("kick sidechain again", true);
		engine->set_session (nullptr);
		delete s;
		engine->stop (); ui.reset (); ARDOUR::cleanup ();
		std::string joined;
		for (auto const& c : cases) joined += (joined.empty () ? "" : ",") + c;
		std::cout << "RESULT {\"passed\":" << (ok ? "true" : "false") << ",\"session\":\"" << dir << "\",\"cases\":[" << joined << "]}" << std::endl;
		return ok ? 0 : 2;
	} catch (std::exception const& e) { std::cerr << "FAIL " << e.what () << "\n"; return 1; }
}
