#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/vulkan/motion_clip_arena.h>

namespace {

using rex::graphics::vulkan::MotionClipArena;

// The production half is 2.1M slots; the tests use a small one so offsets are
// exact and the no-space paths are reachable.
constexpr uint32_t kHalfSlots = 32;
constexpr uint32_t kMaxDrawSlots = 16;

uint32_t SlotOffset(const MotionClipArena& arena, const MotionClipArena::Draw& draw) {
  return draw.clip_base - arena.parity() * arena.half_slots();
}

TEST_CASE("motion clip arena does not invent history for an uncommitted block", "[graphics]") {
  MotionClipArena arena(kHalfSlots, kMaxDrawSlots);
  MotionClipArena::FrameReport report;
  MotionClipArena::Draw draw;

  REQUIRE(arena.BeginFrame(0, report));
  REQUIRE(report.frame == UINT64_MAX);
  REQUIRE(arena.Allocate(1, 0, 7, 0x11, draw));
  // Allocated but never committed (the draw failed before recording).
  REQUIRE(draw.prev_clip_base == UINT32_MAX);

  REQUIRE(arena.BeginFrame(1, report));
  REQUIRE(report.frame == 0);
  REQUIRE(report.freed == 1);
  REQUIRE(arena.Allocate(1, 0, 7, 0x11, draw));
  // A fresh block must start at UINT64_MAX, not 0: 0 would look like a write on
  // frame 0 and pass last_frame + 1 == 1.
  REQUIRE(draw.prev_clip_base == UINT32_MAX);
  REQUIRE(arena.stats().history == 0);
  arena.Commit(1);

  REQUIRE(arena.BeginFrame(2, report));
  REQUIRE(arena.Allocate(1, 0, 7, 0x11, draw));
  REQUIRE(draw.prev_clip_base != UINT32_MAX);
  REQUIRE(arena.stats().history == 1);
}

TEST_CASE("motion clip arena reserves a survivor that has not been drawn yet", "[graphics]") {
  MotionClipArena arena(kHalfSlots, kMaxDrawSlots);
  MotionClipArena::FrameReport report;
  MotionClipArena::Draw a;
  MotionClipArena::Draw b;
  MotionClipArena::Draw c;

  REQUIRE(arena.BeginFrame(1, report));
  REQUIRE(arena.Allocate(1, 0, 7, 0x11, a));
  REQUIRE(arena.Allocate(2, 8, 15, 0x22, b));
  arena.Commit(1);
  arena.Commit(2);
  REQUIRE(SlotOffset(arena, a) == 0);
  REQUIRE(SlotOffset(arena, b) == 8);

  // Frame 2 draws a new block first. It must not take B's range even though B
  // has not been drawn yet this frame: B may still draw later and its previous
  // half is readable.
  REQUIRE(arena.BeginFrame(2, report));
  REQUIRE(arena.Allocate(3, 0, 7, 0x33, c));
  REQUIRE(SlotOffset(arena, c) == 16);
  REQUIRE(arena.Allocate(2, 8, 15, 0x22, b));
  REQUIRE(SlotOffset(arena, b) == 8);
  REQUIRE(b.prev_clip_base != UINT32_MAX);
  REQUIRE(arena.Allocate(1, 0, 7, 0x11, a));
  REQUIRE(SlotOffset(arena, a) == 0);
  arena.Commit(1);
  arena.Commit(2);
  arena.Commit(3);
  REQUIRE(arena.Validate());

  // Frame 3 keeps A and C but not B. Frame 4 prunes B, so its range is free and a
  // new block must be able to take exactly that hole.
  REQUIRE(arena.BeginFrame(3, report));
  REQUIRE(arena.Allocate(1, 0, 7, 0x11, a));
  REQUIRE(arena.Allocate(3, 0, 7, 0x33, c));
  arena.Commit(1);
  arena.Commit(3);
  REQUIRE(arena.BeginFrame(4, report));
  REQUIRE(report.freed == 1);
  REQUIRE(arena.Allocate(4, 8, 15, 0x44, b));
  REQUIRE(SlotOffset(arena, b) == 8);
  REQUIRE(b.prev_clip_base == UINT32_MAX);
  REQUIRE(arena.Validate());
}

TEST_CASE("motion clip arena grows in place only into adjacent free space", "[graphics]") {
  MotionClipArena arena(64, 64);
  MotionClipArena::FrameReport report;
  MotionClipArena::Draw a;
  MotionClipArena::Draw b;

  REQUIRE(arena.BeginFrame(1, report));
  REQUIRE(arena.Allocate(1, 0, 7, 0x11, a));
  arena.Commit(1);

  // The block is at the frontier, so growth extends it without moving it.
  REQUIRE(arena.BeginFrame(2, report));
  REQUIRE(arena.Allocate(1, 0, 15, 0x12, a));
  REQUIRE(SlotOffset(arena, a) == 0);
  REQUIRE(a.prev_clip_base == UINT32_MAX);
  REQUIRE(arena.stats().grew == 1);
  REQUIRE(arena.stats().repacked == 0);
  arena.Commit(1);

  REQUIRE(arena.BeginFrame(3, report));
  REQUIRE(arena.Allocate(1, 0, 15, 0x12, a));
  REQUIRE(a.prev_clip_base != UINT32_MAX);
  arena.Commit(1);

  // Put a block directly above it. A further growth cannot use the old end, so
  // it must move; the freed range is then available to a new block.
  REQUIRE(arena.BeginFrame(4, report));
  REQUIRE(arena.Allocate(2, 16, 23, 0x22, b));
  REQUIRE(SlotOffset(arena, b) == 16);
  arena.Commit(1);
  arena.Commit(2);

  REQUIRE(arena.BeginFrame(5, report));
  REQUIRE(arena.Allocate(1, 0, 31, 0x13, a));
  REQUIRE(SlotOffset(arena, a) == 24);
  REQUIRE(a.prev_clip_base == UINT32_MAX);
  REQUIRE(arena.stats().repacked == 1);
  arena.Commit(1);
  arena.Commit(2);
  MotionClipArena::Draw c;
  REQUIRE(arena.Allocate(3, 0, 7, 0x33, c));
  REQUIRE(SlotOffset(arena, c) == 0);
  REQUIRE(arena.Validate());
}

TEST_CASE("motion clip arena loses history when the index window moves down", "[graphics]") {
  MotionClipArena arena(kHalfSlots, kMaxDrawSlots);
  MotionClipArena::FrameReport report;
  MotionClipArena::Draw draw;

  REQUIRE(arena.BeginFrame(1, report));
  REQUIRE(arena.Allocate(1, 100, 107, 0x11, draw));
  arena.Commit(1);

  // Same capacity, lower min: the slot mapping changes, so the previous half is
  // not readable even though the reserved range did not move.
  REQUIRE(arena.BeginFrame(2, report));
  REQUIRE(arena.Allocate(1, 96, 103, 0x12, draw));
  REQUIRE(draw.prev_clip_base == UINT32_MAX);
  REQUIRE(arena.stats().index_set_changed == 1);
  arena.Commit(1);

  REQUIRE(arena.BeginFrame(3, report));
  REQUIRE(arena.Allocate(1, 96, 103, 0x12, draw));
  REQUIRE(draw.prev_clip_base != UINT32_MAX);
}

TEST_CASE("motion clip arena never keeps history for a clamped draw", "[graphics]") {
  MotionClipArena arena(kHalfSlots, 8);
  MotionClipArena::FrameReport report;
  MotionClipArena::Draw draw;

  REQUIRE(arena.BeginFrame(1, report));
  REQUIRE(arena.Allocate(1, 0, 19, 0x11, draw));
  REQUIRE(arena.stats().clamped == 1);
  REQUIRE(draw.index_max == 7);
  arena.Commit(1);

  REQUIRE(arena.BeginFrame(2, report));
  REQUIRE(arena.Allocate(1, 0, 19, 0x11, draw));
  REQUIRE(draw.prev_clip_base == UINT32_MAX);
}

TEST_CASE("motion clip arena skips one block instead of dropping the frame", "[graphics]") {
  MotionClipArena arena(16, kMaxDrawSlots);
  MotionClipArena::FrameReport report;
  MotionClipArena::Draw a;
  MotionClipArena::Draw b;
  MotionClipArena::Draw c;

  REQUIRE(arena.BeginFrame(1, report));
  REQUIRE(arena.Allocate(1, 0, 7, 0x11, a));
  REQUIRE(arena.Allocate(2, 8, 15, 0x22, b));
  arena.Commit(1);
  arena.Commit(2);

  REQUIRE(arena.BeginFrame(2, report));
  // The half is full. The new block is skipped; the two survivors keep their
  // ranges and their history, which a full-arena reset would have destroyed.
  REQUIRE_FALSE(arena.Allocate(3, 0, 0, 0x33, c));
  REQUIRE(arena.stats().no_space == 1);
  REQUIRE(arena.Allocate(1, 0, 7, 0x11, a));
  REQUIRE(a.prev_clip_base != UINT32_MAX);
  REQUIRE(arena.Allocate(2, 8, 15, 0x22, b));
  REQUIRE(b.prev_clip_base != UINT32_MAX);
  REQUIRE(arena.Validate());
}

TEST_CASE("motion clip arena coalesces adjacent mid-frame holes", "[graphics]") {
  MotionClipArena arena(64, 64);
  MotionClipArena::FrameReport report;
  MotionClipArena::Draw a;
  MotionClipArena::Draw b;
  MotionClipArena::Draw c;
  MotionClipArena::Draw d;

  REQUIRE(arena.BeginFrame(1, report));
  REQUIRE(arena.Allocate(1, 0, 7, 0x11, a));
  REQUIRE(arena.Allocate(2, 8, 15, 0x22, b));
  REQUIRE(arena.Allocate(3, 16, 23, 0x33, c));
  arena.Commit(1);
  arena.Commit(2);
  arena.Commit(3);

  // Frame 2 moves A out of [0,8) and B out of [8,16). The two releases must
  // coalesce into one 16-slot hole, which a new block can then take whole.
  REQUIRE(arena.BeginFrame(2, report));
  REQUIRE(arena.Allocate(1, 0, 23, 0x12, a));
  REQUIRE(SlotOffset(arena, a) == 24);
  REQUIRE(arena.Allocate(2, 8, 23, 0x23, b));
  REQUIRE(SlotOffset(arena, b) == 48);
  REQUIRE(arena.Allocate(4, 0, 15, 0x44, d));
  REQUIRE(SlotOffset(arena, d) == 0);
  REQUIRE(arena.stats().reused == 1);
  REQUIRE(arena.Validate());
}

TEST_CASE("motion clip arena reset clears every range", "[graphics]") {
  MotionClipArena arena(kHalfSlots, kMaxDrawSlots);
  MotionClipArena::FrameReport report;
  MotionClipArena::Draw draw;

  REQUIRE(arena.BeginFrame(1, report));
  REQUIRE(arena.Allocate(1, 0, 7, 0x11, draw));
  arena.Commit(1);
  arena.Clear();
  REQUIRE(arena.layout_end() == 0);
  REQUIRE(arena.block_count() == 0);

  REQUIRE(arena.Allocate(2, 0, 7, 0x22, draw));
  REQUIRE(SlotOffset(arena, draw) == 0);
  REQUIRE(draw.prev_clip_base == UINT32_MAX);
  REQUIRE(arena.Validate());
}

TEST_CASE("motion clip arena denies history to a key drawn twice in one frame", "[graphics]") {
  // Two draw calls carrying one identity key place the same block: the key is
  // the identity, so the map cannot hold two. The slot then holds whichever draw
  // wrote last, so neither draw may leave history behind - the next frame must
  // not read one draw's clip for the other.
  MotionClipArena arena(kHalfSlots, kMaxDrawSlots);
  MotionClipArena::FrameReport report;
  MotionClipArena::Draw first;
  MotionClipArena::Draw second;

  REQUIRE(arena.BeginFrame(1, report));
  REQUIRE(arena.Allocate(7, 0, 7, 0x11, first));
  REQUIRE(first.history_write);
  arena.Commit(7);

  REQUIRE(arena.BeginFrame(2, report));
  REQUIRE(arena.Allocate(7, 0, 7, 0x11, first));
  REQUIRE(first.prev_clip_base != UINT32_MAX);
  REQUIRE(first.history_write);
  arena.Commit(7);

  // The second draw of the same key aliases the first draw's slot. It must not
  // read the previous half either: that half belongs to whichever draw wrote
  // last frame, not necessarily this one.
  REQUIRE(arena.Allocate(7, 0, 7, 0x11, second));
  REQUIRE(second.prev_clip_base == UINT32_MAX);
  REQUIRE_FALSE(second.history_write);
  REQUIRE(arena.stats().aliased == 1);
  arena.Commit(7);

  REQUIRE(arena.BeginFrame(3, report));
  REQUIRE(report.aliased == 1);
  REQUIRE(arena.Allocate(7, 0, 7, 0x11, first));
  REQUIRE(first.prev_clip_base == UINT32_MAX);
  REQUIRE(arena.stats().history == 0);
  REQUIRE(arena.Validate());
}

TEST_CASE("motion clip arena never aliases a previous-half owner", "[graphics]") {
  // Shadow model of the GPU arena: history is valid only if every slot the draw
  // claims to have written last frame is still owned by the same key in the
  // previous half. This is the invariant that a naive "reuse any hole" passes
  // and the survivor-not-drawn case violates.
  MotionClipArena arena(64, kMaxDrawSlots);
  std::mt19937 rng(0xC11A5EEDu);
  std::map<uint32_t, uint64_t> previous_owner;
  std::map<uint32_t, uint64_t> current_owner;

  for (uint64_t frame = 0; frame < 400; ++frame) {
    MotionClipArena::FrameReport report;
    if (!arena.BeginFrame(frame, report)) {
      FAIL("frame did not rotate");
    }
    previous_owner = current_owner;
    current_owner.clear();

    const uint32_t draw_count = rng() % 12;
    std::map<uint64_t, uint32_t> draws_this_frame;
    for (uint32_t i = 0; i < draw_count; ++i) {
      const uint64_t key = rng() % 8;
      const uint32_t min_index = rng() % 24;
      const uint32_t span = 1 + rng() % 8;
      MotionClipArena::Draw draw;
      if (!arena.Allocate(key, min_index, min_index + span - 1, key * 101 + span, draw)) {
        continue;
      }
      // A key drawn a second time in the frame aliases the first draw's block:
      // it may neither read the previous half nor be stamped as history.
      const bool aliased = draws_this_frame[key]++ != 0;
      REQUIRE(draw.history_write == !aliased);
      if (aliased) {
        REQUIRE(draw.prev_clip_base == UINT32_MAX);
      }
      const uint32_t offset = SlotOffset(arena, draw);
      const uint32_t capacity = draw.index_max - draw.index_min + 1;
      REQUIRE(offset + capacity <= 64);
      if (draw.prev_clip_base != UINT32_MAX) {
        REQUIRE(draw.prev_clip_base / 64 == (arena.parity() ^ 1));
        REQUIRE(draw.prev_clip_base % 64 == offset);
        for (uint32_t slot = offset; slot < offset + capacity; ++slot) {
          auto owner = previous_owner.find(slot);
          REQUIRE(owner != previous_owner.end());
          REQUIRE(owner->second == key);
        }
      }
      if (rng() & 1) {
        arena.Commit(key);
        // Commit is a no-op for an aliased block; the shadow model must not
        // record an owner the arena will not honour either.
        if (draw.history_write) {
          for (uint32_t slot = offset; slot < offset + capacity; ++slot) {
            current_owner[slot] = key;
          }
        }
      }
    }
    REQUIRE(arena.stats().aliased ==
            std::count_if(draws_this_frame.begin(), draws_this_frame.end(),
                          [](const auto& entry) { return entry.second > 1; }));
    std::string why;
    const bool valid = arena.Validate(&why);
    INFO(why);
    REQUIRE(valid);
  }
}

}  // namespace
