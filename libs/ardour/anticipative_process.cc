/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/anticipative_process.h"

#include <algorithm>
#include <stdexcept>

#include "ardour/record_enable_control.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/track.h"

using namespace ARDOUR;

AnticipativeProcess::AnticipativeProcess (Session& session) : _session (session) {}
AnticipativeProcess::~AnticipativeProcess () { clear (); }

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
	for (auto& from : _nodes) {
		from->downstream.clear ();
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
