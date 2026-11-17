/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/anticipation_queue.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "temporal/tempo.h"
#include "ardour/audio_buffer.h"
#include "ardour/midi_buffer.h"
#include "ardour/process_thread.h"

using namespace ARDOUR;

AnticipationQueue::AnticipationQueue (Configuration const& config, Render render, NextRange next_range)
	: _config (config)
	, _render (std::move (render))
	, _next_range (std::move (next_range))
	, _state (State::Stopped)
	, _quit (false)
	, _read (0)
	, _write (0)
	, _underruns (0)
	, _discontinuities (0)
	, _live_handoffs (0)
	, _next {0, 0, config.max_frames}
	, _consumer_next (0)
	, _generation (0)
	, _offset (0)
{
	if (!_render || !config.max_frames || config.blocks < 2 || config.blocks > 65536 ||
	    (config.channels.n_midi () && !config.midi_bytes)) {
		throw std::invalid_argument ("Invalid anticipation queue configuration");
	}
	allocate (_scratch);
	for (uint32_t i = 0; i < config.blocks; ++i) {
		std::unique_ptr<Slot> slot (new Slot);
		allocate (slot->buffers);
		_slots.push_back (std::move (slot));
	}
}

AnticipationQueue::~AnticipationQueue ()
{
	stop ();
}

void
AnticipationQueue::allocate (BufferSet& buffers)
{
	buffers.ensure_buffers (DataType::AUDIO, _config.channels.n_audio (), _config.max_frames);
	buffers.ensure_buffers (DataType::MIDI, _config.channels.n_midi (), _config.midi_bytes);
	buffers.set_count (_config.channels);
}

void
AnticipationQueue::start (Range const& first, uint64_t generation)
{
	stop ();
	if (!first.frames || first.frames > _config.max_frames) {
		throw std::invalid_argument ("Invalid initial anticipation range");
	}
	_next = first;
	_consumer_next = first.continuous;
	_generation = generation;
	_offset = 0;
	_read.store (0);
	_write.store (0);
	_underruns.store (0);
	_discontinuities.store (0);
	_live_handoffs.store (0);
	_quit.store (false);
	_state.store (State::Ahead, std::memory_order_release);
	_worker = std::thread (&AnticipationQueue::worker, this);
}

void
AnticipationQueue::stop ()
{
	_quit.store (true, std::memory_order_release);
	if (_worker.joinable ()) {
		_worker.join ();
	}
	_state.store (State::Stopped, std::memory_order_release);
}

uint64_t
AnticipationQueue::queued_blocks () const
{
	/* Independently sampled counters are diagnostic only. */
	uint64_t r = _read.load (std::memory_order_acquire);
	uint64_t w = _write.load (std::memory_order_acquire);
	return w >= r ? std::min<uint64_t> (w - r, _config.blocks) : 0;
}

AnticipationQueue::Range
AnticipationQueue::next (Range const& range) const
{
	if (_next_range) {
		return _next_range (range);
	}
	return { range.project + range.frames, range.continuous + range.frames, _config.max_frames };
}

void
AnticipationQueue::render (BufferSet& buffers, Range const& range, bool live)
{
	ProcessThread::RenderScope time (range.project, range.continuous);
	_render (buffers, range, live);
}

void
AnticipationQueue::worker ()
{
	Temporal::TempoMap::fetch ();
	ProcessThread process_thread;
	process_thread.get_buffers ();
	while (!_quit.load (std::memory_order_acquire)) {
		State state = _state.load (std::memory_order_acquire);
		if (state == State::Draining) {
			_state.compare_exchange_strong (state, State::Drained, std::memory_order_release);
		}
		uint64_t w = _write.load (std::memory_order_relaxed);
		uint64_t r = _read.load (std::memory_order_acquire);
		if (state != State::Ahead || w - r >= _config.blocks) {
			std::this_thread::sleep_for (std::chrono::microseconds (50));
			continue;
		}
		try {
			if (!_next.frames || _next.frames > _config.max_frames) {
				throw std::runtime_error ("Invalid anticipation subdivision");
			}
			_scratch.set_count (_config.channels);
			_scratch.silence (_next.frames, 0);
			render (_scratch, _next, false);
			if (!(_scratch.count () <= _config.channels)) {
				throw std::runtime_error ("Anticipation channel layout changed");
			}
			Slot& slot = *_slots[w % _config.blocks];
			slot.buffers.set_count (_scratch.count ());
			slot.buffers.read_from (_scratch, _next.frames);
			slot.range = _next;
			slot.generation = _generation;
			_next = next (_next);
			_write.store (w + 1, std::memory_order_release);
		} catch (...) {
			_state.store (State::Fault, std::memory_order_release);
		}
	}
	process_thread.drop_buffers ();
}

AnticipationQueue::Result
AnticipationQueue::fault (BufferSet& output, pframes_t nframes)
{
	_state.store (State::Fault, std::memory_order_release);
	_discontinuities.fetch_add (1, std::memory_order_relaxed);
	output.silence (std::min (nframes, _config.max_frames), 0);
	return Result::Discontinuity;
}

void
AnticipationQueue::copy (BufferSet& output, Slot const& slot, pframes_t nframes,
		pframes_t destination_offset, pframes_t source_offset)
{
	for (DataType::iterator t = DataType::begin (); t != DataType::end (); ++t) {
		for (uint32_t c = 0; c < slot.buffers.count ().get (*t); ++c) {
			output.get_available (*t, c).read_from (slot.buffers.get_available (*t, c),
			                                      nframes, destination_offset, source_offset);
		}
	}
}

AnticipationQueue::Result
AnticipationQueue::process (BufferSet& output, Range const& requested, bool want_live, uint64_t generation)
{
	State state = _state.load (std::memory_order_acquire);
	if (state == State::Stopped) {
		output.silence (requested.frames, 0);
		return Result::Stopped;
	}
	if (state == State::Fault || generation != _generation || !requested.frames ||
	    requested.frames > _config.max_frames || requested.continuous != _consumer_next ||
	    !(_config.channels <= output.available ())) {
		return fault (output, requested.frames);
	}
	if (state == State::Ahead && want_live) {
		State expected = State::Ahead;
		_state.compare_exchange_strong (expected, State::Draining, std::memory_order_acq_rel);
		state = _state.load (std::memory_order_acquire);
	}
	if (state == State::Drained &&
	    _read.load () == _write.load (std::memory_order_acquire)) {
		if (_next.continuous != requested.continuous || _next.project != requested.project) {
			return fault (output, requested.frames);
		}
		_state.store (State::Live, std::memory_order_release);
		_live_handoffs.fetch_add (1, std::memory_order_relaxed);
		state = State::Live;
	}
	if (state == State::Live) {
		render (output, requested, true);
		_consumer_next += requested.frames;
		_next = next (requested);
		if (!want_live) {
			_state.store (State::Ahead, std::memory_order_release);
		}
		return Result::Live;
	}

	output.set_count (_config.channels);
	output.silence (requested.frames, 0);
	pframes_t copied = 0;
	while (copied < requested.frames) {
		uint64_t r = _read.load (std::memory_order_relaxed);
		if (r == _write.load (std::memory_order_acquire)) {
			/* Never execute lagging DSP on the consumer as a fallback. It must
			 * catch up on its owner thread, or be reset off the audio thread. */
			_underruns.fetch_add (1, std::memory_order_relaxed);
			_consumer_next += requested.frames;
			return Result::Underrun;
		}
		Slot const& slot = *_slots[r % _config.blocks];
		samplepos_t at = requested.continuous + copied;
		if (slot.range.continuous + slot.range.frames <= at) {
			/* Drop a late completed block after a diagnosed producer underrun.
			 * At most config.blocks slots can be discarded in this call. */
			_offset = 0;
			_read.store (r + 1, std::memory_order_release);
			continue;
		}
		if (slot.range.continuous > at || slot.generation != generation) {
			return fault (output, requested.frames);
		}
		_offset = at - slot.range.continuous;
		if (slot.range.project + _offset != requested.project + copied) {
			return fault (output, requested.frames);
		}
		pframes_t n = std::min (requested.frames - copied, slot.range.frames - _offset);
		copy (output, slot, n, copied, _offset);
		copied += n;
		_offset += n;
		if (_offset == slot.range.frames) {
			_offset = 0;
			_read.store (r + 1, std::memory_order_release);
		}
	}
	_consumer_next += requested.frames;
	return Result::Cached;
}
