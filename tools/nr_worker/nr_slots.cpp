// ============================================================================
// MGPU NR worker - the frame pipeline state machine. See nr_slots.h.
// ============================================================================

#include "nr_slots.h"

#include <cstdio>

namespace nr
{
    const char *slot_state_name(SlotState s)
    {
        switch (s)
        {
        case SlotState::FREE:               return "FREE";
        case SlotState::GAME_COPYING:       return "GAME_COPYING";
        case SlotState::READY_FOR_WORKER:   return "READY_FOR_WORKER";
        case SlotState::WORKER_RUNNING:     return "WORKER_RUNNING";
        case SlotState::READY_FOR_GAME:     return "READY_FOR_GAME";
        case SlotState::GAME_COPYING_BACK:  return "GAME_COPYING_BACK";
        }
        return "?";
    }

    const char *input_slot_name(InputSlot s)
    {
        switch (s)
        {
        case InputSlot::COLOR:          return "COLOR";
        case InputSlot::DEPTH:          return "DEPTH";
        case InputSlot::MOTION_VECTORS: return "MOTION_VECTORS";
        case InputSlot::OUTPUT:         return "OUTPUT";
        case InputSlot::COUNT:          return "COUNT";
        }
        return "?";
    }

    const char *input_slot_metadata_name(InputSlot s)
    {
        // The names MGPU already uses for the same resources. The worker does not
        // discover sources; it is told which of these four a surface is.
        switch (s)
        {
        case InputSlot::COLOR:          return "MGPU_NR_INPUT_COLOR";
        case InputSlot::DEPTH:          return "MGPU_NR_INPUT_DEPTH";
        case InputSlot::MOTION_VECTORS: return "MGPU_NR_INPUT_MOTION_VECTORS";
        case InputSlot::OUTPUT:         return "MGPU_NR_OUTPUT";
        case InputSlot::COUNT:          return "MGPU_NR_COUNT";
        }
        return "MGPU_NR_UNKNOWN";
    }

    SlotRing::SlotRing(std::uint32_t count)
    {
        if (count == 0u) count = 1u;
        if (count > 8u) count = 8u;
        count_ = count;
    }

    int SlotRing::pick_free() const
    {
        for (std::uint32_t i = 0; i < count_; ++i)
        {
            if (slots_[i].state == SlotState::FREE) return (int)i;
        }
        return -1;
    }

    std::uint32_t SlotRing::free_count() const
    {
        std::uint32_t n = 0;
        for (std::uint32_t i = 0; i < count_; ++i)
        {
            if (slots_[i].state == SlotState::FREE) ++n;
        }
        return n;
    }

    std::uint32_t SlotRing::in_flight_count() const
    {
        return count_ - free_count();
    }

    bool SlotRing::all_free() const
    {
        return free_count() == count_;
    }

    bool SlotRing::refuse(std::uint32_t i, const char *why)
    {
        ++illegal_;
        char buf[192];
        std::snprintf(buf, sizeof buf, "slot %u (%s): %s", i,
                      (i < count_) ? slot_state_name(slots_[i].state) : "out of range", why);
        last_refusal_ = buf;
        return false;
    }

    bool SlotRing::begin_game_copy(std::uint32_t i, std::uint32_t frame_id,
                                   std::uint64_t game_fence_will_be)
    {
        if (i >= count_) return refuse(i, "no such frame slot");
        if (slots_[i].state != SlotState::FREE)
        {
            // THE RULE. A slot that is not FREE is still owned by one of the two
            // devices, and overwriting it now would race that device.
            return refuse(i, "the slot is not FREE - one of the two devices has not "
                             "finished with it yet");
        }
        slots_[i].state = SlotState::GAME_COPYING;
        slots_[i].frame_id = frame_id;
        slots_[i].game_required = game_fence_will_be;
        ++frames_started_;
        return true;
    }

    bool SlotRing::game_copy_done(std::uint32_t i, std::uint64_t game_fence)
    {
        if (i >= count_) return refuse(i, "no such frame slot");
        if (slots_[i].state != SlotState::GAME_COPYING)
        {
            return refuse(i, "the game did not begin a copy into this slot");
        }
        slots_[i].game_fence = game_fence;
        slots_[i].state = SlotState::READY_FOR_WORKER;
        return true;
    }

    bool SlotRing::worker_begin(std::uint32_t i, std::uint64_t worker_fence_will_be)
    {
        if (i >= count_) return refuse(i, "no such frame slot");
        if (slots_[i].state != SlotState::READY_FOR_WORKER)
        {
            return refuse(i, "the game has not published this slot yet");
        }
        slots_[i].worker_required = worker_fence_will_be;
        slots_[i].state = SlotState::WORKER_RUNNING;
        return true;
    }

    bool SlotRing::worker_done(std::uint32_t i, std::uint64_t worker_fence)
    {
        if (i >= count_) return refuse(i, "no such frame slot");
        if (slots_[i].state != SlotState::WORKER_RUNNING)
        {
            return refuse(i, "the worker is not running this slot");
        }
        slots_[i].worker_fence = worker_fence;
        slots_[i].state = SlotState::READY_FOR_GAME;
        return true;
    }

    bool SlotRing::begin_game_copyback(std::uint32_t i)
    {
        if (i >= count_) return refuse(i, "no such frame slot");
        if (slots_[i].state != SlotState::READY_FOR_GAME)
        {
            return refuse(i, "the worker has not published the result yet");
        }
        slots_[i].state = SlotState::GAME_COPYING_BACK;
        return true;
    }

    bool SlotRing::release(std::uint32_t i, std::uint64_t game_fence,
                           std::uint64_t worker_fence)
    {
        if (i >= count_) return refuse(i, "no such frame slot");
        if (slots_[i].state != SlotState::GAME_COPYING_BACK)
        {
            return refuse(i, "the game did not begin the copy back");
        }

        // Record what we were told, then check BOTH devices against what this
        // slot requires. The bigger of the observed values is kept, so a stale
        // (lower) reading can never un-complete a fence.
        if (game_fence > slots_[i].game_fence) slots_[i].game_fence = game_fence;
        if (worker_fence > slots_[i].worker_fence) slots_[i].worker_fence = worker_fence;

        if (slots_[i].game_fence < slots_[i].game_required)
        {
            return refuse(i, "the GAME device has not completed this slot's work - "
                             "refusing to make it reusable");
        }
        if (slots_[i].worker_fence < slots_[i].worker_required)
        {
            return refuse(i, "the WORKER device has not completed this slot's work - "
                             "refusing to make it reusable");
        }

        slots_[i].state = SlotState::FREE;
        ++frames_completed_;
        return true;
    }

    bool SlotRing::set_geometry(std::uint32_t i, std::uint32_t width, std::uint32_t height,
                                std::uint32_t dxgi_format, std::uint32_t row_pitch,
                                std::uint32_t slice_pitch, std::uint32_t resource_offset)
    {
        if (i >= count_) return refuse(i, "no such frame slot");
        slots_[i].width = width;
        slots_[i].height = height;
        slots_[i].dxgi_format = dxgi_format;
        slots_[i].row_pitch = row_pitch;
        slots_[i].slice_pitch = slice_pitch;
        slots_[i].resource_offset = resource_offset;
        return true;
    }
}
