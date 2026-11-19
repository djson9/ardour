/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <algorithm>
#include <cmath>
#include "pbd/compose.h"
#include "pbd/xml++.h"
#include "pbd/types_convert.h"
#include "ardour/route.h"
#include "ardour/audio_rack.h"
#include "ardour/audio_buffer.h"
#include "ardour/audioengine.h"
#include "ardour/session.h"

using namespace ARDOUR;
using namespace PBD;

namespace {
// A child can move between racks and routes while keeping its plugin instance.
void watch_plugin_bypass (PluginInsert& plugin)
{
	plugin.ActiveChanged.connect_same_thread (plugin, [&plugin] () {
		if (auto route = dynamic_cast<Route*> (plugin.owner ())) route->processor_latency_changed ();
	});
}

// Macros have no timeline lane yet. GUI writes and DSP reads share only this
// atomic value; the base control retains notification and widget behavior.
class RackMacroControl : public AutomationControl {
public:
	RackMacroControl (Session& s, Evoral::Parameter p, ParameterDescriptor const& d, std::string const& name)
		: AutomationControl (s, p, d, std::shared_ptr<AutomationList> (), name), _current (d.normal) {}
	double get_value () const override { return _current.load (std::memory_order_relaxed); }
	double get_save_value () const override { return get_value (); }
protected:
	void actually_set_value (double v, Controllable::GroupControlDisposition gcd) override {
		if (!std::isfinite (v)) return;
		v = std::max (0., std::min (1., v));
		_current.store (v, std::memory_order_relaxed); AutomationControl::actually_set_value (v, gcd);
	}
private:
	std::atomic<double> _current;
};
}


AudioRack::AudioRack (Session& s, Temporal::TimeDomainProvider const& tdp)
	: Processor (s, "Audio Rack", tdp), _tdp (tdp), _block_size (s.get_block_size ())
{
	for (size_t i = 0; i < _macros.size (); ++i) {
		auto& m = _macros[i];
		m.name = string_compose ("Macro %1", i + 1);
		ParameterDescriptor d;
		d.lower = 0; d.upper = 1; d.normal = 0; d.label = m.name;
		d.update_steps ();
		Evoral::Parameter p (PluginAutomation, 0, i);
		m.control = std::make_shared<RackMacroControl> (s, p, d, m.name);
		add_control (m.control);
	}
	auto c = std::make_shared<Chain> ();
	c->name = "Chain 1";
	_chains.push_back (c);
}

AudioRack::~AudioRack ()
{
	for (auto const& c : _chains) for (auto const& p : c->plugins) p->drop_references ();
}

bool AudioRack::editable () const { return !_session.actively_recording (); }
void AudioRack::changed () { _session.set_dirty (); queue_latency (); Changed (); }

bool AudioRack::can_ungroup () const
{
	if (!enabled () || _chains.size () != 1 || _chains.front ()->gain != 1 || _chains.front ()->mute) return false;
	for (auto const& macro : _macros) if (!macro.mappings.empty ()) return false;
	return true;
}

bool AudioRack::contains (std::shared_ptr<PluginInsert> p) const
{
	for (auto const& c : _chains) if (std::find (c->plugins.begin (), c->plugins.end (), p) != c->plugins.end ()) return true;
	return false;
}

bool AudioRack::add_chain (std::string const& name)
{
	if (!editable ()) return false;
	auto c = std::make_shared<Chain> (); c->name = name;
	{
		Mutex::Lock lock (AudioEngine::instance ()->process_lock ());
		_session.retire_anticipation_for_edit ();
		if (_configured && !configure_chain (*c, _configured_input)) return false;
		_chains.push_back (c); update_latency ();
	}
	changed (); return true;
}

bool AudioRack::remove_chain (size_t index)
{
	if (!editable () || index >= _chains.size () || _chains.size () == 1) return false;
	auto removed = _chains[index];
	{
		Mutex::Lock lock (AudioEngine::instance ()->process_lock ());
		_session.retire_anticipation_for_edit ();
		for (auto const& p : removed->plugins) forget_plugin (p);
		_chains.erase (_chains.begin () + index); update_latency ();
	}
	for (auto const& p : removed->plugins) p->drop_references ();
	changed (); return true;
}

bool AudioRack::set_chain (size_t i, std::string const& name, float gain, bool mute, bool solo)
{
	if (!editable () || i >= _chains.size () || !std::isfinite (gain) || gain < 0 || gain > 4) return false;
	{
		Mutex::Lock lock (AudioEngine::instance ()->process_lock ());
		_session.retire_anticipation_for_edit ();
		auto& c = *_chains[i]; c.name = name; c.gain = gain; c.mute = mute; c.solo = solo;
	}
	changed (); return true;
}

bool AudioRack::configure_chain (Chain& c, ChanCount in)
{
	ChanCount audio (DataType::AUDIO, in.n_audio ());
	ChanCount required = audio;
	for (auto const& p : c.plugins) {
		if (!p->plugin () || p->is_instrument () || p->has_sidechain () || p->sidechain_input_pins ().n_total () ||
		    p->natural_input_streams ().n_midi () || p->natural_output_streams ().n_midi ()) return false;
		p->set_strict_io (true);
		ChanCount out;
		if (!p->can_support_io_configuration (audio, out) || out != audio || !p->configure_io (audio, out)) return false;
		required = ChanCount::max (required, p->required_buffers ());
		p->set_block_size (_block_size);
	}
	c.buffers.ensure_buffers (required, _block_size);
	c.buffers.set_count (audio);
	return true;
}

bool AudioRack::add_plugin (size_t index, std::shared_ptr<PluginInsert> p)
{
	if (!editable () || index >= _chains.size () || !p || contains (p)) return false;
	{
		Mutex::Lock lock (AudioEngine::instance ()->process_lock ());
		_session.retire_anticipation_for_edit ();
		auto& c = *_chains[index]; c.plugins.push_back (p);
		if (_configured && !configure_chain (c, _configured_input)) {
			c.plugins.pop_back (); configure_chain (c, _configured_input); return false;
		}
		p->set_owner (owner ()); p->activate (); update_latency ();
	}
	watch_plugin_bypass (*p);
	changed (); return true;
}

void AudioRack::forget_plugin (std::shared_ptr<PluginInsert> p)
{
	for (auto& m : _macros) {
		m.mappings.erase (std::remove_if (m.mappings.begin (), m.mappings.end (), [p] (Mapping const& t) { return t.plugin_id == p->id ().to_s (); }), m.mappings.end ());
	}
}

bool AudioRack::remove_plugin (size_t chain, size_t index)
{
	if (!editable () || chain >= _chains.size () || index >= _chains[chain]->plugins.size ()) return false;
	auto p = _chains[chain]->plugins[index];
	{
		Mutex::Lock lock (AudioEngine::instance ()->process_lock ()); _session.retire_anticipation_for_edit ();
		forget_plugin (p); _chains[chain]->plugins.erase (_chains[chain]->plugins.begin () + index); update_latency ();
	}
	p->drop_references (); changed (); return true;
}

bool AudioRack::move_plugin (size_t chain, size_t index, int direction)
{
	if (!editable () || chain >= _chains.size () || (direction != -1 && direction != 1)) return false;
	auto& ps = _chains[chain]->plugins;
	int next = int (index) + direction;
	if (index >= ps.size () || next < 0 || next >= int (ps.size ())) return false;
	{
		Mutex::Lock lock (AudioEngine::instance ()->process_lock ()); _session.retire_anticipation_for_edit ();
		std::swap (ps[index], ps[next]);
	}
	changed (); return true;
}

bool AudioRack::map_parameter (size_t macro, std::shared_ptr<PluginInsert> p, uint32_t parameter, double lower, double upper)
{
	if (!editable () || macro >= 8 || !contains (p) || !std::isfinite (lower) || !std::isfinite (upper) ||
	    lower < 0 || lower > 1 || upper < 0 || upper > 1 || parameter >= p->plugin ()->parameter_count () ||
	    !p->plugin ()->parameter_is_input (parameter) || !p->plugin ()->parameter_is_control (parameter)) return false;
	auto control = p->automation_control (Evoral::Parameter (PluginAutomation, 0, parameter));
	if (!control || control->desc ().upper <= control->desc ().lower) return false;
	{
		Mutex::Lock lock (AudioEngine::instance ()->process_lock ()); _session.retire_anticipation_for_edit ();
		for (auto& m : _macros) m.mappings.erase (std::remove_if (m.mappings.begin (), m.mappings.end (), [control] (Mapping const& t) { return t.control == control; }), m.mappings.end ());
		Mapping t; t.plugin_id = p->id ().to_s (); t.parameter = parameter; t.lower = lower; t.upper = upper; t.control = control;
		_macros[macro].mappings.push_back (t);
		// Learning never jumps the parameter. The next macro move applies it.
		_macros[macro].applied = _macros[macro].control->get_value ();
	}
	changed (); return true;
}

bool AudioRack::unmap_parameter (size_t macro, size_t target)
{
	if (!editable () || macro >= 8 || target >= _macros[macro].mappings.size ()) return false;
	{
		Mutex::Lock lock (AudioEngine::instance ()->process_lock ()); _session.retire_anticipation_for_edit ();
		auto& m = _macros[macro].mappings; m.erase (m.begin () + target);
	}
	changed (); return true;
}
void AudioRack::clear_macro (size_t i) { if (i < 8) while (!_macros[i].mappings.empty ()) if (!unmap_parameter (i, 0)) break; }
void AudioRack::rename_macro (size_t i, std::string const& name) { if (i < 8 && editable ()) { _macros[i].name = name; changed (); } }

bool AudioRack::can_support_io_configuration (ChanCount const& in, ChanCount& out)
{
	if (in.n_audio () == 0) return false;
	ChanCount audio (DataType::AUDIO, in.n_audio ());
	for (auto const& c : _chains) for (auto const& p : c->plugins) {
		ChanCount supported;
		if (!p->can_support_io_configuration (audio, supported) || supported != audio) return false;
	}
	out = in; return true;
}

bool AudioRack::configure_io (ChanCount in, ChanCount out)
{
	if (in != out || !in.n_audio ()) return false;
	for (auto const& c : _chains) if (!configure_chain (*c, in)) return false;
	_input.ensure_buffers (DataType::AUDIO, in.n_audio (), _block_size);
	_input.set_count (ChanCount (DataType::AUDIO, in.n_audio ()));
	Processor::configure_io (in, out); update_latency (); return true;
}

int AudioRack::set_block_size (pframes_t n)
{
	_block_size = n;
	if (_configured) return configure_io (_configured_input, _configured_output) ? 0 : -1;
	return 0;
}

static samplecnt_t chain_latency (AudioRack::Chain const& c)
{
	samplecnt_t n = 0;
	for (auto const& p : c.plugins) if (p->active ()) n += p->effective_latency ();
	return n;
}

samplecnt_t AudioRack::signal_latency () const
{
	samplecnt_t n = 0;
	for (auto const& c : _chains) n = std::max (n, chain_latency (*c));
	return n;
}

bool AudioRack::latency_needs_update () const
{
	auto n = signal_latency ();
	for (auto const& c : _chains) if (c->delay.delay () != n - chain_latency (*c)) return true;
	return false;
}

void AudioRack::update_latency ()
{
	if (!_configured) return;
	auto n = signal_latency ();
	for (auto const& c : _chains) c->delay.set (ChanCount (DataType::AUDIO, _configured_input.n_audio ()), n - chain_latency (*c));
}

void AudioRack::run (BufferSet& bufs, samplepos_t start, samplepos_t end, double speed, pframes_t n, bool)
{
	if (!check_active () || !_configured) return;
	for (auto& m : _macros) {
		m.control->automation_run (start, n);
		double v = m.control->get_value ();
		if (v == m.applied) continue;
		m.applied = v;
		for (auto const& t : m.mappings) {
			if (t.control->automation_playback ()) continue;
			auto const& d = t.control->desc ();
			double value = d.from_interface (t.lower + v * (t.upper - t.lower));
			if (d.enumeration && d.scale_points && !d.scale_points->empty ()) {
				double distance = HUGE_VAL; const double requested = value;
				for (auto const& point : *d.scale_points) if (std::abs (point.second - requested) < distance) { distance = std::abs (point.second - requested); value = point.second; }
			} else if (d.toggled) value = value >= (d.lower + d.upper) * .5 ? d.upper : d.lower;
			else if (d.integer_step) value = std::round (value);
			t.control->set_value (value, Controllable::NoGroup);
		}
	}
	_input.read_from (bufs, n, DataType::AUDIO);
	for (uint32_t ch = 0; ch < _configured_input.n_audio (); ++ch) bufs.get_audio (ch).silence (n);
	const samplecnt_t rack_latency = signal_latency ();
	bool solo = false; for (auto const& c : _chains) solo |= c->solo;
	for (auto const& c : _chains) {
		c->buffers.set_count (_input.count ()); c->buffers.read_from (_input, n);
		samplecnt_t latency = 0;
		for (auto const& p : c->plugins) {
			if (p->active ()) latency += p->effective_latency ();
			const samplecnt_t offset = speed < 0 ? latency - rack_latency : rack_latency - latency;
			p->run (c->buffers, start + offset, end + offset, speed, n, true);
		}
		for (uint32_t ch = 0; ch < _configured_input.n_audio (); ++ch) {
			auto& b = c->buffers.get_audio (ch);
			// FixedDelay processes at most 8192 samples per call.
			for (pframes_t off = 0; off < n; off += 8192) c->delay.delay (DataType::AUDIO, ch, b, b, std::min<pframes_t> (8192, n - off), off, off);
			if (!c->mute && (!solo || c->solo)) bufs.get_audio (ch).accumulate_with_gain_from (b, n, c->gain);
		}
	}
}

void AudioRack::flush () { for (auto const& c : _chains) { c->delay.flush (); for (auto const& p : c->plugins) p->flush (); } }
void AudioRack::silence (samplecnt_t n, samplepos_t start) { for (auto const& c : _chains) { c->delay.flush (); for (auto const& p : c->plugins) p->silence (n, start); } }
void AudioRack::realtime_locate (bool b) { for (auto const& c : _chains) { c->delay.flush (); for (auto const& p : c->plugins) p->realtime_locate (b); } }
void AudioRack::realtime_handle_transport_stopped () { for (auto const& c : _chains) for (auto const& p : c->plugins) p->realtime_handle_transport_stopped (); }
void AudioRack::set_owner (SessionObject* o) { Processor::set_owner (o); for (auto const& c : _chains) for (auto const& p : c->plugins) p->set_owner (o); }

XMLNode& AudioRack::state () const
{
	XMLNode& node = Processor::state (); node.set_property ("type", "audio-rack"); node.set_property ("rack-version", 1);
	for (auto const& c : _chains) {
		auto& cn = *node.add_child ("Chain"); cn.set_property ("id", c->id); cn.set_property ("name", c->name);
		cn.set_property ("gain", c->gain); cn.set_property ("mute", c->mute); cn.set_property ("solo", c->solo);
		for (auto const& p : c->plugins) cn.add_child_nocopy (p->get_state ());
	}
	for (size_t i = 0; i < 8; ++i) {
		auto const& m = _macros[i]; auto& mn = *node.add_child ("Macro");
		mn.set_property ("index", uint32_t (i)); mn.set_property ("name", m.name); mn.set_property ("presentation", m.presentation); mn.set_property ("value", m.control->get_value ());
		for (auto const& t : m.mappings) {
			auto& tn = *mn.add_child ("Mapping"); tn.set_property ("plugin", t.plugin_id); tn.set_property ("parameter", t.parameter);
			tn.set_property ("lower", t.lower); tn.set_property ("upper", t.upper);
		}
	}
	return node;
}

int AudioRack::set_state (XMLNode const& node, int version)
{
	int schema = 0;
	if (!node.get_property ("rack-version", schema) || schema != 1 || Processor::set_state (node, version)) return -1;
	_chains.clear ();
	std::map<std::string, std::shared_ptr<PluginInsert>> loaded;
	for (auto const* cn : node.children ()) if (cn->name () == "Chain") {
		auto c = std::make_shared<Chain> ();
		cn->get_property ("id", c->id); cn->get_property ("name", c->name); cn->get_property ("gain", c->gain);
		cn->get_property ("mute", c->mute); cn->get_property ("solo", c->solo);
		if (!std::isfinite (c->gain) || c->gain < 0 || c->gain > 4) return -1;
		for (auto const* pn : cn->children ()) if (pn->name () == Processor::state_node_name) {
			if (_session.get_disable_all_loaded_plugins ()) return -1;
			auto p = std::make_shared<PluginInsert> (_session, _tdp); p->set_owner (owner ());
			if (p->set_state (*pn, version)) return -1;
			std::string saved_id;
			if (!pn->get_property ("id", saved_id) || loaded.count (saved_id)) return -1;
			loaded[saved_id] = p;
			c->plugins.push_back (p);
			watch_plugin_bypass (*p);
		}
		_chains.push_back (c);
	}
	if (_chains.empty ()) return -1;
	for (auto& m : _macros) m.mappings.clear ();
	for (auto const* mn : node.children ()) if (mn->name () == "Macro") {
		uint32_t i; double value;
		if (!mn->get_property ("index", i) || i >= 8 || !mn->get_property ("value", value) || !std::isfinite (value) || value < 0 || value > 1) return -1;
		auto& m = _macros[i]; mn->get_property ("name", m.name);
		mn->get_property ("presentation", m.presentation);
		if (m.presentation != "knob" && m.presentation != "toggle" && m.presentation != "choices") return -1; m.control->set_value_unchecked (value); m.applied = value;
		for (auto const* tn : mn->children ()) if (tn->name () == "Mapping") {
			Mapping t;
			if (!tn->get_property ("plugin", t.plugin_id) || !tn->get_property ("parameter", t.parameter) ||
			    !tn->get_property ("lower", t.lower) || !tn->get_property ("upper", t.upper) ||
			    !std::isfinite (t.lower) || !std::isfinite (t.upper) || t.lower < 0 || t.lower > 1 || t.upper < 0 || t.upper > 1) return -1;
			auto found = loaded.find (t.plugin_id);
			if (found != loaded.end ()) {
				auto p = found->second;
				if (t.parameter >= p->plugin ()->parameter_count () || !p->plugin ()->parameter_is_input (t.parameter)) return -1;
				t.plugin_id = p->id ().to_s ();
				t.control = p->automation_control (Evoral::Parameter (PluginAutomation, 0, t.parameter));
			}
			if (!t.control) return -1;
			for (auto const& existing : _macros) for (auto const& map : existing.mappings) if (map.control == t.control) return -1;
			m.mappings.push_back (t);
		}
	}
	return 0;
}

// Copy only after loading external plugin state using its original IDs.
void AudioRack::regenerate_ids ()
{
	reset_id ();
	for (auto& m : _macros) m.control->reset_id ();
	for (auto const& c : _chains) {
		c->id = PBD::ID ();
		for (auto const& p : c->plugins) {
			std::string old = p->id ().to_s (); p->update_id (PBD::ID ());
			for (auto const& entry : p->controls ()) {
				if (auto c = std::dynamic_pointer_cast<AutomationControl> (entry.second)) { c->reset_id (); if (c->alist ()) c->alist ()->reset_id (); }
			}
			for (auto& m : _macros) for (auto& t : m.mappings) if (t.plugin_id == old) t.plugin_id = p->id ().to_s ();
		}
	}
}
bool AudioRack::requires_fixed_sized_buffers () const
{
	for (auto const& c : _chains) for (auto const& p : c->plugins) if (p->requires_fixed_sized_buffers ()) return true;
	return false;
}

void AudioRack::queue_latency () { if (auto route = dynamic_cast<Route*> (owner ())) route->processor_latency_changed (); }

void AudioRack::set_macro_presentation (size_t i, std::string const& kind)
{
	if (editable () && i < 8 && (kind == "knob" || kind == "toggle" || kind == "choices")) { _macros[i].presentation = kind; changed (); }
}

void AudioRack::monitoring_changed () { for (auto const& c : _chains) for (auto const& p : c->plugins) p->monitoring_changed (); }
void AudioRack::set_loop (Location* loc) { Processor::set_loop (loc); for (auto const& c : _chains) for (auto const& p : c->plugins) p->set_loop (loc); }
