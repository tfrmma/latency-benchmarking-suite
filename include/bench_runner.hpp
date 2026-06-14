#pragma once

#include "rdtsc.hpp"
#include "histogram.hpp"
#include "cpu_utils.hpp"

#include <cstdint>
#include <cstdio>
#include <string_view>

namespace bench {

struct BenchConfig {
    uint32_t warmup_iters{10'000};
    uint32_t measure_iters{100'000};
    int      pin_core{-1};
    bool     set_rt_priority{false};
    double   spike_threshold{5.0};
};

// runs fn() measure_iters times with lfence/rdtscp bracketing.
// warmup pass first so caches and branch predictor are settled.
//
// the lfence before rdtsc is not optional — without it the CPU can
// speculatively start fn() before the counter is read and you measure
// nothing. rdtscp on the other side serializes retirement.
// total fence overhead is ~10 cycles. live with it.
template<typename Fn>
[[nodiscard]] Histogram run_bench(
    Fn&& fn,
    const BenchConfig& cfg  = {},
    uint64_t* spike_log     = nullptr,
    uint32_t* spike_count   = nullptr
) noexcept {
    if (cfg.pin_core >= 0) {
        const int err = pin_thread_to_core(cfg.pin_core);
        if (err != 0)
            std::fprintf(stderr, "[bench] pin failed core=%d: %s\n",
                         cfg.pin_core, strerror(err));
    }

    if (cfg.set_rt_priority) (void)set_realtime_priority(80);

    warmup_tsc();

    for (uint32_t i = 0; i < cfg.warmup_iters; ++i) {
        fn();
        compiler_barrier();
    }

    Histogram hist{};
    uint32_t n_spikes = 0;

    for (uint32_t i = 0; i < cfg.measure_iters; ++i) {
        __asm__ volatile("lfence" ::: "memory");
        const uint64_t t0 = rdtsc();

        fn();

        const uint64_t t1 = rdtscp();
        compiler_barrier();

        const uint64_t cycles = t1 - t0;

        if (spike_log && spike_count
                && n_spikes < 64
                && hist.is_spike(cycles, cfg.spike_threshold))
            spike_log[n_spikes++] = cycles;

        hist.record(cycles);
    }

    if (spike_count) *spike_count = n_spikes;
    return hist;
}

inline void print_report(const LatencyReport& r) noexcept {
    std::printf("%-30s  n=%7lu  p50=%7.1fns  p95=%7.1fns  p99=%7.1fns  "
                "p999=%8.1fns  p9999=%9.1fns  mean=%7.1fns  min=%5.1fns  max=%9.1fns",
                r.name.data(), static_cast<unsigned long>(r.samples),
                r.p50_ns, r.p95_ns, r.p99_ns,
                r.p999_ns, r.p9999_ns,
                r.mean_ns, r.min_ns, r.max_ns);
    if (r.overflows > 0)
        std::printf("  [%lu overflows]", static_cast<unsigned long>(r.overflows));
    std::printf("\n");
}

} // namespace bench
