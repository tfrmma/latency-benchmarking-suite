#pragma once

#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstring>

namespace bench {

[[nodiscard]] inline uint64_t rdtsc() noexcept {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// rdtscp also stalls until prior instructions retire. use at END of region.
// costs maybe 5 cycles more than rdtsc. worth it for correctness.
[[nodiscard]] inline uint64_t rdtscp() noexcept {
    uint32_t lo, hi, aux;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

inline void compiler_barrier() noexcept { __asm__ volatile("" ::: "memory"); }
inline void memory_fence()     noexcept { __asm__ volatile("mfence" ::: "memory"); }

// three ways to get TSC frequency, in order of preference.
// sysfs is the most accurate — no scheduling jitter, no sleep.
// CPUID 0x15 works on Skylake+ but AMD often returns 0 even when it shouldn't.
// wall_clock fallback has ~0.1% error, which is fine for benchmarking but
// bad if you're doing anything time-critical with the result.

static uint64_t try_sysfs_tsc_hz() noexcept {
    FILE* f = fopen("/sys/devices/system/cpu/cpu0/tsc_freq_khz", "r");
    if (!f) return 0;
    uint64_t khz = 0;
    fscanf(f, "%lu", &khz);
    fclose(f);
    return khz * 1000ULL;
}

static uint64_t try_cpuid_tsc_hz() noexcept {
    uint32_t eax = 0, ebx = 0, ecx = 0;
    __asm__ volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx)
        : "0"(0x15u) : "edx");
    if (eax == 0 || ebx == 0 || ecx == 0) return 0;
    return static_cast<uint64_t>(ecx) * ebx / eax;
}

static uint64_t calibrate_via_sleep(uint32_t sleep_ms) noexcept {
    // bracket order matters: wall clock brackets TSC, not the other way around.
    // previous version had this backwards — both clocks need to measure the
    // same interval or you're adding ~20ns of systematic error.
    struct timespec t0{}, t1{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
    const uint64_t tsc0 = rdtsc();

    struct timespec req{ 0, static_cast<long>(sleep_ms) * 1'000'000L };
    nanosleep(&req, nullptr);

    const uint64_t tsc1 = rdtscp();
    clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

    const uint64_t wall_ns =
        (static_cast<uint64_t>(t1.tv_sec - t0.tv_sec) * 1'000'000'000ULL)
        + static_cast<uint64_t>(t1.tv_nsec - t0.tv_nsec);

    if (wall_ns == 0) return 0;
    return (tsc1 - tsc0) * 1'000'000'000ULL / wall_ns;
}

struct TscCalibration {
    uint64_t    hz{0};
    double      ns_per_cycle{0.0};
    const char* method{"none"};

    static TscCalibration calibrate(uint32_t sleep_ms = 200) noexcept {
        TscCalibration cal{};

        cal.hz = try_sysfs_tsc_hz();
        if (cal.hz > 0) { cal.method = "sysfs"; goto done; }

        cal.hz = try_cpuid_tsc_hz();
        if (cal.hz > 0) { cal.method = "cpuid_0x15"; goto done; }

        cal.hz = calibrate_via_sleep(sleep_ms);
        cal.method = "wall_clock";

    done:
        if (cal.hz > 0)
            cal.ns_per_cycle = 1e9 / static_cast<double>(cal.hz);
        return cal;
    }

    [[nodiscard]] double to_ns(uint64_t cycles) const noexcept {
        return static_cast<double>(cycles) * ns_per_cycle;
    }
    [[nodiscard]] double to_us(uint64_t cycles) const noexcept { return to_ns(cycles) / 1000.0; }
};

// TODO: re-calibrate periodically for long-running processes. invariant TSC
// shouldn't drift but I've seen it happen on some Xeon E5 boards under NUMA
// migration. probably a BIOS bug but "shouldn't happen" is not a guarantee.
inline const TscCalibration& global_tsc() noexcept {
    static TscCalibration cal = TscCalibration::calibrate(200);
    return cal;
}

[[nodiscard]] inline bool has_invariant_tsc() noexcept {
    uint32_t edx = 0;
    __asm__ volatile("cpuid" : "=d"(edx) : "0"(0x80000007u) : "eax", "ebx", "ecx");
    return (edx & (1u << 8)) != 0;
}

inline void warmup_tsc(uint32_t iters = 1000) noexcept {
    volatile uint64_t sink = 0;
    for (uint32_t i = 0; i < iters; ++i) { sink ^= rdtsc(); compiler_barrier(); }
    (void)sink;
}

} // namespace bench
