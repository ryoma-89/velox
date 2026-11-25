#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace velox::transport {

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4324)  // Each fence slot occupies its own cache line.
#endif

struct FenceToken {
    std::uint32_t slot = 0;
    std::uint64_t generation = 0;
};

// One lifecycle owner per slot. Completion workers hold a table reference and
// a token, with no native fence pointer. Only an observed success can publish.
class FencePage {
  public:
    enum class DeviceState : std::uint32_t { live, retired, deviceLost };
    static constexpr std::uint32_t kSlots = 64;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<DeviceState>::is_always_lock_free);

    // Called after successful creation or reset. External payloads and other
    // ineligible fences get a generation but always use the synchronous path.
    FenceToken arm(std::uint32_t slot, bool completionCacheEligible) {
        return advance(slot, completionCacheEligible ? State::pending : State::untracked);
    }

    void invalidate(std::uint32_t slot) { advance(slot, State::untracked); }

    bool confirmed(FenceToken token) const {
        return valid(token) &&
               slots_[token.slot].state.load(std::memory_order_acquire) ==
                   pack(token.generation, State::complete) &&
               deviceState() == DeviceState::live;
    }

    bool publishObservedSuccess(FenceToken token) {
        if (!valid(token) || deviceState() != DeviceState::live) return false;
        auto expected = pack(token.generation, State::pending);
        return slots_[token.slot].state.compare_exchange_strong(
            expected, pack(token.generation, State::complete),
            std::memory_order_release, std::memory_order_relaxed);
    }

    // Device loss, disconnect and snapshot transition invalidate this table.
    // The caller owns error propagation and the fallback status RPC.
    void retire() { deactivate(DeviceState::retired); }
    void markDeviceLost() { deactivate(DeviceState::deviceLost); }
    DeviceState deviceState() const { return deviceState_.load(std::memory_order_acquire); }

  private:
    enum class State : std::uint64_t { untracked, pending, complete };
    static constexpr std::uint64_t kMaxGeneration =
        std::numeric_limits<std::uint64_t>::max() >> 2;

    struct alignas(64) Slot {
        std::atomic<std::uint64_t> state{0};
    };

    static std::uint64_t pack(std::uint64_t generation, State state) {
        return (generation << 2) | static_cast<std::uint64_t>(state);
    }

    static bool valid(FenceToken token) {
        return token.slot < kSlots && token.generation && token.generation < kMaxGeneration;
    }

    void deactivate(DeviceState state) {
        auto expected = DeviceState::live;
        deviceState_.compare_exchange_strong(expected, state, std::memory_order_release,
                                             std::memory_order_relaxed);
    }

    FenceToken advance(std::uint32_t slot, State state) {
        if (slot >= kSlots || deviceState() != DeviceState::live) return {};
        auto generation = slots_[slot].state.load(std::memory_order_relaxed) >> 2;
        if (generation == kMaxGeneration) return {};
        ++generation;
        slots_[slot].state.store(pack(generation, state), std::memory_order_release);
        if (generation == kMaxGeneration) return {};
        return {slot, generation};
    }

    std::atomic<DeviceState> deviceState_{DeviceState::live};
    std::array<Slot, kSlots> slots_;
};

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

}  // namespace velox::transport
