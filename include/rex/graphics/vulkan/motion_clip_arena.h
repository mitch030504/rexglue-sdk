#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rex::graphics::vulkan {

// CPU-side allocator for the motion-guide clip arena.
//
// The GPU arena is split into two equal parity halves. A block owns one offset
// range; the current frame writes that range in one half and reads the previous
// frame from the same range in the other half. Ownership is therefore by
// offset, not by key: two blocks at the same offset would alias each other's
// history even though the CPU map still holds two distinct keys. Every block
// committed on frame - 1 reserves its range for the whole of the next frame,
// including one that has not been drawn yet - it may be drawn at any later draw
// index and its previous half is still readable.
//
// Placement is a property of the block: a reused block that keeps its capacity
// and index window keeps its offset no matter which blocks were culled or added
// around it. New or grown blocks are first-fit into a hole left by a block that
// did not survive the rotation, and otherwise go above the live layout end.
// Freeing a range never shrinks `layout_end_` within a frame; the frontier is
// the frame's high-water mark and is recomputed at the next rotation.
class MotionClipArena {
 public:
  // One clip is a float4 (xy = guest clip xy, z = clip w, w = written), so a
  // slot is 16 bytes and each half of the 64 MiB arena is this many slots.
  static constexpr uint32_t kSlotBytes = sizeof(float) * 4;
  static constexpr uint32_t kDefaultHalfSlots = uint32_t((uint64_t(64) << 20) / 2 / kSlotBytes);
  // A single draw may not reserve the whole half: max - min + 1 can be enormous
  // for a sparse index set, and one such draw would evict every other block.
  static constexpr uint32_t kDefaultMaxDrawSlots = UINT32_C(1) << 20;

  struct Block {
    // Offset of the block inside either half of the arena, in float4 slots. The
    // same layout is used in both halves, so the previous frame's block is at
    // the same offset in the other half.
    uint32_t slot_offset = 0;
    // Size of the block in slots, and the gl_VertexIndex window it covers. Never
    // shrinks: a stable draw keeps its offset and its history.
    uint32_t capacity = 0;
    uint32_t index_min = 0;
    uint32_t index_max = 0;
    // Guest frame that last recorded clip data into this block, or UINT64_MAX if
    // it has never been issued. Only a block written on frame - 1 may be read.
    uint64_t last_frame = UINT64_MAX;
    // Order-sensitive identity of the index set drawn last (stream + the fields
    // that select it). A different identity means the slots mean different
    // vertices, so the whole draw loses history.
    uint64_t index_fingerprint = 0;
    // One identity key can be drawn as several primitives (sub-draws). The block
    // is placed once per frame and every later sub-draw reuses that placement.
    uint64_t allocated_frame = UINT64_MAX;
    uint32_t frame_prev_clip_base = UINT32_MAX;
  };

  // Per-draw result handed to the system constants. 0xFFFFFFFF in either base
  // disables that side in the shader.
  struct Draw {
    uint64_t key = 0;
    uint32_t clip_base = UINT32_MAX;
    uint32_t prev_clip_base = UINT32_MAX;
    uint32_t index_min = 0;
    uint32_t index_max = 0;
    bool allocated = false;
  };

  // Counters for the frame that just ended, reported by BeginFrame. The layout
  // fields describe the new frame after the rotation prune and rebuild, except
  // `layout_end` (the completed frame's high-water mark) and `drawn_end`.
  struct FrameReport {
    uint64_t frame = UINT64_MAX;
    uint32_t blocks = 0;  // survivors after the prune (readable next frame)
    uint32_t freed = 0;   // blocks erased at this rotation
    uint32_t reallocated = 0;
    uint32_t index_set_changed = 0;
    uint32_t clamped = 0;
    uint32_t history = 0;  // draw sub-draws with a readable previous half
    uint32_t new_blocks = 0;
    uint32_t moved_blocks = 0;  // reused blocks whose placement was recomputed
    uint32_t repacked = 0;      // reused blocks whose offset actually changed
    uint32_t grew = 0;          // in-place growths into adjacent free space
    uint32_t reused = 0;        // placements served from a hole
    uint32_t bumped = 0;        // placements taken at the frontier
    uint32_t no_space = 0;      // draws skipped because no range fit
    uint32_t ff_steps = 0;      // first-fit hole nodes visited (pathology check)
    uint32_t layout_end = 0;
    uint32_t live_end = 0;
    uint32_t drawn_end = 0;
    uint32_t holes = 0;
    uint32_t free_slots = 0;
    uint32_t tail_free = 0;
    bool corrupt = false;
  };

  explicit MotionClipArena(uint32_t half_slots = kDefaultHalfSlots,
                           uint32_t max_draw_slots = kDefaultMaxDrawSlots)
      : slot_capacity_(half_slots), max_draw_slots_(max_draw_slots) {}

  // Rotates to `frame`: prunes blocks that cannot be read, rebuilds the live and
  // free ranges from every survivor, and fills `report` for the frame that just
  // ended. Returns false when `frame` is already current (idempotent).
  bool BeginFrame(uint64_t frame, FrameReport& report);

  // Places the draw's block, decides whether the previous half is readable, and
  // fills the constants. Returns false when no range fits; the caller must drop
  // this draw's motion for the frame and leave every other block alone.
  bool Allocate(uint64_t key, uint32_t draw_index_min, uint32_t draw_index_max,
                uint64_t index_fingerprint, Draw& draw_out);

  // Stamps the block as written for the current guest frame. Called only after
  // the draw command has actually been recorded.
  void Commit(uint64_t key);

  // Drops every block and range, keeping the current frame/parity so an
  // arena-full retry or a cache clear can rebuild from zero.
  void Clear();

  // Clear plus the frame stamp, for destruction.
  void Forget();

  uint64_t frame() const { return frame_; }
  uint32_t parity() const { return parity_; }
  uint32_t half_slots() const { return slot_capacity_; }
  size_t block_count() const { return blocks_.size(); }
  uint32_t layout_end() const { return layout_end_; }
  const FrameReport& stats() const { return stats_; }

  // Checks the layout invariants. Returns false and fills `why` on violation.
  bool Validate(std::string* why = nullptr) const;

 private:
  bool FindPlacement(uint32_t need, uint32_t& offset, bool& from_hole);
  void Claim(uint32_t offset, uint32_t need, bool from_hole);
  bool GrowInPlace(const Block& previous, uint32_t needed, uint32_t& offset);
  void Release(uint32_t offset, uint32_t capacity);
  void InsertFree(uint32_t begin, uint32_t end);
  void EraseBlock(uint64_t key);
  bool Fail(std::string* why, const char* message) const;

  uint32_t slot_capacity_;
  uint32_t max_draw_slots_;

  uint64_t frame_ = UINT64_MAX;
  uint32_t parity_ = 0;
  // High-water mark of the current frame's layout. Never lowered within a
  // frame; recomputed from the survivors at rotation.
  uint32_t layout_end_ = 0;

  std::unordered_map<uint64_t, Block> blocks_;
  // Sorted, disjoint [begin, end) ranges owned by blocks in this frame. Seeded
  // from every survivor, not from the draws seen so far.
  std::map<uint32_t, uint32_t> live_;
  // Sorted, coalesced [begin, end) holes below `layout_end_`.
  std::map<uint32_t, uint32_t> free_;

  FrameReport stats_;
};

inline bool MotionClipArena::BeginFrame(uint64_t frame, FrameReport& report) {
  if (frame == frame_) {
    return false;
  }
  report = FrameReport{};
  report.frame = frame_;
  if (frame_ != UINT64_MAX) {
    report.layout_end = layout_end_;
    report.reallocated = stats_.reallocated;
    report.index_set_changed = stats_.index_set_changed;
    report.clamped = stats_.clamped;
    report.history = stats_.history;
    report.new_blocks = stats_.new_blocks;
    report.moved_blocks = stats_.moved_blocks;
    report.repacked = stats_.repacked;
    report.grew = stats_.grew;
    report.reused = stats_.reused;
    report.bumped = stats_.bumped;
    report.no_space = stats_.no_space;
    report.ff_steps = stats_.ff_steps;
    for (const auto& entry : blocks_) {
      const Block& block = entry.second;
      if (block.last_frame == frame_) {
        report.drawn_end = std::max(report.drawn_end, block.slot_offset + block.capacity);
      }
    }
  }

  // Only frame - 1 can ever be a valid previous frame, so anything older (or
  // never committed) is dead weight and must not keep its range reserved.
  for (auto it = blocks_.begin(); it != blocks_.end();) {
    const Block& block = it->second;
    if (block.last_frame == UINT64_MAX || block.last_frame + 1 < frame) {
      ++report.freed;
      it = blocks_.erase(it);
    } else {
      ++it;
    }
  }
  report.blocks = uint32_t(blocks_.size());

  // Rebuild from the survivors. A block committed on frame - 1 is live even if
  // it has not been drawn yet in `frame`; it may be drawn later and its previous
  // half is still readable. Freeing it here would let another block overwrite
  // the offset this frame and alias the history at frame + 1.
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  ranges.reserve(blocks_.size());
  for (const auto& entry : blocks_) {
    const Block& block = entry.second;
    if (block.capacity == 0 || uint64_t(block.slot_offset) + block.capacity > slot_capacity_) {
      Clear();
      frame_ = frame;
      parity_ = uint32_t(frame & 1);
      stats_ = FrameReport{};
      stats_.frame = frame;
      report.corrupt = true;
      return true;
    }
    ranges.emplace_back(block.slot_offset, block.slot_offset + block.capacity);
  }
  std::sort(ranges.begin(), ranges.end());

  live_.clear();
  free_.clear();
  uint32_t cursor = 0;
  for (const auto& range : ranges) {
    if (range.first < cursor) {
      Clear();
      frame_ = frame;
      parity_ = uint32_t(frame & 1);
      stats_ = FrameReport{};
      stats_.frame = frame;
      report.corrupt = true;
      return true;
    }
    if (range.first > cursor) {
      free_[cursor] = range.first;
    }
    live_[range.first] = range.second;
    cursor = range.second;
  }
  layout_end_ = cursor;
  report.live_end = cursor;
  report.holes = uint32_t(free_.size());
  for (const auto& hole : free_) {
    report.free_slots += hole.second - hole.first;
  }
  report.tail_free = slot_capacity_ - layout_end_;

  frame_ = frame;
  parity_ = uint32_t(frame & 1);
  stats_ = FrameReport{};
  stats_.frame = frame;
  return true;
}

inline bool MotionClipArena::Allocate(uint64_t key, uint32_t draw_index_min,
                                      uint32_t draw_index_max, uint64_t index_fingerprint,
                                      Draw& draw_out) {
  // A key may be drawn as several primitives. The block is placed once per
  // frame; every later sub-draw reuses that placement and its history decision.
  {
    auto existing = blocks_.find(key);
    if (existing != blocks_.end() && existing->second.allocated_frame == frame_) {
      const Block& block = existing->second;
      draw_out.key = key;
      draw_out.clip_base = parity_ * slot_capacity_ + block.slot_offset;
      draw_out.prev_clip_base = block.frame_prev_clip_base;
      draw_out.index_min = block.index_min;
      draw_out.index_max = block.index_max;
      draw_out.allocated = true;
      if (block.frame_prev_clip_base != UINT32_MAX) {
        ++stats_.history;
      }
      return true;
    }
  }

  const bool reused = blocks_.find(key) != blocks_.end();
  const Block previous = reused ? blocks_.at(key) : Block{};

  // The block is a fixed gl_VertexIndex window: slot = base + (index - min).
  // Its min may only move down, which requires rewriting every slot, so that is
  // a reallocation like growth.
  const uint32_t previous_index_min = reused ? previous.index_min : draw_index_min;
  const uint32_t new_index_min = std::min(previous_index_min, draw_index_min);
  const uint64_t required_span = uint64_t(draw_index_max) - new_index_min + 1;
  const uint32_t slot_cap = std::min(max_draw_slots_, slot_capacity_);
  const uint32_t needed_capacity = uint32_t(std::min<uint64_t>(required_span, slot_cap));
  const bool clamped = required_span > needed_capacity;
  if (needed_capacity == 0) {
    return false;
  }
  const uint32_t previous_capacity = reused ? previous.capacity : 0;
  const uint32_t capacity = std::max(previous_capacity, needed_capacity);
  const bool min_changed = reused && new_index_min != previous.index_min;
  const bool capacity_changed = capacity > previous_capacity;

  uint32_t offset = UINT32_MAX;
  if (!reused) {
    // A block that did not exist last frame has no history; any hole is safe.
    uint32_t candidate = 0;
    bool from_hole = false;
    if (!FindPlacement(needed_capacity, candidate, from_hole)) {
      ++stats_.no_space;
      return false;
    }
    Claim(candidate, needed_capacity, from_hole);
    ++stats_.new_blocks;
    offset = candidate;
  } else if (!capacity_changed) {
    // The reserved interval is unchanged, so the offset must stay: another
    // block may already own the space the old range would have freed. A lowered
    // index_min still invalidates history below but does not move the block.
    offset = previous.slot_offset;
  } else {
    uint32_t candidate = 0;
    if (GrowInPlace(previous, needed_capacity, candidate)) {
      offset = candidate;
    } else {
      // Find the new range before releasing the old one, so a failed placement
      // leaves the block exactly as it was.
      bool from_hole = false;
      if (!FindPlacement(needed_capacity, candidate, from_hole)) {
        ++stats_.no_space;
        return false;
      }
      // Claim before releasing the old range: the release may coalesce with the
      // hole that supplied the new range, which would move the hole's start.
      Claim(candidate, needed_capacity, from_hole);
      EraseBlock(key);
      ++stats_.moved_blocks;
      ++stats_.repacked;
      offset = candidate;
    }
  }

  // All-or-nothing: the previous half is readable only if this exact block was
  // written last frame, the layout kept its offset, the block still covers the
  // whole index set, and the index set is the same one.
  const bool history_valid = reused && !min_changed && !capacity_changed &&
                             previous.capacity != 0 && previous.last_frame != UINT64_MAX &&
                             previous.last_frame + 1 == frame_ && previous.slot_offset == offset &&
                             previous.index_fingerprint == index_fingerprint &&
                             draw_index_max <= previous.index_max;

  Block& block = blocks_[key];
  block.slot_offset = offset;
  block.capacity = capacity;
  block.index_min = new_index_min;
  block.index_max = new_index_min + capacity - 1;
  block.index_fingerprint = index_fingerprint;
  block.allocated_frame = frame_;
  block.frame_prev_clip_base = history_valid ? (parity_ ^ 1) * slot_capacity_ + offset : UINT32_MAX;
  if (reused && (min_changed || capacity_changed)) {
    ++stats_.reallocated;
  }
  if (reused && previous.index_fingerprint != index_fingerprint) {
    ++stats_.index_set_changed;
  }
  if (clamped) {
    ++stats_.clamped;
  }
  if (history_valid) {
    ++stats_.history;
  }

  draw_out.key = key;
  draw_out.clip_base = parity_ * slot_capacity_ + offset;
  draw_out.prev_clip_base = block.frame_prev_clip_base;
  draw_out.index_min = new_index_min;
  draw_out.index_max = new_index_min + capacity - 1;
  draw_out.allocated = true;
  return true;
}

inline void MotionClipArena::Commit(uint64_t key) {
  auto it = blocks_.find(key);
  if (it != blocks_.end()) {
    it->second.last_frame = frame_;
  }
}

inline void MotionClipArena::Clear() {
  blocks_.clear();
  live_.clear();
  free_.clear();
  layout_end_ = 0;
}

inline void MotionClipArena::Forget() {
  Clear();
  frame_ = UINT64_MAX;
  parity_ = 0;
  stats_ = FrameReport{};
}

inline bool MotionClipArena::FindPlacement(uint32_t need, uint32_t& offset, bool& from_hole) {
  // First-fit in ascending offset order. Only new or grown blocks take this
  // path, so the scan is bounded by the number of holes, not by draws.
  for (const auto& hole : free_) {
    ++stats_.ff_steps;
    if (hole.second - hole.first >= need) {
      offset = hole.first;
      from_hole = true;
      return true;
    }
  }
  if (uint64_t(layout_end_) + need <= slot_capacity_) {
    offset = layout_end_;
    from_hole = false;
    return true;
  }
  return false;
}

inline void MotionClipArena::Claim(uint32_t offset, uint32_t need, bool from_hole) {
  if (from_hole) {
    auto hole = free_.find(offset);
    const uint32_t hole_end = hole->second;
    free_.erase(hole);
    if (offset + need < hole_end) {
      free_[offset + need] = hole_end;
    }
    ++stats_.reused;
  } else {
    layout_end_ = offset + need;
    ++stats_.bumped;
  }
  live_[offset] = offset + need;
}

inline bool MotionClipArena::GrowInPlace(const Block& previous, uint32_t needed, uint32_t& offset) {
  if (needed <= previous.capacity) {
    return false;
  }
  const uint32_t old_end = previous.slot_offset + previous.capacity;
  const uint32_t grow = needed - previous.capacity;
  if (old_end == layout_end_) {
    if (uint64_t(old_end) + grow > slot_capacity_) {
      return false;
    }
    layout_end_ = old_end + grow;
    live_[previous.slot_offset] = layout_end_;
    ++stats_.grew;
    offset = previous.slot_offset;
    return true;
  }
  auto hole = free_.find(old_end);
  if (hole == free_.end() || hole->second - hole->first < grow) {
    return false;
  }
  const uint32_t hole_end = hole->second;
  free_.erase(hole);
  if (old_end + grow < hole_end) {
    free_[old_end + grow] = hole_end;
  }
  live_[previous.slot_offset] = old_end + grow;
  ++stats_.grew;
  offset = previous.slot_offset;
  return true;
}

inline void MotionClipArena::Release(uint32_t offset, uint32_t capacity) {
  live_.erase(offset);
  InsertFree(offset, offset + capacity);
}

inline void MotionClipArena::InsertFree(uint32_t begin, uint32_t end) {
  if (begin >= end) {
    return;
  }
  // Coalesce with the successor, then with the predecessor. Both are keyed by
  // the range start, so a hole is never adjacent to another hole afterwards.
  auto next = free_.lower_bound(begin);
  if (next != free_.end() && next->first == end) {
    end = next->second;
    free_.erase(next);
  }
  auto position = free_.lower_bound(begin);
  if (position != free_.begin()) {
    auto previous = std::prev(position);
    if (previous->second == begin) {
      begin = previous->first;
      free_.erase(previous);
    }
  }
  free_[begin] = end;
}

inline void MotionClipArena::EraseBlock(uint64_t key) {
  auto it = blocks_.find(key);
  if (it == blocks_.end()) {
    return;
  }
  Release(it->second.slot_offset, it->second.capacity);
  blocks_.erase(it);
}

inline bool MotionClipArena::Fail(std::string* why, const char* message) const {
  if (why != nullptr) {
    *why = message;
  }
  return false;
}

inline bool MotionClipArena::Validate(std::string* why) const {
  uint32_t live_slots = 0;
  uint32_t previous_end = 0;
  for (const auto& range : live_) {
    if (range.first < previous_end) {
      return Fail(why, "overlapping live ranges");
    }
    if (range.second <= range.first || range.second > slot_capacity_) {
      return Fail(why, "live range outside the half");
    }
    previous_end = range.second;
    live_slots += range.second - range.first;
  }
  if (previous_end != layout_end_) {
    return Fail(why, "layout end is not the live end");
  }
  uint32_t free_slots = 0;
  uint32_t free_end = 0;
  for (const auto& range : free_) {
    if (range.first < free_end || range.second <= range.first || range.second > layout_end_) {
      return Fail(why, "free range outside the layout or not maximal");
    }
    free_end = range.second;
    free_slots += range.second - range.first;
  }
  if (live_slots + free_slots != layout_end_) {
    return Fail(why, "live and free ranges do not cover the layout");
  }
  const uint32_t slot_cap = std::min(max_draw_slots_, slot_capacity_);
  for (const auto& entry : blocks_) {
    const Block& block = entry.second;
    if (block.capacity == 0 || block.capacity > slot_cap ||
        uint64_t(block.slot_offset) + block.capacity > slot_capacity_) {
      return Fail(why, "block outside the half");
    }
    auto live = live_.find(block.slot_offset);
    if (live == live_.end() || live->second != block.slot_offset + block.capacity) {
      return Fail(why, "block has no matching live range");
    }
    if (block.index_max != block.index_min + block.capacity - 1) {
      return Fail(why, "block index window does not match its capacity");
    }
  }
  return true;
}

}  // namespace rex::graphics::vulkan
