#include "../include/rdtsc.hpp"
#include "../include/histogram.hpp"
#include "../include/cpu_utils.hpp"
#include "../include/bench_runner.hpp"
#include "../include/bench_config.hpp"
#include "../include/report_collector.hpp"

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <array>

// tick-to-trade: L2 delta -> book update -> reserve price -> quote generation.
//
// the hot path is: receive update, recompute mid, compute reserve price with
// inventory skew + OFI adjustment, emit bid/ask. that's it.
// we're not doing a full book reconstruction here — just top-of-book repricing
// which is what actually matters for latency.

namespace bench {
namespace tick_to_trade {

struct L2Delta {
    double   price;
    double   qty;
    double   ofi;      // [-1, 1]. positive = buy pressure at this level.
    uint8_t  side;     // 0=bid 1=ask
    uint8_t  level;
    uint64_t seq_id;
};

struct BookLevel { double price{0.0}; double qty{0.0}; };

struct alignas(64) LocalBook {
    BookLevel bids[32]{};
    BookLevel asks[32]{};
    uint32_t  depth{0};
    double    mid{0.0};
    uint64_t  last_seq{0};
};

struct alignas(64) QuoteOrder {
    double price;
    double qty;
    int8_t side;   // -1=bid 1=ask
    bool   active;
};

// 4 quotes max — 2 levels each side. more than enough for MM on perps.
struct alignas(64) OutBuffer {
    QuoteOrder orders[4]{};
    uint32_t   count{0};
};

// A-S reserve price: r = mid - q*gamma*sigma^2*T + alpha*ofi
// OFI term shifts r toward the side with more flow. if buys dominate,
// we shade up — meaning we're willing to ask higher and buy higher too.
// the alpha value needs empirical calibration; 0.0002 is a reasonable starting
// point for BTC perps at 1s horizon but don't trust it blindly.
inline double reserve_price(double mid, double inv, double ofi, const PricingConfig& p) noexcept {
    return mid
        - inv * p.gamma * p.sigma * p.sigma * p.horizon_s
        + p.ofi_alpha * ofi;
}

inline void fill_quotes(double r, const PricingConfig& p, const BookConfig& b,
                        QuoteOrder* orders, uint32_t& count) noexcept {
    const double half = p.half_spread_ticks * b.tick_size;
    count = 0;
    orders[count++] = { r - half, b.lot_size, -1, true };
    orders[count++] = { r + half, b.lot_size,  1, true };
}

inline void tick_to_order(LocalBook& book, const L2Delta& delta,
                          OutBuffer& out, const Config& cfg) noexcept {
    auto& side = (delta.side == 0) ? book.bids : book.asks;
    if (delta.level < book.depth) {
        side[delta.level].price = delta.price;
        side[delta.level].qty   = delta.qty;
    }
    book.last_seq = delta.seq_id;
    book.mid = (book.bids[0].price + book.asks[0].price) * 0.5;

    const double r = reserve_price(book.mid, cfg.pricing.inventory, delta.ofi, cfg.pricing);
    fill_quotes(r, cfg.pricing, cfg.book, out.orders, out.count);
}

// build a sequence of deltas that hits different levels with OFI varying sign.
// the pattern repeats every 32 iterations which is fine — the point is that
// it's not a single repeated value that the branch predictor can trivialize.
static void build_deltas(std::array<L2Delta, 32>& deltas, const BookConfig& b, uint32_t n) noexcept {
    const double ofi_pattern[8] = { 0.3, -0.1, 0.6, 0.2, -0.4, 0.5, -0.2, 0.1 };
    for (uint32_t i = 0; i < n; ++i) {
        deltas[i] = {
            b.base_price - (i % b.depth) * b.tick_size * (i % 2 == 0 ? 1.0 : -1.0),
            b.lot_size * (1.0 + (i % 5) * 0.1),
            ofi_pattern[i % 8],
            static_cast<uint8_t>(i % 2),
            static_cast<uint8_t>(i % b.depth),
            static_cast<uint64_t>(i)
        };
    }
}

void run(const Config& cfg) {
    alignas(64) LocalBook book{};
    alignas(64) OutBuffer out{};

    book.depth = std::min(cfg.book.depth, 32u);
    for (uint32_t i = 0; i < book.depth; ++i) {
        book.bids[i] = { cfg.book.base_price - i * cfg.book.tick_size, cfg.book.lot_size };
        book.asks[i] = { cfg.book.base_price + cfg.book.tick_size + i * cfg.book.tick_size, cfg.book.lot_size };
    }
    book.mid = (book.bids[0].price + book.asks[0].price) * 0.5;

    std::array<L2Delta, 32> deltas{};
    const uint32_t n_deltas = std::min(32u, cfg.book.depth);
    build_deltas(deltas, cfg.book, n_deltas);

    cache_warm_region(&book, sizeof(book));
    cache_warm_region(&out,  sizeof(out));
    cache_warm_region(deltas.data(), n_deltas * sizeof(L2Delta));

    uint32_t delta_idx = 0;

    BenchConfig bc{};
    bc.warmup_iters    = cfg.run.warmup_iters;
    bc.measure_iters   = cfg.run.measure_iters;
    bc.pin_core        = cfg.run.pin_core;
    bc.spike_threshold = cfg.run.spike_threshold;

    uint64_t spikes[64]{};
    uint32_t n_spikes = 0;

    const auto hist = run_bench([&]() noexcept {
        tick_to_order(book, deltas[delta_idx % n_deltas], out, cfg);
        ++delta_idx;
    }, bc, spikes, &n_spikes);

    const auto& tsc = global_tsc();
    const auto  rpt = LatencyReport::from_histogram(hist, tsc.ns_per_cycle, "tick_to_trade");
    print_report(rpt);
    collect_report(rpt);

    if (n_spikes > 0)
        std::printf("  spikes: %u  first=%.1fns\n", n_spikes, tsc.to_ns(spikes[0]));
}

} // namespace tick_to_trade
} // namespace bench

void bench_tick_to_trade(const bench::Config& cfg) { bench::tick_to_trade::run(cfg); }
