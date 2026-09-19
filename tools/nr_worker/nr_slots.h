// ============================================================================
// MGPU NR worker - the frame pipeline: FRAME SLOTS and INPUT SLOTS.
//
// TWO DIFFERENT KINDS OF SLOT, DELIBERATELY NAMED DIFFERENTLY
//
//     INPUT SLOT      what the data IS. Four of them, fixed:
//                         COLOR, DEPTH, MOTION_VECTORS, OUTPUT
//                     (OUTPUT travels the other way, but it is the same kind of
//                     thing - a surface that is shared and copied - so it is the
//                     fourth input slot rather than a parallel concept.)
//
//     FRAME SLOT      WHEN the data is. Three of them, in flight:
//                         slot 0, slot 1, slot 2
//
//     A frame in flight therefore covers 4 shared surfaces, and there can be up
//     to 3 frames in flight. 12 surfaces total is the v1 budget.
//
// THE STATE MACHINE, PER FRAME SLOT
//
//     FREE -> GAME_COPYING -> READY_FOR_WORKER -> WORKER_RUNNING
//          -> READY_FOR_GAME -> GAME_COPYING_BACK -> FREE
//
// THE ONE RULE THAT MATTERS
//     A frame slot may not be reused until BOTH devices have completed it. That
//     is not a comment, it is the `release()` precondition: the slot carries the
//     fence value each device must reach, and `release()` refuses while either
//     fence is short. Without it the game's next CopyTextureRegion would race
//     the worker's read of the surface it is overwriting - which on cross-adapter
//     memory in system memory (MEMORY_POOL_L0) is a data race with no driver
//     protection at all.
//
// WHY THE THIRD SLOT EXISTS
//     With one slot the game would wait for the worker every frame. With three,
//     the game copies frame N+1 while the worker still owns N, and the only
//     waits are the two fence waits that a slot's own correctness requires.
//     There is no global per-frame CPU wait anywhere in this design.
//
// ILLEGAL TRANSITIONS ARE RECORDED, NOT IGNORED
//     Every refusal increments a counter and stores the reason. The self-test
//     asserts that a specific set of wrong calls is refused, so this cannot
//     silently degrade into a state machine that accepts anything.
// ============================================================================

#pragma once

#include <cstdint>
#include <string>

namespace nr
{
    enum class SlotState : std::uint32_t
    {
        FREE = 0,
        GAME_COPYING = 1,
        READY_FOR_WORKER = 2,
        WORKER_RUNNING = 3,
        READY_FOR_GAME = 4,
        GAME_COPYING_BACK = 5,
    };

    const char *slot_state_name(SlotState s);

    enum class InputSlot : std::uint32_t
    {
        COLOR = 0,
        DEPTH = 1,
        MOTION_VECTORS = 2,
        OUTPUT = 3,
        COUNT = 4,
    };

    const char *input_slot_name(InputSlot s);
    const char *input_slot_metadata_name(InputSlot s);

    //: One frame in flight.
    struct FrameSlot
    {
        SlotState     state = SlotState::FREE;
        std::uint32_t frame_id = 0;

        //: The last fence value each device is known to have completed for this
        //: slot, and the value it must reach before the slot may be reused.
        std::uint64_t game_fence = 0;
        std::uint64_t worker_fence = 0;
        std::uint64_t game_required = 0;
        std::uint64_t worker_required = 0;

        //: Metadata the game published for this frame, for the log and the
        //: benchmark JSON. Geometry only - never pixels.
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t dxgi_format = 0;
        std::uint32_t row_pitch = 0;
        std::uint32_t slice_pitch = 0;
        std::uint32_t resource_offset = 0;
    };

    //: The ring of frame slots, with the transitions as methods so that the rule
    //: "both devices completed it" is enforced in exactly one place.
    class SlotRing
    {
    public:
        explicit SlotRing(std::uint32_t count = 3);

        std::uint32_t count() const { return count_; }
        const FrameSlot &at(std::uint32_t i) const { return slots_[i]; }

        //: The first FREE slot, or -1 when every slot is still in flight. The
        //: caller decides what to do about a full ring; this never blocks.
        int pick_free() const;

        //: How many slots are FREE / in use, for the report.
        std::uint32_t free_count() const;
        std::uint32_t in_flight_count() const;

        // ---- the transitions ------------------------------------------------
        bool begin_game_copy(std::uint32_t i, std::uint32_t frame_id,
                             std::uint64_t game_fence_will_be);
        bool game_copy_done(std::uint32_t i, std::uint64_t game_fence);
        bool worker_begin(std::uint32_t i, std::uint64_t worker_fence_will_be);
        bool worker_done(std::uint32_t i, std::uint64_t worker_fence);
        bool begin_game_copyback(std::uint32_t i);
        bool release(std::uint32_t i, std::uint64_t game_fence, std::uint64_t worker_fence);

        //: Publish the geometry the game reported (metadata only).
        bool set_geometry(std::uint32_t i, std::uint32_t width, std::uint32_t height,
                          std::uint32_t dxgi_format, std::uint32_t row_pitch,
                          std::uint32_t slice_pitch, std::uint32_t resource_offset);

        // ---- bookkeeping for the report -------------------------------------
        std::uint32_t illegal_transitions() const { return illegal_; }
        const std::string &last_refusal() const { return last_refusal_; }
        std::uint32_t frames_started() const { return frames_started_; }
        std::uint32_t frames_completed() const { return frames_completed_; }

        //: True when every slot is FREE. A shutdown that leaves this false has
        //: work in flight, which the log must say rather than hide.
        bool all_free() const;

    private:
        bool refuse(std::uint32_t i, const char *why);

        std::uint32_t count_ = 3;
        FrameSlot     slots_[8];
        std::uint32_t illegal_ = 0;
        std::uint32_t frames_started_ = 0;
        std::uint32_t frames_completed_ = 0;
        std::string   last_refusal_;
    };
}
