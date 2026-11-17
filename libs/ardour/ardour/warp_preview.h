/* Copyright (C) 2026. GPL-2.0-or-later.
 * Single producer / single audio consumer preview handover.
 * Producer supplies immutable, planar track-output windows; audio callbacks
 * never allocate, wait, destroy buffers, or access a producer-owned slot.
 */
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace ARDOUR {
class WarpPreview {
 static_assert (std::atomic<int>::is_always_lock_free, "Preview slots require lock-free atomics");
 static_assert (std::atomic<uint64_t>::is_always_lock_free, "Preview generations require lock-free atomics");
 enum State { Free, Writing, Ready, Active };
 struct Slot {
  std::atomic<int> state {Free};
  std::vector<std::vector<float>> pcm;
  int64_t start = 0, loop_start = 0, loop_end = 0;
  uint64_t generation = 0;
 };
 Slot slots[3];
 std::atomic<uint64_t> requested {0};
 std::atomic<uint64_t> switches {0}, processed {0}, misses {0}, active_generation {0};
 mutable std::atomic<uint64_t> changed_samples {0};
 int current = -1, previous = -1;
 uint32_t fade = 256, fade_length = 256;
 uint32_t block_frames = 0;
 int64_t block_start = 0;
 bool enabled = false;

 int64_t offset (Slot const& slot, int64_t start) const {
  int64_t result = start-slot.start;
  if (result < 0 && slot.loop_end > slot.loop_start && start >= slot.loop_start && start < slot.loop_end && slot.start < slot.loop_end)
   result += slot.loop_end-slot.loop_start;
  return result;
 }
 bool covers (int index, int64_t start, uint32_t frames) const {
  if (index < 0) return false;
  auto const& s = slots[index];
  const int64_t at = offset (s, start);
  return !s.pcm.empty () && at >= 0 &&
   uint64_t(at) + frames <= s.pcm.front ().size ();
 }
 float sample (int index, unsigned channel, uint32_t frame, float fallback) const {
  if (index < 0 || channel >= slots[index].pcm.size ()) return fallback;
  return slots[index].pcm[channel][size_t(offset (slots[index], block_start)) + frame];
 }
 void release (int& index) {
  if (index >= 0) slots[index].state.store (Free, std::memory_order_release);
  index = -1;
 }
public:
 WarpPreview () = default;
 WarpPreview (WarpPreview const&) = delete;
 WarpPreview& operator= (WarpPreview const&) = delete;

 struct Statistics { uint64_t requested, active, switches, frames, changed_samples, missed_windows; };
 Statistics statistics () const {
  return {requested.load (), active_generation.load (), switches.load (), processed.load (), changed_samples.load (), misses.load ()};
 }

 /* Producer thread. A new request invalidates unfinished, older renders. */
 uint64_t request () { return requested.fetch_add (1, std::memory_order_acq_rel) + 1; }
 bool publish (uint64_t generation, int64_t start, std::vector<std::vector<float>> pcm, int64_t loop_start = 0, int64_t loop_end = 0) {
  if (generation != requested.load (std::memory_order_acquire) || pcm.empty () || pcm.front ().empty ()) return false;
  for (auto const& channel : pcm) if (channel.size () != pcm.front ().size ()) return false;
  for (auto& slot : slots) {
   int expected = Free;
   if (!slot.state.compare_exchange_strong (expected, Writing, std::memory_order_acq_rel)) {
    expected = Ready; // supersede an unconsumed window; never touch Active
    if (!slot.state.compare_exchange_strong (expected, Writing, std::memory_order_acq_rel)) continue;
   }
   slot.pcm = std::move (pcm); slot.start = start; slot.generation = generation;slot.loop_start=loop_start;slot.loop_end=loop_end;
   slot.state.store (Ready, std::memory_order_release);
   return true;
  }
  return false;
 }
 /* Publish an empty handover marker to fade back to the disk reader. */
 bool cancel () {
  const auto generation = request ();
  for (auto& slot : slots) {
   int expected = Free;
   if (!slot.state.compare_exchange_strong (expected, Writing, std::memory_order_acq_rel)) {
    expected = Ready;
    if (!slot.state.compare_exchange_strong (expected, Writing, std::memory_order_acq_rel)) continue;
   }
   slot.pcm.clear (); slot.generation = generation;
   slot.state.store (Ready, std::memory_order_release);return true;
  }
  return false;
 }

 /* Audio thread: begin once, apply to every channel, then end once. */
 void begin (int64_t start, uint32_t frames, bool forward, uint32_t crossfade_frames = 256) {
  block_start = start; block_frames = frames; enabled = forward;
  if (!forward) return;
  fade_length = std::max (1u, crossfade_frames);
  if (fade >= fade_length) {
   release (previous);
   for (int i=0;i<3;++i) {
    int expected = Ready;
    if (!slots[i].state.compare_exchange_strong (expected, Active, std::memory_order_acq_rel)) continue;
    if (slots[i].generation != requested.load (std::memory_order_acquire)) { slots[i].state.store (Free, std::memory_order_release);continue; }
    if (!slots[i].pcm.empty () && !covers (i, start, frames)) { slots[i].state.store (Ready, std::memory_order_release);continue; }
    previous = current; current = i; fade = 0; switches.fetch_add (1, std::memory_order_relaxed);active_generation.store (slots[i].pcm.empty () ? 0 : slots[i].generation, std::memory_order_relaxed);break;
   }
  }
  // A window must cover the whole callback; never read unrendered memory.
  if (current >= 0 && !slots[current].pcm.empty () && !covers (current, start, frames)) { release (current);active_generation.store (0, std::memory_order_relaxed);misses.fetch_add (1, std::memory_order_relaxed); }
  if (previous >= 0 && !covers (previous, start, frames)) release (previous);
 }
 void apply (float* data, unsigned channel) const {
  if (!enabled || (current < 0 && previous < 0)) return;
  uint64_t changed = 0;
  for (uint32_t i=0;i<block_frames;++i) {
   const float disk = data[i];
   const float before = sample (previous, channel, i, disk);
   const float after = sample (current, channel, i, disk);
   const float alpha = std::min (1.f, float(uint64_t(fade)+i+1)/fade_length);
   data[i] = before + alpha*(after-before);
   if (data[i] != disk) ++changed;
  }
  if (channel == 0) changed_samples.fetch_add (changed, std::memory_order_relaxed);
 }
 void end () {
  if (!enabled) return;
  if (current >= 0 && !slots[current].pcm.empty ()) processed.fetch_add (block_frames, std::memory_order_relaxed);
  fade = uint32_t(std::min<uint64_t> (fade_length, uint64_t(fade)+block_frames));
  if (fade >= fade_length) {
   release (previous);
   if (current >= 0 && slots[current].pcm.empty ()) release (current);
  }
 }
};
}
