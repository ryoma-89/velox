#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <thread>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace velox::transport {

inline constexpr std::uint32_t kVersion = 2;
inline constexpr std::uint32_t kCapacity = 64;
inline constexpr std::uint32_t kBulkBytes = 64 * 1024;
inline constexpr std::uint32_t kUploadHandle = 1;
inline constexpr std::uint32_t kIndexMask = kCapacity - 1;

struct Request {
    std::uint64_t id;
    std::uint32_t generation;
    std::uint32_t handle;
    std::uint32_t offset;
    std::uint32_t size;
};

struct Reply {
    std::uint64_t id;
    std::uint32_t generation;
    std::uint32_t size;
    std::uint64_t checksum;
};

static_assert(sizeof(Request) == 24 && sizeof(Reply) == 24);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

inline std::uint32_t negotiate(std::uint32_t hostVersion, std::uint32_t guestVersion) {
    return hostVersion == kVersion && guestVersion == kVersion ? kVersion : 1;
}

inline std::uint64_t checksum(std::span<const std::byte> data) {
    std::uint64_t value = 0;
    for (auto byte : data) {
        value = (value * 31) + std::to_integer<unsigned>(byte);
    }
    return value;
}

template <typename Record>
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4324)  // Producer and consumer fields occupy separate cache lines.
#endif
struct Ring {
    explicit Ring(std::uint32_t start) : produced(start), consumed(start) {}

    alignas(64) std::atomic<std::uint32_t> produced;
    alignas(64) std::atomic<std::uint32_t> consumed;
    alignas(64) std::array<Record, kCapacity> slots{};
};

struct Doorbell {
    std::atomic<std::uint32_t> epoch{0};

    void notify() {
        epoch.fetch_add(1, std::memory_order_release);
        epoch.notify_one();
    }

    bool wait(std::uint32_t observed, unsigned spinBudget) {
        for (unsigned i = 0; i < spinBudget; ++i) {
            if (epoch.load(std::memory_order_acquire) != observed) return false;
#if defined(_M_X64) || defined(__x86_64__)
            _mm_pause();
#else
            std::this_thread::yield();
#endif
        }
        epoch.wait(observed, std::memory_order_acquire);
        return true;
    }
};

enum class Fault : std::uint32_t { none, closed, cursor, request, reply };
enum class Outcome { ready, backpressure, closed, invalid };

// This prototype uses one guest thread and one renderer thread inside one process.
// Doorbell uses the local OS wait primitive, which must be replaced across a VM.
struct Shared {
    Shared(std::uint32_t generation, std::uint32_t start = 0)
        : generation(generation), start(start), requests(start), replies(start) {}

    const std::uint32_t generation;
    const std::uint32_t start;
    Ring<Request> requests;
    Ring<Reply> replies;
    alignas(64) Doorbell commands;
    alignas(64) Doorbell results;
    alignas(64) std::atomic<Fault> fault{Fault::none};
    alignas(64) std::array<std::byte, kCapacity * kBulkBytes> bulk{};

    bool open() const { return fault.load(std::memory_order_acquire) == Fault::none; }

    void close(Fault reason = Fault::closed) {
        Fault expected = Fault::none;
        fault.compare_exchange_strong(expected, reason, std::memory_order_acq_rel);
        commands.notify();
        results.notify();
    }
};
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

struct Call {
    std::span<const std::byte> upload;
};

class Guest {
  public:
    explicit Guest(Shared& shared) : shared_(shared), sent_(shared.start), received_(shared.start) {}

    Outcome send(std::span<const Call> calls) {
        if (!shared_.open()) return Outcome::closed;
        if (calls.empty() || calls.size() > kCapacity) return Outcome::invalid;
        if (calls.size() > std::numeric_limits<std::uint64_t>::max() - nextRequest_) {
            shared_.close(Fault::request);
            return Outcome::closed;
        }
        // A reply, including its bulk slot, owns one credit until it is read.
        if (calls.size() > kCapacity - (sent_ - received_)) return Outcome::backpressure;
        for (const auto& call : calls) {
            if (call.upload.size() > kBulkBytes) return Outcome::invalid;
        }
        for (const auto& call : calls) {
            const std::uint32_t slot = sent_ & kIndexMask;
            Request req{nextRequest_++, shared_.generation, 0, 0, 0};
            if (!call.upload.empty()) {
                req.handle = kUploadHandle;
                req.offset = slot * kBulkBytes;
                req.size = static_cast<std::uint32_t>(call.upload.size());
                std::memcpy(shared_.bulk.data() + req.offset, call.upload.data(), req.size);
            }
            shared_.requests.slots[slot] = req;
            ++sent_;
        }
        shared_.requests.produced.store(sent_, std::memory_order_release);
        shared_.commands.notify();
        return Outcome::ready;
    }

    Outcome receive(Reply& result, unsigned spinBudget) {
        while (shared_.open()) {
            if (sent_ == received_) return Outcome::invalid;
            const auto epoch = shared_.results.epoch.load(std::memory_order_acquire);
            const auto available = shared_.replies.produced.load(std::memory_order_acquire) - received_;
            if (available > sent_ - received_) {
                shared_.close(Fault::cursor);
                return Outcome::closed;
            }
            if (available) {
                result = shared_.replies.slots[received_ & kIndexMask];
                if (result.generation != shared_.generation || result.id != nextReply_) {
                    shared_.close(Fault::reply);
                    return Outcome::closed;
                }
                ++nextReply_;
                shared_.replies.consumed.store(++received_, std::memory_order_release);
                return Outcome::ready;
            }
            if (!shared_.open()) break;
            waits += shared_.results.wait(epoch, spinBudget);
        }
        return Outcome::closed;
    }

    std::uint64_t waits = 0;

  private:
    Shared& shared_;
    std::uint32_t sent_;
    std::uint32_t received_;
    std::uint64_t nextRequest_ = 1;
    std::uint64_t nextReply_ = 1;
};

class Host {
  public:
    explicit Host(Shared& shared)
        : shared_(shared), served_(shared.start), retired_(shared.start) {}

    void run(unsigned spinBudget) {
        while (shared_.open()) {
            const auto epoch = shared_.commands.epoch.load(std::memory_order_acquire);
            const auto available = shared_.requests.produced.load(std::memory_order_acquire) - served_;
            if (available > kCapacity) {
                shared_.close(Fault::cursor);
                return;
            }
            if (!available) {
                if (!shared_.open()) return;
                waits += shared_.commands.wait(epoch, spinBudget);
                continue;
            }
            const auto retired = shared_.replies.consumed.load(std::memory_order_acquire);
            const auto outstanding = served_ - retired_;
            if (retired - retired_ > outstanding || available > kCapacity - (served_ - retired)) {
                shared_.close(Fault::cursor);
                return;
            }
            retired_ = retired;
            for (std::uint32_t i = 0; i < available; ++i) {
                if (!shared_.open() || !serve()) return;
            }
            shared_.requests.consumed.store(served_, std::memory_order_release);
            shared_.replies.produced.store(served_, std::memory_order_release);
            shared_.results.notify();
        }
    }

    std::uint64_t waits = 0;

  private:
    bool serve() {
        const auto slot = served_ & kIndexMask;
        const Request req = shared_.requests.slots[slot];
        const bool ping = req.handle == 0 && req.offset == 0 && req.size == 0;
        const bool upload = req.handle == kUploadHandle && req.offset == slot * kBulkBytes &&
                            req.size > 0 && req.size <= kBulkBytes;
        if (req.generation != shared_.generation || req.id != nextRequest_ || (!ping && !upload)) {
            shared_.close(Fault::request);
            return false;
        }
        auto value = upload ? checksum(std::span(shared_.bulk).subspan(req.offset, req.size)) : 0;
        shared_.replies.slots[slot] = Reply{req.id, req.generation, req.size, value};
        ++nextRequest_;
        ++served_;
        return true;
    }

    Shared& shared_;
    std::uint32_t served_;
    std::uint32_t retired_;
    std::uint64_t nextRequest_ = 1;
};

}  // namespace velox::transport
