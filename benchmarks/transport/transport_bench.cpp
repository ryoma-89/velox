#include "duplex.h"
#include "shared_fence.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

namespace {
using namespace velox::transport;
using Clock = std::chrono::steady_clock;

double cpuSeconds() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    const auto ticks = [](FILETIME time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) * 1e-7;
#else
    timespec time{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time) != 0) return -1;
    return time.tv_sec + time.tv_nsec * 1e-9;
#endif
}

bool transferTest() {
    auto shared = std::make_unique<Shared>(8, std::numeric_limits<std::uint32_t>::max() - 12);
    Guest guest(*shared);
    Host host(*shared);
    const std::vector<std::byte> payload(kBulkBytes, std::byte{57});
    const auto expected = checksum(payload);
    const std::array<Call, kCapacity> entries{Call{payload}, Call{payload}};
    const std::vector<std::byte> tooLarge(kBulkBytes + 1);
    const std::array oversize{Call{tooLarge}};
    if (guest.send(oversize) != Outcome::invalid) return false;

    auto worker = std::thread([&] { host.run(0); });
    bool verified = true;
    for (unsigned batch = 0; verified && batch < 32; ++batch) {
        verified = guest.send(entries) == Outcome::ready &&
                   guest.send(entries) == Outcome::backpressure;
        for (unsigned i = 0; verified && i < kCapacity; ++i) {
            Reply reply{};
            verified = guest.receive(reply, 0) == Outcome::ready &&
                       reply.size == (i < 2 ? kBulkBytes : 0) &&
                       reply.checksum == (i < 2 ? expected : 0);
        }
    }
    shared->close();
    worker.join();
    return verified && shared->fault == Fault::closed;
}

bool requestTest() {
    const Request faults[] = {
        {1, 2, 0, 0, 0},                              // wrong generation
        {2, 3, 0, 0, 0},                              // wrong request ID
        {1, 3, 9, 0, 0},                              // unknown handle
        {1, 3, kUploadHandle, kCapacity * kBulkBytes, 1}, // outside the mapping
        {1, 3, kUploadHandle, kBulkBytes, 1},           // belongs to another slot
        {1, 3, kUploadHandle, 0, kBulkBytes + 1},       // crosses the slot boundary
    };
    for (const Request& req : faults) {
        auto shared = std::make_unique<Shared>(3);
        shared->requests.slots[0] = req;
        shared->requests.produced.store(1, std::memory_order_release);
        Host(*shared).run(0);
        if (shared->fault != Fault::request || shared->replies.produced != 0) return false;
    }
    auto shared = std::make_unique<Shared>(3);
    shared->requests.produced.store(kCapacity + 1, std::memory_order_release);
    Host(*shared).run(0);
    if (shared->fault != Fault::cursor) return false;

    auto next = std::make_unique<Shared>(4);
    Guest guest(*next);
    const std::array one{Call{}};
    if (guest.send(one) != Outcome::ready) return false;
    next->replies.slots[0] = {1, 3, 0, 0};
    next->replies.produced.store(1, std::memory_order_release);
    Reply reply{};
    return guest.receive(reply, 0) == Outcome::closed && next->fault == Fault::reply;
}

bool closeTest() {
    auto shared = std::make_unique<Shared>(9);
    Guest guest(*shared);
    Host host(*shared);
    auto worker = std::thread([&] { host.run(0); });
    const std::array<Call, kCapacity> entries{};
    const auto epoch = shared->results.epoch.load(std::memory_order_acquire);
    bool verified = guest.send(entries) == Outcome::ready;
    if (verified) {
        shared->results.epoch.wait(epoch, std::memory_order_acquire);
        verified = shared->replies.produced == kCapacity &&
                   guest.send(entries) == Outcome::backpressure;
    }
    shared->close();
    worker.join();
    if (!verified) return false;

    auto next = std::make_unique<Shared>(10);
    Guest receiver(*next);
    if (receiver.send(entries) != Outcome::ready) return false;
    std::atomic<bool> starting{false};
    Outcome result = Outcome::invalid;
    auto waitingReceiver = std::thread([&] {
        starting.store(true, std::memory_order_release);
        starting.notify_one();
        Reply reply{};
        result = receiver.receive(reply, 0);
    });
    starting.wait(false, std::memory_order_acquire);
    next->close();
    waitingReceiver.join();
    return result == Outcome::closed;
}

bool fenceTest() {
    FencePage page;
    const auto first = page.arm(0, true);
    const auto otherQueue = page.arm(1, true);
    if (page.confirmed(first) || page.confirmed(otherQueue)) return false;
    if (!page.publishObservedSuccess(otherQueue) || !page.confirmed(otherQueue) ||
        page.confirmed(first)) return false;
    auto late = std::thread([&] { page.publishObservedSuccess(first); });
    const auto afterReset = page.arm(0, true);
    late.join();
    if (page.confirmed(first) || page.confirmed(afterReset) ||
        page.publishObservedSuccess(first)) return false;
    if (!page.publishObservedSuccess(afterReset) || !page.confirmed(afterReset)) return false;
    page.invalidate(0);
    const auto external = page.arm(0, false);
    if (page.confirmed(afterReset) || page.confirmed(external) ||
        page.publishObservedSuccess(external)) return false;
    page.retire();
    if (page.deviceState() != FencePage::DeviceState::retired || page.confirmed(otherQueue) ||
        page.arm(0, true).generation || page.confirmed({FencePage::kSlots, 1})) return false;

    FencePage lostDevice;
    const auto completed = lostDevice.arm(0, true);
    const auto pending = lostDevice.arm(1, true);
    if (!lostDevice.publishObservedSuccess(completed) || !lostDevice.confirmed(completed)) return false;
    lostDevice.markDeviceLost();
    lostDevice.retire();
    return lostDevice.deviceState() == FencePage::DeviceState::deviceLost &&
           !lostDevice.confirmed(completed) && !lostDevice.publishObservedSuccess(pending);
}

bool verify() {
    return negotiate(2, 2) == kVersion && negotiate(2, 1) == 1 &&
           negotiate(1, 2) == 1 && transferTest() && requestTest() && closeTest() && fenceTest();
}

int measure(unsigned rounds, unsigned batch, unsigned spinBudget) {
    constexpr unsigned warmup = 1000;
    auto shared = std::make_unique<Shared>(1);
    Guest guest(*shared);
    Host host(*shared);
    const std::array<Call, kCapacity> entries{};
    const auto calls = std::span(entries).first(batch);
    std::vector<double> samples(rounds);
    auto worker = std::thread([&] { host.run(spinBudget); });
    double cpu = 0;
    auto start = Clock::now();
    bool verified = true;
    for (unsigned i = 0; i < warmup + rounds && verified; ++i) {
        if (i == warmup) {
            cpu = cpuSeconds();
            start = Clock::now();
        }
        const auto begin = Clock::now();
        verified = guest.send(calls) == Outcome::ready;
        for (unsigned j = 0; verified && j < batch; ++j) {
            Reply reply{};
            verified = guest.receive(reply, spinBudget) == Outcome::ready && reply.size == 0;
        }
        if (i >= warmup) {
            samples[i - warmup] = std::chrono::duration<double, std::micro>(Clock::now() - begin)
                                      .count();
        }
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    const double cpuEnd = cpuSeconds();
    shared->close();
    worker.join();
    if (!verified || shared->fault != Fault::closed) return 1;
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](unsigned p) { return samples[(rounds - 1) * p / 100]; };

    FencePage page;
    const auto complete = page.arm(0, true);
    const auto pending = page.arm(1, true);
    if (!page.publishObservedSuccess(complete)) return 1;
    std::uint64_t confirmations = 0;
    const auto timeStatus = [&](FenceToken token) {
        const auto begin = Clock::now();
        for (unsigned i = 0; i < rounds; ++i) confirmations += page.confirmed(token);
        return std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / rounds;
    };
    const double confirmedNs = timeStatus(complete);
    const double fallbackNs = timeStatus(pending);
    if (confirmations != rounds) return 1;

    std::printf("{\"protocol\":%u,\"rounds\":%u,\"warmup\":%u,\"batch\":%u,\"spin_budget\":%u,"
                "\"p50_us\":%.3f,\"p95_us\":%.3f,\"p99_us\":%.3f,\"calls_per_s\":%.1f,"
                "\"cpu_seconds\":%.6f,\"wall_seconds\":%.6f,"
                "\"host_wait_calls_incl_warmup\":%llu,\"guest_wait_calls_incl_warmup\":%llu,"
                "\"fence_confirmed_load_ns\":%.3f,\"fence_fallback_load_ns\":%.3f}\n",
                kVersion, rounds, warmup, batch, spinBudget, percentile(50), percentile(95),
                percentile(99), static_cast<double>(rounds) * batch / elapsed,
                cpu < 0 || cpuEnd < 0 ? -1.0 : cpuEnd - cpu, elapsed,
                static_cast<unsigned long long>(host.waits),
                static_cast<unsigned long long>(guest.waits), confirmedNs, fallbackNs);
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--test") {
        const bool passed = verify();
        std::printf("{\"protocol\":%u,\"checks_passed\":%s}\n", kVersion,
                    passed ? "true" : "false");
        return passed ? 0 : 1;
    }
    unsigned rounds = 20000, batch = 1, spins = 0;
    for (int i = 1; i < argc; i += 2) {
        const std::string_view option(argv[i]);
        if (i + 1 == argc) return 2;
        const std::string_view arg(argv[i + 1]);
        unsigned value = 0;
        const auto parsed = std::from_chars(arg.data(), arg.data() + arg.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != arg.data() + arg.size()) return 2;
        if (option == "--rounds" && value && value <= 2000000) {
            rounds = value;
        } else if (option == "--batch" && value && value <= kCapacity) {
            batch = value;
        } else if (option == "--spins" && value <= 1024) {
            spins = value;
        } else {
            return 2;
        }
    }
    return measure(rounds, batch, spins);
}
