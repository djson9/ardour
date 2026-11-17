/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "ardour/anticipation_queue.h"

namespace ARDOUR {

class Route;
class Session;
class InternalSend;

/** Session-owned anticipation plan and exclusive DSP queues.
 *
 * Plan construction/retirement require a quiescent audio consumer. begin_cycle()
 * uses a prepared copy of existing route edges, without allocating or rebuilding
 * the graph. Route adapters retain ownership of their I/O and processor snapshots
 * until clear() returns. The audio callback never calls start(), stop() or clear().
 */
class LIBARDOUR_API AnticipativeProcess {
public:
	struct RouteAdapter;
	using Queue = AnticipationQueue;
	/** Immutable snapshot of the callback's block, preroll and loop boundaries.
	 * Construct only while the session's transport/routing plan is quiescent. */
	class LIBARDOUR_API Timeline {
	public:
		Timeline (Session&, samplepos_t start, bool preroll = true);
		Queue::Range first (Route const&) const;
		Queue::Range next (Queue::Range const&) const;
	private:
		Queue::Range at (samplecnt_t elapsed) const;
		pframes_t _block;
		samplepos_t _start, _loop_start {0}, _loop_end {0};
		samplecnt_t _preroll;
		std::vector<samplecnt_t> _boundaries;
	};
	enum Reason : uint32_t {
		Predictable = 0, Forced = 1, Recording = 2, Monitoring = 4,
		UpstreamLive = 8, OutputRoute = 16, UnmanagedUpstream = 32, RequiredLive = 64
	};

	struct Node {
		std::shared_ptr<Route> route;
		std::unique_ptr<Queue> queue;
		Queue::Range first;
		std::atomic<bool> forced {false};
		std::atomic<uint32_t> reason {Predictable};
		std::vector<size_t> downstream;
		bool unmanaged_upstream {false};
		bool requires_live {false};
		std::shared_ptr<RouteAdapter> adapter;
	};

	explicit AnticipativeProcess (Session&);
	~AnticipativeProcess ();
	AnticipativeProcess (AnticipativeProcess const&) = delete;
	AnticipativeProcess& operator= (AnticipativeProcess const&) = delete;

	/** Control thread, stopped plan. The route must belong to this Session. */
	Node& add (std::shared_ptr<Route>, Queue::Configuration const&, Queue::Render,
	           Queue::Range const&, Queue::NextRange = Queue::NextRange (), Queue::Ready = Queue::Ready ());
	/** Bind the original registered route DSP and main output to the plan. */
	Node& add_route (std::shared_ptr<Route>, Queue::Range const&, uint32_t blocks,
	                 Queue::NextRange = Queue::NextRange ());
	/** Control thread: snapshot existing direct edges, then start producers. */
	void start (uint64_t generation);
	/** Control thread, consumer quiescent: join all producers before mutation. */
	void stop ();
	void clear ();
	/** RT: cancel future work; acknowledgement precedes transport DSP mutation. */
	void suspend ();
	bool quiescent () const;
	bool owns (Route const*) const;
	/** Non-RT, consumer quiescent: prepare ordinary active session routes. */
	void prepare (samplepos_t start, uint32_t milliseconds, uint64_t generation);
	bool primed () const;
	bool faulted () const;
	/** Diagnostic lookup; hold the process lock while a plan can be rebuilt. */
	Node const* find_node (Route const*) const;

	/** RT, before the normal route graph: recompute the live dependency closure. */
	void begin_cycle ();
	/** RT, from the route's consumer boundary. */
	Queue::Result process (Node&, BufferSet&, Queue::Range const&);
	bool running () const { return _running.load (std::memory_order_acquire); }
	uint64_t generation () const { return _generation; }
	size_t size () const { return _nodes.size (); }

private:
	static bool cached_send (void*, InternalSend const*, BufferSet&, samplepos_t, pframes_t);
	Session& _session;
	std::vector<std::unique_ptr<Node>> _nodes;
	std::vector<size_t> _work;
	std::vector<std::shared_ptr<Route>> _return_routes;
	std::atomic<bool> _running {false};
	uint64_t _generation {0};
};

} // namespace ARDOUR
