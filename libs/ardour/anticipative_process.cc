/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/anticipative_process.h"

#include <algorithm>
#include <stdexcept>

#include "ardour/record_enable_control.h"
#include "ardour/audio_buffer.h"
#include "ardour/midi_buffer.h"
#include "ardour/delivery.h"
#include "ardour/internal_send.h"
#include "ardour/plugin_insert.h"
#include "ardour/port_insert.h"
#include "ardour/send.h"
#include "ardour/port.h"
#include "ardour/port_set.h"
#include "ardour/audioengine.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/track.h"

using namespace ARDOUR;

struct AnticipativeProcess::RouteAdapter {
	struct Feed {
		Node* source;
		Queue::Reader* reader;
		std::unique_ptr<BufferSet> input;
	};
	struct Connection { size_t feed; DataType type; uint32_t source, destination; };
	AnticipativeProcess& plan;
	std::shared_ptr<Route> route;
	Node* node {nullptr};
	BufferSet work, main, audible, future_input;
	ChanCount channels;
	std::vector<Feed> feeds;
	std::vector<Connection> connections;
	bool wired {false};
	bool track;
	samplepos_t continuous;

	RouteAdapter (AnticipativeProcess& p, std::shared_ptr<Route> r, Queue::Range const& first)
		: plan (p), route (r), channels (ChanCount::max (r->max_processor_streams (), ChanCount::max (r->n_inputs (), r->n_outputs ()))),
		  track (bool (std::dynamic_pointer_cast<Track> (r))), continuous (first.continuous)
	{
		for (BufferSet* buffer : {&work, &main, &audible, &future_input}) { allocate (*buffer, channels, first.frames); }
		main.set_count (r->n_outputs ());
	}
	~RouteAdapter () { route->set_render_consumer (nullptr, nullptr); route->main_outs ()->set_render_buffers (nullptr); }
	static void allocate (BufferSet& b, ChanCount c, pframes_t n) {
		b.ensure_buffers (DataType::AUDIO, c.n_audio (), n); b.ensure_buffers (DataType::MIDI, c.n_midi (), 8192); b.set_count (c);
	}
	void wire () {
		if (wired) { return; }
		wired = true;
		/* DiskReader replaces the playback input; physical capture remains live. */
		if (track) { return; }
		for (DataType::iterator type = DataType::begin (); type != DataType::end (); ++type) {
			for (uint32_t destination = 0; destination < route->n_inputs ().get (*type); ++destination) {
				auto port = route->input ()->ports ()->port (*type, destination);
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
								allocate (*f.input, candidate->adapter->channels, node->first.frames); feeds.push_back (std::move (f));
							}
							connections.push_back ({feed, *type, source, destination});
						}
					}
					if (!found) { node->forced.store (true); }
				}
			}
		}
	}
	bool ready (Queue::Range const& range) {
		for (auto& feed : feeds) {
			if (!feed.source->queue->peek (*feed.reader, *feed.input, range, plan.generation ())) { return false; }
		}
		future_input.set_count (channels); future_input.silence (range.frames, 0);
		for (uint32_t c = 0; c < channels.n_midi (); ++c) { future_input.get_midi (c).clear (); }
		for (auto const& connection : connections) {
			auto& source = feeds[connection.feed].input->get_available (connection.type, connection.source);
			if (connection.type == DataType::AUDIO) { future_input.get_audio (connection.destination).accumulate_from (static_cast<AudioBuffer&> (source), range.frames); }
			else { future_input.get_midi (connection.destination).merge_from (source, range.frames); }
		}
		return true;
	}
	void render (BufferSet& b, Queue::Range const& range, bool live) {
		work.set_count (channels); work.silence (range.frames, 0);
		for (uint32_t c = 0; c < channels.n_midi (); ++c) { work.get_midi (c).clear (); }
		work.read_from (live ? b : future_input, range.frames);
		route->render_anticipative (work, range.project, range.project + range.frames, range.frames, live);
		for (uint32_t c = 0; c < main.count ().n_midi (); ++c) { b.get_midi (c).clear (); }
		b.read_from (main, range.frames);
		for (auto& feed : feeds) { feed.source->queue->release_until (*feed.reader, range.continuous + range.frames); }
	}
	static bool consume (void* context, BufferSet& input, samplepos_t start, samplepos_t end, pframes_t nframes, bool moving) {
		auto& self = *static_cast<RouteAdapter*> (context);
		if (!moving || !self.plan.running ()) { self.route->output ()->silence (nframes); return true; }
		self.route->capture_anticipative_input (input, start, end, nframes);
		for (uint32_t c = 0; c < self.channels.n_midi (); ++c) { self.audible.get_midi (c).clear (); }
		self.audible.read_from (input, nframes);
		self.plan.process (*self.node, self.audible, {start, self.continuous, nframes});
		self.continuous += nframes;
		self.audible.set_count (self.route->n_outputs ());
		self.route->deliver_anticipative (self.audible, nframes);
		return true;
	}
};

AnticipativeProcess::AnticipativeProcess (Session& session) : _session (session) {}
AnticipativeProcess::~AnticipativeProcess () { clear (); }

AnticipativeProcess::Node&
AnticipativeProcess::add_route (std::shared_ptr<Route> route, Queue::Range const& first, uint32_t blocks)
{
	/* Physical inserts and routing taps need their respective adapter bindings;
	 * never let an unbound routing processor run against device buffers ahead. */
	bool unsupported = false;
	route->foreach_processor ([&] (std::weak_ptr<Processor> weak) {
		auto processor = weak.lock ();
		auto plugin = std::dynamic_pointer_cast<PluginInsert> (processor);
		if (std::dynamic_pointer_cast<Send> (processor) || std::dynamic_pointer_cast<PortInsert> (processor) || (plugin && plugin->has_sidechain ())) { unsupported = true; }
	});
	if (unsupported) { throw std::invalid_argument ("Route requires send/sidechain adapter bindings"); }
	auto adapter = std::make_shared<RouteAdapter> (*this, route, first);
	Node& node = add (route, {adapter->channels, first.frames, blocks, 8192},
	                 [adapter] (BufferSet& b, Queue::Range const& r, bool live) { adapter->render (b, r, live); }, first,
	                 Queue::NextRange (), [adapter] (Queue::Range const& r) { return adapter->ready (r); });
	node.adapter = adapter; adapter->node = &node;
	route->main_outs ()->set_render_buffers (&adapter->main);
	route->set_render_consumer (&RouteAdapter::consume, adapter.get ());
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
	return *_nodes.back ();
}

void
AnticipativeProcess::start (uint64_t generation)
{
	stop ();
	_generation = generation;
	_work.resize (_nodes.size ());
	for (auto& node : _nodes) {
		if (node->adapter) { node->adapter->wire (); node->adapter->continuous = node->first.continuous; }
	}
	auto routes = _session.get_routes ();
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
AnticipativeProcess::stop ()
{
	_running.store (false, std::memory_order_release);
	/* Readiness predicates never wait inside DSP; stopping any source cannot
	 * deadlock a downstream queue waiting for an unpublished range. */
	for (auto& node : _nodes) { node->queue->stop (); }
}

void
AnticipativeProcess::clear ()
{
	stop ();
	_nodes.clear ();
	_work.clear ();
}

void
AnticipativeProcess::begin_cycle ()
{
	size_t head = 0, tail = 0;
	for (size_t i = 0; i < _nodes.size (); ++i) {
		Node& node = *_nodes[i];
		uint32_t reason = node.forced.load (std::memory_order_relaxed) ? Forced : Predictable;
		if (node.unmanaged_upstream) { reason |= UnmanagedUpstream; }
		if (node.route->is_master () || node.route->is_monitor () || node.route->is_auditioner ()) {
			reason |= OutputRoute;
		}
		auto track = std::dynamic_pointer_cast<Track> (node.route);
		if (track) {
			if (track->rec_enable_control ()->get_value () != 0) { reason |= Recording; }
			if (track->monitoring_state () & MonitoringInput) { reason |= Monitoring; }
		}
		node.reason.store (reason, std::memory_order_relaxed);
		if (reason) { _work[tail++] = i; }
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
