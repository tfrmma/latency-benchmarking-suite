#pragma once

#include <cstdint>
#include <cstddef>
#include <pthread.h>
#include <sched.h>
#include <cerrno>
#include <cstring>
#include <sys/mman.h>

namespace bench {

// pin this thread to core_id. if this fails, your latency numbers are noise.
// the OS will migrate you mid-measurement and you'll get a spike that looks
// like a bug in your code when it's actually a bug in your setup.
[[nodiscard]] inline int pin_thread_to_core(int core_id) noexcept {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

// SCHED_FIFO to reduce timer interrupts mid-measurement.
// needs CAP_SYS_NICE or root. not fatal if it fails.
[[nodiscard]] inline int set_realtime_priority(int prio = 80) noexcept {
    struct sched_param sp{ .sched_priority = prio };
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

inline void prefetch_read(const void* ptr, int lines = 1) noexcept {
    for (int i = 0; i < lines; ++i)
        __builtin_prefetch(static_cast<const char*>(ptr) + i * 64, 0, 3);
}

// touches every cache line in the region to force pages and cache lines
// into L1/L2 before timing starts. if you skip this you're measuring
// cold-start behavior, which is interesting exactly once.
inline void cache_warm_region(const void* ptr, size_t size_bytes) noexcept {
    const volatile char* p = static_cast<const volatile char*>(ptr);
    volatile char sink = 0;
    for (size_t i = 0; i < size_bytes; i += 64) sink ^= p[i];
    (void)sink;
}

[[nodiscard]] inline int lock_memory(void* ptr, size_t n) noexcept {
    return mlock(ptr, n);
}

// spin for a bit to kick the CPU into max turbo before benchmarking.
// yes this is ugly. the correct fix is `cpupower frequency-set -g performance`
// but that needs root and I'm not writing a setup script.
// TODO: check /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor and warn
// if it's not "performance". would save a lot of "why are my numbers low" grief.
inline void request_max_frequency() noexcept {
    volatile uint64_t x = 1;
    for (uint32_t i = 0; i < 1'000'000; ++i) x = x * 6364136223846793005ULL + 1;
    (void)x;
}

} // namespace bench
