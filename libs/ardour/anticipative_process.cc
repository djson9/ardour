/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/anticipative_process.h"

#include <algorithm>
#include <stdexcept>

#include "ardour/record_enable_control.h"
#include "ardour/audio_buffer.h"
#include "ardour/amp.h"
#include "ardour/midi_buffer.h"
#include "ardour/midi_track.h"
#include "ardour/delivery.h"
#include "ardour/internal_send.h"
#include "ardour/internal_return.h"
#include "ardour/disk_reader.h"
#include "ardour/meter.h"
#include "ardour/plugin_insert.h"
#include "ardour/sidechain.h"
#include "ardour/port_insert.h"
#include "ardour/send.h"
#include "ardour/port.h"
#include "ardour/port_set.h"
#include "ardour/audioengine.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/track.h"
#include "ardour/location.h"
#include "ardour/triggerbox.h"

using namespace ARDOUR;

AnticipativeProcess::Timeline::Timeline (Session& session, samplepos_t start, bool preroll)
	: _block (session.engine ().samples_per_cycle ()), _start (start),
	  _preroll (preroll ? session.worst_latency_preroll_buffer_size_ceil () : 0)
{
	if (!_block) { throw std::invalid_argument ("Anticipation needs a running backend"); }
	for (auto const& route : *session.get_routes ()) {
		if (route->active ()) { _boundaries.push_back (std::max (samplecnt_t (0), _preroll - route->playback_latency ())); }
	}
	_boundaries.push_back (_preroll);
	std::sort (_boundaries.begin (), _boundaries.end ());
	if (session.get_play_loop ()) {
		if (auto loop = session.locations ()->auto_loop_location ()) { _loop_start = loop->start_sample (); _loop_end = loop->end_sample (); }
	}
}

AnticipativeProcess::Queue::Range
AnticipativeProcess::Timeline::at (samplecnt_t elapsed) const
{
	samplepos_t project = _start - _preroll + elapsed;
	pframes_t frames = _block - elapsed % _block;
	for (auto boundary : _boundaries) {
		if (boundary > elapsed) { frames = std::min (samplecnt_t (frames), boundary - elapsed); break; }
	}
	if (_loop_end > _loop_start) {
		if (project >= _loop_end) { project = _loop_start + (project - _loop_start) % (_loop_end - _loop_start); }
		frames = std::min (samplecnt_t (frames), _loop_end - project);
	}
	return {project, elapsed, frames};
}

AnticipativeProcess::Queue::Range
AnticipativeProcess::Timeline::first (Route const& route) const
{
	/* Track no-roll preroll still advances instruments, effects and their tails.
	 * Only the disk reader is held; skipping the entire route loses DSP history. */
	return at (0);
}

AnticipativeProcess::Queue::Range
AnticipativeProcess::Timeline::next (Queue::Range const& previous) const
{
	return at (previous.continuous + previous.frames);
}

struct AnticipativeProcess::RouteAdapter {
	struct Feed {
		Node* source;
		Queue::Reader* reader;
		std::unique_ptr<BufferSet> input;
	};
	struct Connection { size_t feed; DataType type; uint32_t source, destination; };
	struct SideInput { RouteAdapter* owner; std::shared_ptr<SideChain> sidechain; std::vector<Connection> connections; };
	struct Tap {
		std::shared_ptr<Processor> processor;
		std::shared_ptr<InternalSend> send;
		ChanCount offset, count;
		std::unique_ptr<BufferSet> data, delivery;
	};
	AnticipativeProcess& plan;
	std::shared_ptr<Route> route;
	Node* node {nullptr};
	BufferSet work, main, audible, future_input;
	ChanCount channels;
	std::vector<Feed> feeds;
	std::vector<Connection> connections;
	std::vector<Tap> taps;
	std::vector<std::unique_ptr<SideInput>> side_inputs;
	ChanCount processing_channels;
	bool requires_live {false}, attached {false}, rendering_future {false};
	bool captured_live {false}; // callback only; read by the live DSP owner
	static thread_local RouteAdapter* current;
	bool wired {false};
	bool track;
	samplepos_t continuous;
	pframes_t max_frames;
	samplecnt_t preroll, reader_start;
	samplepos_t last_project_end;

	RouteAdapter (AnticipativeProcess& p, std::shared_ptr<Route> r, Queue::Range const& first)
		: plan (p), route (r), channels (ChanCount::max (r->max_processor_streams (), ChanCount::max (r->n_inputs (), r->n_outputs ()))),
		  track (bool (std::dynamic_pointer_cast<Track> (r))), continuous (first.continuous),
		  max_frames (p._session.engine ().samples_per_cycle ()),
		  preroll (std::max (samplecnt_t (0), first.continuous + p._session.transport_sample () - first.project)),
		  reader_start (track ? std::max (samplecnt_t (0), preroll - r->playback_latency ()) : 0),
		  last_project_end (first.project)
	{
		processing_channels = channels;
		if (r->triggerbox () && !r->triggerbox ()->empty ()) { requires_live = true; }
		if (r->surround_send () || r->is_surround_master ()) { requires_live = true; }
		ChanCount packed = r->n_outputs ();
		bool suffix = !track;
		r->foreach_processor ([&] (std::weak_ptr<Processor> weak) {
			auto proc = weak.lock ();
			if (std::dynamic_pointer_cast<DiskReader> (proc)) { suffix = true; }
			auto send = std::dynamic_pointer_cast<InternalSend> (proc);
			auto meter = std::dynamic_pointer_cast<PeakMeter> (proc);
			auto plugin = std::dynamic_pointer_cast<PluginInsert> (proc);
			if ((!send && std::dynamic_pointer_cast<Send> (proc)) || std::dynamic_pointer_cast<PortInsert> (proc)) { requires_live = true; }
			if (plugin && plugin->has_sidechain ()) { std::unique_ptr<SideInput> side (new SideInput); side->owner = this; side->sidechain = plugin->sidechain (); side_inputs.push_back (std::move (side)); }
			if (!send && !(meter && suffix)) { return; }
			if (send && !suffix) { requires_live = true; }
			Tap tap; tap.processor = proc; tap.send = send; tap.offset = packed;
			tap.count = send ? send->get_buffers ().count () : proc->input_streams ();
			tap.data.reset (new BufferSet); tap.delivery.reset (new BufferSet);
			allocate (*tap.data, tap.count, max_frames); allocate (*tap.delivery, tap.count, max_frames);
			packed += tap.count; taps.push_back (std::move (tap));
		});
		channels = ChanCount::max (channels, packed);
		allocate (work, processing_channels, max_frames); allocate (future_input, processing_channels, max_frames);
		allocate (main, r->n_outputs (), max_frames); allocate (audible, channels, max_frames);
		main.set_count (r->n_outputs ());
	}
	~RouteAdapter () {
		if (!attached) { return; }
		/* An exception before Amp::run may leave automation prepared in the
		 * worker's private storage. Retiring the owner invalidates that storage. */
		route->amp ()->set_gain_automation_buffer (nullptr);
		route->trim ()->set_gain_automation_buffer (nullptr);
		route->set_render_consumer (nullptr, nullptr); route->set_render_observer (nullptr, nullptr); route->main_outs ()->set_render_buffers (nullptr);
		for (auto& tap : taps) { if (tap.send) { tap.send->set_manual_cycle_start (false); } }
		for (auto& side : side_inputs) { side->sidechain->set_cached_input_reader (nullptr, nullptr); }
	}
	static void allocate (BufferSet& b, ChanCount c, pframes_t n) {
		b.ensure_buffers (DataType::AUDIO, c.n_audio (), n); b.ensure_buffers (DataType::MIDI, c.n_midi (), 8192); b.set_count (c);
	}
	size_t feed_for (Node& source) {
		for (size_t i = 0; i < feeds.size (); ++i) { if (feeds[i].source == &source) { return i; } }
		Feed f; f.source = &source; f.reader = &source.queue->add_reader (); f.input.reset (new BufferSet);
		allocate (*f.input, source.adapter->channels, max_frames); feeds.push_back (std::move (f)); return feeds.size () - 1;
	}
	void wire () {
		if (wired) { return; }
		wired = true;
		/* DiskReader replaces the playback input; physical capture remains live. */
		if (!track) for (auto& source : plan._nodes) {
			if (!source->adapter || source.get () == node) { continue; }
			for (auto const& tap : source->adapter->taps) {
				if (tap.send && tap.send->target_route () == route) { feed_for (*source); }
			}
		}
		if (!track) { wire_io (route->input (), connections); }
		for (auto& side : side_inputs) { wire_io (side->sidechain->input (), side->connections); }
	}
	void wire_io (std::shared_ptr<IO> input, std::vector<Connection>& target) {
		for (DataType::iterator type = DataType::begin (); type != DataType::end (); ++type) {
			for (uint32_t destination = 0; destination < input->n_ports ().get (*type); ++destination) {
				auto port = input->ports ()->port (*type, destination);
				std::vector<std::string> names; port->get_connections (names);
				for (auto const& name : names) {
					bool found = false;
					for (auto const& candidate : plan._nodes) {
						if (!candidate->adapter || candidate.get () == node) { continue; }
						for (uint32_t source = 0; source < candidate->route->n_outputs ().get (*type); ++source) {
							auto source_name = candidate->route->output ()->ports ()->port (*type, source)->name ();
							if (source_name != name && plan._session.engine ().make_port_name_non_relative (source_name) != name) { continue; }
							found = true; size_t feed = 0;
							for (; feed < feeds.size () && feeds[feed].source != candidate.get (); ++feed) {}
							if (feed == feeds.size ()) {
								Feed f; f.source = candidate.get (); f.reader = &candidate->queue->add_reader (); f.input.reset (new BufferSet);
									allocate (*f.input, candidate->adapter->channels, max_frames); feeds.push_back (std::move (f));
							}
							target.push_back ({feed, *type, source, destination});
						}
					}
					if (!found) { node->requires_live = true; }
				}
			}
		}
	}
	static void side_input (void* context, BufferSet& output, ChanCount const& offset, samplepos_t, pframes_t nframes) {
		auto& side = *static_cast<SideInput*> (context);
		if (!side.owner->rendering_future) { side.sidechain->input ()->collect_input (output, nframes, offset); return; }
		for (DataType::iterator type = DataType::begin (); type != DataType::end (); ++type) {
			for (uint32_t c = 0; c < side.sidechain->input ()->n_ports ().get (*type); ++c) {
				auto& buffer = output.get_available (*type, c + offset.get (*type));
				if (*type == DataType::MIDI) { static_cast<MidiBuffer&> (buffer).clear (); } else { buffer.silence (nframes); }
			}
		}
		for (auto const& connection : side.connections) {
			auto const& source = side.owner->feeds[connection.feed].input->get_available (connection.type, connection.source);
			uint32_t c = connection.destination + offset.get (connection.type);
			if (connection.type == DataType::AUDIO) { output.get_audio (c).accumulate_from (static_cast<AudioBuffer const&> (source), nframes); }
			else { output.get_midi (c).merge_from (source, nframes); }
		}
	}
	bool ready (Queue::Range const& range) {
		for (auto& feed : feeds) {
			/* Tracks remain silent until their existing PDC preroll boundary.
			 * Buses still run during that prefix and must not wait for a range
			 * the source will never render. Timeline splits at every boundary. */
			if (range.continuous + range.frames <= feed.source->first.continuous) {
				feed.input->silence (range.frames, 0);
				for (uint32_t c = 0; c < feed.input->count ().n_midi (); ++c) { feed.input->get_midi (c).clear (); }
				continue;
			}
			if (!feed.source->queue->peek (*feed.reader, *feed.input, range, plan.generation ())) { return false; }
		}
		future_input.set_count (processing_channels); future_input.silence (range.frames, 0);
		for (uint32_t c = 0; c < processing_channels.n_midi (); ++c) { future_input.get_midi (c).clear (); }
		for (auto const& connection : connections) {
			auto& source = feeds[connection.feed].input->get_available (connection.type, connection.source);
			if (connection.type == DataType::AUDIO) { future_input.get_audio (connection.destination).accumulate_from (static_cast<AudioBuffer&> (source), range.frames); }
			else { future_input.get_midi (connection.destination).merge_from (source, range.frames); }
		}
		return true;
	}
	void render (BufferSet& b, Queue::Range const& range, bool live) {
		struct Scope { RouteAdapter* previous; Scope(RouteAdapter* self):previous(current){current=self;} ~Scope(){current=previous;} } scope(this);
		rendering_future = !live;
		if (range.project != last_project_end) { route->realtime_locate (true); }
		last_project_end = range.project + range.frames;
		work.set_count (processing_channels); work.silence (range.frames, 0);
		for (uint32_t c = 0; c < processing_channels.n_midi (); ++c) { work.get_midi (c).clear (); }
		work.read_from (live ? b : future_input, range.frames);
		route->render_anticipative (work, range.project, range.project + range.frames, range.frames, live, range.continuous >= reader_start, live && captured_live, range.continuous >= preroll);
		b.set_count (channels); b.silence (range.frames, 0);
		for (uint32_t c = 0; c < channels.n_midi (); ++c) { b.get_midi (c).clear (); }
		pack (b, main, ChanCount::ZERO, range.frames);
		for (auto const& tap : taps) { pack (b, *tap.data, tap.offset, range.frames); }
		for (auto& feed : feeds) { feed.source->queue->release_until (*feed.reader, range.continuous + range.frames); }
	}
	static void pack (BufferSet& to, BufferSet const& from, ChanCount offset, pframes_t n) {
		for (DataType::iterator type = DataType::begin (); type != DataType::end (); ++type) {
			for (uint32_t c = 0; c < from.count ().get (*type); ++c) { to.get_available (*type, c + offset.get (*type)).read_from (from.get_available (*type, c), n); }
		}
	}
	static void observe (void* context, Processor const* processor, BufferSet const& input, pframes_t n) {
		auto& self = *static_cast<RouteAdapter*> (context);
		for (auto& tap : self.taps) {
			if (tap.processor.get () != processor) { continue; }
			for (uint32_t c = 0; c < tap.count.n_midi (); ++c) { tap.data->get_midi (c).clear (); }
			tap.data->read_from (tap.send ? tap.send->get_buffers () : input, n);
		}
	}
	static bool consume (void* context, BufferSet& input, samplepos_t start, samplepos_t end, pframes_t nframes, bool moving) {
		auto& self = *static_cast<RouteAdapter*> (context);
		self.audible.set_count (self.channels);
		if (!moving && !self.plan._session.remaining_latency_preroll ()) {
			self.audible.silence (nframes, 0);
			for (uint32_t c = 0; c < self.channels.n_midi (); ++c) { self.audible.get_midi (c).clear (); }
			self.route->output ()->silence (nframes); return true;
		}
		/* Keep immediate MIDI queued through drain. The first complete live
		 * callback snapshots it once for both DiskWriter and instrument delivery. */
		self.captured_live = self.node->queue->state () == Queue::State::Live;
		self.route->capture_anticipative_input (input, start, end, nframes, self.captured_live);
		if (!self.plan.running ()) {
			self.audible.silence (nframes, 0);
			for (uint32_t c = 0; c < self.channels.n_midi (); ++c) { self.audible.get_midi (c).clear (); }
			self.route->output ()->silence (nframes); return true;
		}
		for (uint32_t c = 0; c < self.channels.n_midi (); ++c) { self.audible.get_midi (c).clear (); }
		self.audible.read_from (input, nframes);
		self.plan.process (*self.node, self.audible, {start, self.continuous, nframes});
		self.continuous += nframes;
		for (auto& tap : self.taps) {
			if (tap.send) { continue; }
			for (DataType::iterator type = DataType::begin (); type != DataType::end (); ++type) {
				for (uint32_t c = 0; c < tap.count.get (*type); ++c) { tap.delivery->get_available (*type, c).read_from (self.audible.get_available (*type, c + tap.offset.get (*type)), nframes); }
			}
			tap.processor->run (*tap.delivery, start, end, 1, nframes, false);
		}
		self.audible.set_count (self.route->n_outputs ());
		self.route->deliver_anticipative (self.audible, nframes);
		return true;
	}
};

thread_local AnticipativeProcess::RouteAdapter* AnticipativeProcess::RouteAdapter::current = nullptr;

bool
AnticipativeProcess::cached_send (void* context, InternalSend const* send, BufferSet& destination, samplepos_t, pframes_t nframes)
{
	auto& plan = *static_cast<AnticipativeProcess*> (context);
	for (auto const& node : plan._nodes) {
		if (!node->adapter) { continue; }
		for (auto const& tap : node->adapter->taps) {
			if (tap.send.get () != send) { continue; }
			BufferSet const* source = &node->adapter->audible;
			auto current = RouteAdapter::current;
			if (current && current->rendering_future) {
				source = nullptr;
				for (auto const& feed : current->feeds) { if (feed.source == node.get ()) { source = feed.input.get (); break; } }
				if (!source) { return true; }
			}
			for (DataType::iterator type = DataType::begin (); type != DataType::end (); ++type) {
				for (uint32_t c = 0; c < std::min (tap.count.get (*type), destination.count ().get (*type)); ++c) {
					auto const& buffer = source->get_available (*type, c + tap.offset.get (*type));
					if (*type == DataType::AUDIO) { destination.get_audio (c).accumulate_from (static_cast<AudioBuffer const&> (buffer), nframes); }
					else { destination.get_midi (c).merge_from (buffer, nframes); }
				}
			}
			return true;
		}
	}
	return false;
}

AnticipativeProcess::AnticipativeProcess (Session& session) : _session (session) {}
AnticipativeProcess::~AnticipativeProcess () { clear (); }

void
AnticipativeProcess::prepare (samplepos_t position, uint32_t milliseconds, uint64_t generation)
{
	clear ();
	if (milliseconds < 1 || milliseconds > 2000) { throw std::invalid_argument ("Anticipation horizon must be 1..2000 ms"); }
	auto timeline = std::make_shared<Timeline> (_session, position);
	uint32_t block = _session.engine ().samples_per_cycle ();
	uint32_t blocks = std::max (uint32_t (2), uint32_t ((_session.sample_rate () * milliseconds / 1000 + block - 1) / block));
	try {
		for (auto const& route : *_session.get_routes ()) {
			if (!route->active () || route->anticipative_processing_disabled ()) { continue; }
			add_route (route, timeline->first (*route), blocks, [timeline] (Queue::Range const& range) { return timeline->next (range); });
		}
		start (generation);
	} catch (...) { clear (); throw; }
}

bool
AnticipativeProcess::primed () const
{
	if (!running ()) { return false; }
	for (auto const& node : _nodes) {
		if (node->reason.load () == Predictable && !node->queue->primed ()) { return false; }
	}
	return true;
}

bool
AnticipativeProcess::faulted () const
{
	for (auto const& node : _nodes) { if (node->queue->state () == Queue::State::Fault) { return true; } }
	return false;
}

AnticipativeProcess::Node const*
AnticipativeProcess::find_node (Route const* route) const
{
	for (auto const& node : _nodes) { if (node->route.get () == route) { return node.get (); } }
	return nullptr;
}

AnticipativeProcess::Node&
AnticipativeProcess::add_route (std::shared_ptr<Route> route, Queue::Range const& first, uint32_t blocks, Queue::NextRange next)
{
	if (running () || !route) { throw std::logic_error ("Route binding requires a stopped plan"); }
	for (auto const& node : _nodes) { if (node->route == route) { throw std::invalid_argument ("Duplicate anticipation owner"); } }
	auto adapter = std::make_shared<RouteAdapter> (*this, route, first);
	Node& node = add (route, {adapter->channels, adapter->max_frames, blocks, 8192},
	                 [adapter] (BufferSet& b, Queue::Range const& r, bool live) { adapter->render (b, r, live); }, first,
	                 std::move (next), [adapter] (Queue::Range const& r) { return adapter->ready (r); });
	node.adapter = adapter; adapter->node = &node; node.requires_live = adapter->requires_live;
	for (auto& tap : adapter->taps) { if (tap.send) { tap.send->set_manual_cycle_start (true); } }
	for (auto& side : adapter->side_inputs) { side->sidechain->set_cached_input_reader (&RouteAdapter::side_input, side.get ()); }
	route->main_outs ()->set_render_buffers (&adapter->main);
	route->set_render_consumer (&RouteAdapter::consume, adapter.get ());
	route->set_render_observer (&RouteAdapter::observe, adapter.get ()); adapter->attached = true;
	return node;
}

AnticipativeProcess::Node&
AnticipativeProcess::add (std::shared_ptr<Route> route, Queue::Configuration const& config,
	Queue::Render render, Queue::Range const& first, Queue::NextRange next, Queue::Ready ready)
{
	if (running ()) { throw std::logic_error ("Cannot change a running anticipation plan"); }
	auto routes = _session.get_routes ();
	if (!route || std::find (routes->begin (), routes->end (), route) == routes->end ()) {
		throw std::invalid_argument ("Anticipation owner must be a registered session route");
	}
	for (auto const& node : _nodes) {
		if (node->route == route) { throw std::invalid_argument ("Duplicate anticipation owner"); }
	}
	std::unique_ptr<Node> node (new Node);
	node->route = std::move (route);
	node->first = first;
	node->queue.reset (new Queue (config, std::move (render), std::move (next), std::move (ready)));
	_nodes.push_back (std::move (node));
	_bound.store (true, std::memory_order_release);
	return *_nodes.back ();
}

void
AnticipativeProcess::start (uint64_t generation)
{
	stop ();
	_generation = generation;
	_work.resize (_nodes.size ());
	for (auto& node : _nodes) {
		if (node->adapter) { node->adapter->wire (); node->adapter->continuous = node->first.continuous; node->adapter->last_project_end = node->first.project; }
	}
	auto routes = _session.get_routes ();
	if (_return_routes.empty ()) {
		for (auto const& route : *routes) {
			if (route->internal_return ()) { route->internal_return ()->set_cached_input_reader (&AnticipativeProcess::cached_send, this); _return_routes.push_back (route); }
		}
	}
	for (auto& from : _nodes) {
		from->downstream.clear ();
		from->unmanaged_upstream = false;
		for (auto const& route : *routes) {
			bool managed = false;
			for (auto const& node : _nodes) { if (node->route == route) { managed = true; break; } }
			if (!managed && route->feeds (from->route)) { from->unmanaged_upstream = true; }
		}
		for (size_t i = 0; i < _nodes.size (); ++i) {
			if (from.get () != _nodes[i].get () && from->route->direct_feeds_according_to_graph (_nodes[i]->route)) {
				from->downstream.push_back (i);
			}
		}
	}
	begin_cycle ();
	try {
		for (auto& node : _nodes) {
			node->queue->start (node->first, generation, node->reason.load () != Predictable);
		}
	} catch (...) {
		stop ();
		throw;
	}
	_running.store (true, std::memory_order_release);
}

void
AnticipativeProcess::suspend ()
{
	_running.store (false, std::memory_order_release);
	for (auto& node : _nodes) { node->queue->request_stop (); }
}

bool
AnticipativeProcess::quiescent () const
{
	for (auto const& node : _nodes) { if (!node->queue->quiescent ()) { return false; } }
	return true;
}

bool
AnticipativeProcess::owns (Route const* route) const
{
	for (auto const& node : _nodes) { if (node->adapter && node->route.get () == route) { return true; } }
	return false;
}

void
AnticipativeProcess::stop ()
{
	suspend ();
	/* Readiness predicates never wait inside DSP; stopping any source cannot
	 * deadlock a downstream queue waiting for an unpublished range. */
	for (auto& node : _nodes) { node->queue->stop (); }
}

void
AnticipativeProcess::clear ()
{
	stop ();
	for (auto const& route : _return_routes) { route->internal_return ()->set_cached_input_reader (nullptr, nullptr); }
	_return_routes.clear ();
	for (auto const& node : _nodes) { node->route->set_anticipative_status (Unbound, node->queue->underruns ()); }
	_nodes.clear ();
	_work.clear ();
	_bound.store (false, std::memory_order_release);
}

void
AnticipativeProcess::begin_cycle ()
{
	size_t head = 0, tail = 0;
	for (size_t i = 0; i < _nodes.size (); ++i) {
		Node& node = *_nodes[i];
		uint32_t reason = (node.forced.load (std::memory_order_relaxed) || node.route->anticipative_processing_disabled ()) ? Forced : Predictable;
		if (node.requires_live) { reason |= RequiredLive; }
		if (node.unmanaged_upstream) { reason |= UnmanagedUpstream; }
		if (node.route->is_master () || node.route->is_monitor () || node.route->is_auditioner ()) {
			reason |= OutputRoute;
		}
		auto track = std::dynamic_pointer_cast<Track> (node.route);
		auto midi = std::dynamic_pointer_cast<MidiTrack> (node.route);
		if (midi && (midi->immediate_events ().read_space () || midi->user_immediate_events ().read_space ())) { reason |= ImmediateMidi; }
		if (track) {
			if (track->rec_enable_control ()->get_value () != 0) { reason |= Recording; }
			if (track->monitoring_state () & MonitoringInput) { reason |= Monitoring; }
		}
		node.reason.store (reason, std::memory_order_relaxed);
	}
	/* Eligibility can change before DSP ownership does. Keep dependencies live
	 * while a source is still live or offering its cursor to an unscheduled
	 * worker. The source itself remains eligible to claim the offer. Seed all
	 * reasons before building the work list so each node is enqueued once. */
	for (auto const& source : _nodes) {
		auto state = source->queue->state ();
		if (state != Queue::State::Live && state != Queue::State::Offered &&
		    state != Queue::State::Draining && state != Queue::State::Drained) { continue; }
		for (size_t target : source->downstream) {
			if (_nodes[target]->reason.load (std::memory_order_relaxed) == Predictable) {
				_nodes[target]->reason.store (UpstreamLive, std::memory_order_relaxed);
			}
		}
	}
	for (size_t i = 0; i < _nodes.size (); ++i) {
		if (_nodes[i]->reason.load (std::memory_order_relaxed)) { _work[tail++] = i; }
	}
	while (head < tail) {
		for (size_t target : _nodes[_work[head++]]->downstream) {
			Node& node = *_nodes[target];
			if (node.reason.load (std::memory_order_relaxed) == Predictable) {
				node.reason.store (UpstreamLive, std::memory_order_relaxed);
				_work[tail++] = target;
			}
		}
	}
	for (auto const& node : _nodes) {
		uint32_t reason = node->reason.load (std::memory_order_relaxed);
		if (node->queue->state () == Queue::State::Fault) { reason |= RenderFault; }
		node->route->set_anticipative_status (reason, node->queue->underruns ());
	}
}

AnticipativeProcess::Queue::Result
AnticipativeProcess::process (Node& node, BufferSet& output, Queue::Range const& range)
{
	if (!running ()) {
		output.silence (range.frames, 0);
		return Queue::Result::Stopped;
	}
	return node.queue->process (output, range, node.reason.load (std::memory_order_relaxed) != Predictable, _generation);
}
