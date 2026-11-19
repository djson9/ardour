/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <array>
#include "ardour/processor.h"
#include "ardour/plugin_insert.h"
#include "ardour/fixed_delay.h"

namespace ARDOUR {

/** Parallel audio-effect chains. Topology is edited on the GUI thread under
 * the engine process lock; run() neither locks nor allocates. */
class LIBARDOUR_API AudioRack : public Processor {
public:
	struct Mapping {
		std::string plugin_id;
		uint32_t parameter;
		double lower = 0, upper = 1; // normalized in the target descriptor's domain
		std::shared_ptr<AutomationControl> control;
	};
	struct Macro {
		std::string name;
		std::string presentation = "knob";
		std::shared_ptr<AutomationControl> control;
		std::vector<Mapping> mappings;
		double applied = -1;
	};
	struct Chain {
		PBD::ID id;
		std::string name;
		float gain = 1;
		bool mute = false, solo = false;
		std::vector<std::shared_ptr<PluginInsert>> plugins;
		BufferSet buffers;
		FixedDelay delay;
	};

	AudioRack (Session&, Temporal::TimeDomainProvider const&);
	~AudioRack ();
	const std::vector<std::shared_ptr<Chain>>& chains () const { return _chains; }
	const std::array<Macro, 8>& macros () const { return _macros; }
	bool add_chain (std::string const&);
	bool remove_chain (size_t);
	bool set_chain (size_t, std::string const&, float gain, bool mute, bool solo);
	bool add_plugin (size_t, std::shared_ptr<PluginInsert>);
	bool add_plugins (size_t chain, std::vector<std::shared_ptr<PluginInsert>> const&, size_t position, bool activate);
	bool remove_plugin (size_t, size_t);
	bool move_plugin (size_t, size_t, int direction);
	bool map_parameter (size_t, std::shared_ptr<PluginInsert>, uint32_t, double lower = 0, double upper = 1);
	bool unmap_parameter (size_t, size_t);
	void rename_macro (size_t, std::string const&);
	void set_macro_presentation (size_t, std::string const&);
	void clear_macro (size_t);
	void regenerate_ids ();
	bool requires_fixed_sized_buffers () const;
	bool can_ungroup () const;
	bool contains (std::shared_ptr<PluginInsert>) const;

	bool can_support_io_configuration (ChanCount const&, ChanCount&);
	bool configure_io (ChanCount, ChanCount);
	int set_block_size (pframes_t);
	void run (BufferSet&, samplepos_t, samplepos_t, double, pframes_t, bool);
	void silence (samplecnt_t, samplepos_t);
	void flush ();
	void realtime_locate (bool);
	void realtime_handle_transport_stopped ();
	void set_owner (SessionObject*);
	void monitoring_changed ();
	void set_loop (Location*);
	samplecnt_t signal_latency () const;
	bool latency_needs_update () const;
	void update_latency (); // process lock held; non-realtime
	int set_state (XMLNode const&, int);
	PBD::Signal<void()> Changed;

protected:
	XMLNode& state () const;
private:
	friend class Route;
	bool configure_chain (Chain&, ChanCount);
	bool editable () const;
	void changed ();
	void queue_latency ();
	void forget_plugin (std::shared_ptr<PluginInsert>);
	Temporal::TimeDomainProvider const& _tdp;
	std::vector<std::shared_ptr<Chain>> _chains;
	std::array<Macro, 8> _macros;
	BufferSet _input;
	pframes_t _block_size;
};
}
