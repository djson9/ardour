/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "ardour/buffer_set.h"
#include "ardour/libardour_visibility.h"
#include "ardour/types.h"

namespace ARDOUR {

class ThreadBuffers;

/** Exclusive DSP ownership with bounded, timestamped audio/MIDI read-ahead.
 *
 * Construction, start() and stop() belong to the control thread, with the
 * consumer quiescent. process() belongs to one realtime consumer. Render must
 * use exclusively owned DSP and buffers: it runs on the producer OR consumer,
 * never both. Its live argument selects current input versus predictable media.
 * The owner must retain routing/processor/tempo snapshots until stop() returns.
 * This class neither changes the Session clock nor waits on the audio thread.
 */
class LIBARDOUR_API AnticipationQueue {
public:

	struct Range {
		samplepos_t project;
		samplepos_t continuous;
		pframes_t frames;
	};

	struct Configuration {
		ChanCount channels;
		pframes_t max_frames;
		uint32_t blocks;
		size_t midi_bytes;
	};

	enum class State { Stopped, Ahead, Draining, Drained, Live, Fault, Offered };
	enum class Result { Cached, Live, Underrun, Discontinuity, Stopped };

	using Render = std::function<void (BufferSet&, Range const&, bool live)>;
	/** Called only by the DSP owner, including the live consumer. No allocations
	 * or locks are permitted. Allows event/loop-boundary subdivisions. */
	using NextRange = std::function<Range (Range const&)>;
	/** Producer-only prerequisite check, before touching DSP. False retries later
	 * and permits cancellation while waiting for a routed upstream range. */
	using Ready = std::function<bool (Range const&)>;

	AnticipationQueue (Configuration const&, Render, NextRange = NextRange (), Ready = Ready ());
	~AnticipationQueue ();
	AnticipationQueue (AnticipationQueue const&) = delete;
	AnticipationQueue& operator= (AnticipationQueue const&) = delete;

	/** Non-RT; caller supplies DSP already located/reset to this range. */
	void start (Range const&, uint64_t generation, bool initially_live = false);
	/** Non-RT; joins the producer before allowing DSP mutation/destruction. */
	void stop ();
	/** RT cancellation. The owner must stop consuming before requesting this.
	 * quiescent() acknowledges the final job without joining on the callback. */
	void request_stop () { _quit.store (true, std::memory_order_release); }
	bool quiescent () const { return _exited.load (std::memory_order_acquire); }
	/** RT; a false live request offers DSP to the producer after this live call.
	 * Until claimed, the next consumer may reclaim it and continue live. Once
	 * claimed, missing output remains a diagnosed underrun; never wait or race DSP. */
	Result process (BufferSet&, Range const&, bool want_live, uint64_t generation);

	State state () const { return _state.load (std::memory_order_acquire); }
	uint64_t queued_blocks () const;
	bool primed () const { return queued_blocks () == _config.blocks; }
	uint64_t underruns () const { return _underruns.load (); }
	uint64_t discontinuities () const { return _discontinuities.load (); }
	uint64_t live_handoffs () const { return _live_handoffs.load (); }
	uint64_t generation () const { return _generation; }
	bool stopping () const { return _quit.load (std::memory_order_acquire); }

	/** Non-RT, before start: retain output until each future consumer releases it.
	 * The token and its queue must outlive all peek() calls. Each token has one
	 * consumer; release_until() is called only after that consumer finishes reading.
	 */
	struct Reader {
		std::atomic<samplepos_t> released_until {0};
		std::atomic<uint64_t> index {0};
	};
	Reader& add_reader ();
	/** Worker-only, non-consuming range lookup. Never waits; false means not yet
	 * rendered or an invalid generation/range. A registered Reader pins the data.
	 */
	bool peek (Reader const&, BufferSet&, Range const&, uint64_t generation);
	void release_until (Reader&, samplepos_t continuous);

private:

	struct Slot {
		BufferSet buffers;
		Range range;
		uint64_t generation;
	};

	void allocate (BufferSet&);
	void worker ();
	void render (BufferSet&, Range const&, bool);
	Range next (Range const&) const;
	Result fault (BufferSet&, pframes_t);
	void copy (BufferSet&, Slot const&, pframes_t, pframes_t, pframes_t);

	Configuration const _config;
	Render const _render;
	NextRange const _next_range;
	Ready const _ready;
	std::vector<std::unique_ptr<Slot>> _slots;
	std::vector<std::unique_ptr<Reader>> _readers;
	BufferSet _scratch;
	BufferSet _live_input;
	std::unique_ptr<ThreadBuffers> _worker_buffers;
	std::thread _worker;
	std::atomic<State> _state;
	std::atomic<bool> _quit;
	std::atomic<bool> _exited {true};
	std::atomic<uint64_t> _read;
	std::atomic<uint64_t> _write;
	std::atomic<uint64_t> _underruns;
	std::atomic<uint64_t> _discontinuities;
	std::atomic<uint64_t> _live_handoffs;
	Range _next; // exclusive DSP owner; published with state
	samplepos_t _consumer_next;
	uint64_t _generation; // immutable between start/stop
	pframes_t _offset; // consumer only
};

} // namespace ARDOUR
