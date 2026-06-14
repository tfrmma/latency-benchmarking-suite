#include "../include/rdtsc.hpp"
#include "../include/histogram.hpp"
#include "../include/cpu_utils.hpp"
#include "../include/bench_runner.hpp"
#include "../include/bench_config.hpp"
#include "../include/report_collector.hpp"

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace bench {
namespace orderbook {

// flat sorted array per side. no map, no tree, no ceremony.
// at depth <= 32 this is faster than anything with pointer chasing
// because it fits in a couple of cache lines.
struct alignas(64) Side {
    double   px[32]{};       // prices, sorted
    double   qty[32]{};
    double   cumqty[32]{};   // prefix sum — used by SOR for fill estimation
    uint32_t count{0};
    uint32_t depth{0};
};

// recompute prefix sums after any update. boring but necessary.
static void recompute_cum(Side& s) noexcept {
    double running = 0.0;
    for (uint32_t i = 0; i < s.count; ++i) {
        running  += s.qty[i];
        s.cumqty[i] = running;
    }
}

void apply_update(Side& s, double price, double qty, bool ascending) noexcept {
    // find existing level. linear scan at depth=32 is ~5-8 cycles with SIMD
    // auto-vectorization. a hash map would be slower because cache miss.
    uint32_t idx = s.count;
    for (uint32_t i = 0; i < s.count; ++i) {
        if (s.px[i] == price) { idx = i; break; }
    }

    if (qty == 0.0) {
        if (idx == s.count) return;  // wasn't there, ignore
        for (uint32_t i = idx; i + 1 < s.count; ++i) {
            s.px[i]  = s.px[i + 1];
            s.qty[i] = s.qty[i + 1];
        }
        if (s.count > 0) --s.count;
    } else if (idx < s.count) {
        s.qty[idx] = qty;  // in-place update, price unchanged, no resort
    } else {
        // new level — insert and sort
        if (s.count >= s.depth) [[unlikely]] s.count = s.depth - 1;

        s.px[s.count]  = price;
        s.qty[s.count] = qty;
        idx = s.count++;

        // insertion sort. at depth=32 this is fine.
        // TODO: worth benchmarking if a branchless swap here helps at all.
        // my gut says no but I've been wrong about branchless before.
        while (idx > 0) {
            const bool out_of_order = ascending
                ? s.px[idx] < s.px[idx - 1]
                : s.px[idx] > s.px[idx - 1];
            if (!out_of_order) break;
            std::swap(s.px[idx],  s.px[idx - 1]);
            std::swap(s.qty[idx], s.qty[idx - 1]);
            --idx;
        }
    }

    recompute_cum(s);
}

// xorshift64. fast, not predictable by the branch predictor.
// we could use std::mt19937 but it's overkill and slower.
static uint64_t xor64(uint64_t& state) noexcept {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

// level selection weighted toward top of book.
// real market data on HL perps: L0 gets updated ~5-8x more than L5+.
// these weights are eyeballed from a week of tick data, not calculated.
static uint32_t pick_level(uint64_t& rng, uint32_t depth) noexcept {
    const uint64_t r = xor64(rng) & 0xFF;
    if (r < 100) return 0;
    if (r < 165) return 1;
    if (r < 210) return 2;
    if (r < 235) return 3;
    return static_cast<uint32_t>((xor64(rng) % (depth - 4)) + 4);
}

void run(const Config& cfg) {
    const uint32_t depth = std::min(cfg.book.depth, 32u);

    alignas(64) Side asks{};
    alignas(64) Side bids{};
    asks.depth = bids.depth = depth;

    for (uint32_t i = 0; i < depth; ++i) {
        asks.px[i]  = cfg.book.base_price + cfg.book.tick_size + i * cfg.book.tick_size;
        asks.qty[i] = cfg.book.lot_size * (1.0 + i * 0.05);
        bids.px[i]  = cfg.book.base_price - i * cfg.book.tick_size;
        bids.qty[i] = cfg.book.lot_size * (1.0 + i * 0.05);
    }
    asks.count = bids.count = depth;
    recompute_cum(asks);
    recompute_cum(bids);

    cache_warm_region(&asks, sizeof(asks));
    cache_warm_region(&bids, sizeof(bids));

    uint64_t rng = 0xdeadbeefcafe1234ULL;

    BenchConfig bc{};
    bc.warmup_iters    = cfg.run.warmup_iters;
    bc.measure_iters   = cfg.run.measure_iters;
    bc.pin_core        = cfg.run.pin_core;
    bc.spike_threshold = cfg.run.spike_threshold;

    const auto hist = run_bench([&]() noexcept {
        const uint32_t lvl  = pick_level(rng, depth);
        const bool is_ask   = (rng & 1);
        const double base   = is_ask
            ? cfg.book.base_price + cfg.book.tick_size + lvl * cfg.book.tick_size
            : cfg.book.base_price - lvl * cfg.book.tick_size;
        const double qty    = (rng & 0xF) == 0
            ? 0.0
            : cfg.book.lot_size * (1.0 + (rng & 7) * 0.05);

        if (is_ask) apply_update(asks, base, qty, true);
        else        apply_update(bids, base, qty, false);
    }, bc);

    const auto& tsc = global_tsc();
    const auto  rpt = LatencyReport::from_histogram(hist, tsc.ns_per_cycle, "orderbook_update");
    print_report(rpt);
    collect_report(rpt);
}

} // namespace orderbook
} // namespace bench

void bench_orderbook(const bench::Config& cfg) { bench::orderbook::run(cfg); }
