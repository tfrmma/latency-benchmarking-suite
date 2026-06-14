#include "../include/rdtsc.hpp"
#include "../include/histogram.hpp"
#include "../include/cpu_utils.hpp"
#include "../include/bench_runner.hpp"
#include "../include/bench_config.hpp"
#include "../include/report_collector.hpp"

#include <cstdint>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstring>

// contention benchmarks: mutex vs gen-counter vs seqlock under a live writer.
//
// the point isn't "mutexes are slow" (everyone knows). the point is to put
// actual nanosecond numbers on paper so you can argue in a design review.
// "seqlock is 15x faster than mutex" is more convincing than "it should be faster".

namespace bench {
namespace contention {

struct alignas(64) MutexPrice {
    mutable std::mutex mtx;
    double bid{0.0};
    double ask{0.0};
};

// gen-counter seqlock variant. writer odd->update->even, reader checks even+stable.
// GenCounterPrice and SpscPrice are structurally identical except for the seq type
// and cache line layout. kept separate because the alignment matters for the numbers.
struct alignas(64) GenCounterPrice {
    std::atomic<uint32_t> gen{0};
    std::atomic<double>   bid{0.0};
    std::atomic<double>   ask{0.0};

    void write(double b, double a) noexcept {
        gen.fetch_add(1, std::memory_order_release);
        bid.store(b, std::memory_order_relaxed);
        ask.store(a, std::memory_order_relaxed);
        gen.fetch_add(1, std::memory_order_release);
    }

    bool try_read(double& b, double& a) const noexcept {
        const uint32_t g0 = gen.load(std::memory_order_acquire);
        if (g0 & 1) return false;
        b = bid.load(std::memory_order_relaxed);
        a = ask.load(std::memory_order_relaxed);
        return g0 == gen.load(std::memory_order_acquire);
    }
};

// seq and data on separate cache lines. false sharing between them would
// dominate the measurement and give you garbage numbers.
// the previous version didn't do this. it was wrong.
struct SpscPrice {
    alignas(64) std::atomic<uint64_t> seq{0};
    char _pad[56]{};
    alignas(64) std::atomic<double> bid{0.0};
    std::atomic<double> ask{0.0};

    void write(double b, double a) noexcept {
        seq.fetch_add(1, std::memory_order_release);
        bid.store(b, std::memory_order_relaxed);
        ask.store(a, std::memory_order_relaxed);
        seq.fetch_add(1, std::memory_order_release);
    }

    bool try_read(double& b, double& a) const noexcept {
        const uint64_t s0 = seq.load(std::memory_order_acquire);
        if (s0 & 1) return false;
        b = bid.load(std::memory_order_relaxed);
        a = ask.load(std::memory_order_relaxed);
        return s0 == seq.load(std::memory_order_acquire);
    }
};

// shared setup for the writer threads. three benchmarks, same pattern.
static double make_writer_bound(const BookConfig& b) noexcept {
    return b.base_price + 1000 * b.tick_size;
}

void bench_mutex(const Config& cfg) {
    MutexPrice state{};
    state.bid = cfg.book.base_price;
    state.ask = cfg.book.base_price + cfg.book.tick_size;
    std::atomic<bool> stop{false};

    std::thread writer([&]() {
        (void)pin_thread_to_core(cfg.run.writer_core);
        double p = cfg.book.base_price;
        const double bound = make_writer_bound(cfg.book);
        while (!stop.load(std::memory_order_relaxed)) {
            { std::lock_guard<std::mutex> lk(state.mtx); state.bid = p; state.ask = p + cfg.book.tick_size; }
            if ((p += cfg.book.tick_size) > bound) p = cfg.book.base_price;
        }
    });

    // intentionally NOT warming the mutex cache line. the writer is hammering it.
    // warming it here would give you a misleadingly good first few measurements.

    BenchConfig bc{};
    bc.warmup_iters    = cfg.run.warmup_iters;
    bc.measure_iters   = cfg.run.measure_iters;
    bc.pin_core        = cfg.run.pin_core;
    bc.spike_threshold = cfg.run.spike_threshold;

    volatile double sb = 0.0, sa = 0.0;
    const auto hist = run_bench([&]() noexcept {
        std::lock_guard<std::mutex> lk(state.mtx);
        sb = state.bid; sa = state.ask;
    }, bc);

    stop.store(true); writer.join();
    (void)sb; (void)sa;

    const auto& tsc = global_tsc();
    const auto  rpt = LatencyReport::from_histogram(hist, tsc.ns_per_cycle, "mutex_contended_read");
    print_report(rpt);
    collect_report(rpt);
}

void bench_gen_counter(const Config& cfg) {
    GenCounterPrice state{};
    state.write(cfg.book.base_price, cfg.book.base_price + cfg.book.tick_size);
    std::atomic<bool> stop{false};

    std::thread writer([&]() {
        (void)pin_thread_to_core(cfg.run.writer_core);
        double p = cfg.book.base_price;
        const double bound = make_writer_bound(cfg.book);
        while (!stop.load(std::memory_order_relaxed)) {
            state.write(p, p + cfg.book.tick_size);
            if ((p += cfg.book.tick_size) > bound) p = cfg.book.base_price;
        }
    });

    BenchConfig bc{};
    bc.warmup_iters    = cfg.run.warmup_iters;
    bc.measure_iters   = cfg.run.measure_iters;
    bc.pin_core        = cfg.run.pin_core;
    bc.spike_threshold = cfg.run.spike_threshold;

    volatile double sb = 0.0, sa = 0.0;
    const auto hist = run_bench([&]() noexcept {
        double b, a;
        while (!state.try_read(b, a)) __builtin_ia32_pause();
        sb = b; sa = a;
    }, bc);

    stop.store(true); writer.join();
    (void)sb; (void)sa;

    const auto& tsc = global_tsc();
    const auto  rpt = LatencyReport::from_histogram(hist, tsc.ns_per_cycle, "gen_counter_read");
    print_report(rpt);
    collect_report(rpt);
}

void bench_spsc_seqlock(const Config& cfg) {
    SpscPrice state{};
    state.write(cfg.book.base_price, cfg.book.base_price + cfg.book.tick_size);
    std::atomic<bool> stop{false};

    std::thread writer([&]() {
        (void)pin_thread_to_core(cfg.run.writer_core);
        double p = cfg.book.base_price;
        const double bound = make_writer_bound(cfg.book);
        while (!stop.load(std::memory_order_relaxed)) {
            state.write(p, p + cfg.book.tick_size);
            if ((p += cfg.book.tick_size) > bound) p = cfg.book.base_price;
        }
    });

    cache_warm_region(&state, sizeof(state));

    BenchConfig bc{};
    bc.warmup_iters    = cfg.run.warmup_iters;
    bc.measure_iters   = cfg.run.measure_iters;
    bc.pin_core        = cfg.run.pin_core;
    bc.spike_threshold = cfg.run.spike_threshold;

    volatile double sb = 0.0, sa = 0.0;
    const auto hist = run_bench([&]() noexcept {
        double b, a;
        while (!state.try_read(b, a)) __builtin_ia32_pause();
        sb = b; sa = a;
    }, bc);

    stop.store(true); writer.join();
    (void)sb; (void)sa;

    const auto& tsc = global_tsc();
    const auto  rpt = LatencyReport::from_histogram(hist, tsc.ns_per_cycle, "spsc_seqlock_read");
    print_report(rpt);
    collect_report(rpt);
}

void run(const Config& cfg) {
    std::printf("  [contention: reader=core%d  writer=core%d]\n",
                cfg.run.pin_core, cfg.run.writer_core);
    bench_mutex(cfg);
    bench_gen_counter(cfg);
    bench_spsc_seqlock(cfg);
}

} // namespace contention
} // namespace bench

void bench_contention(const bench::Config& cfg) { bench::contention::run(cfg); }
