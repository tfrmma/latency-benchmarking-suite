#include "../include/rdtsc.hpp"
#include "../include/histogram.hpp"
#include "../include/cpu_utils.hpp"
#include "../include/bench_runner.hpp"
#include "../include/bench_config.hpp"
#include "../include/report_collector.hpp"

#include <cstdint>
#include <cstdio>
#include <cmath>

namespace bench {
namespace cancel_replace {

struct alignas(64) ActiveQuote {
    double   bid_price{0.0};
    double   ask_price{0.0};
    double   bid_qty{0.0};
    double   ask_qty{0.0};
    uint64_t order_id_bid{0};
    uint64_t order_id_ask{0};
    bool     active{false};
};

struct alignas(64) Action {
    enum class Type : uint8_t { NONE, NEW_QUOTE, CANCEL_REPLACE };
    Type     type{Type::NONE};
    double   new_bid{0.0};
    double   new_ask{0.0};
    uint64_t cancel_bid_id{0};
    uint64_t cancel_ask_id{0};
    uint64_t new_bid_id{0};
    uint64_t new_ask_id{0};
};

static uint64_t g_next_id = 1;

// OFI widens the quote on the side that's getting hit.
// positive OFI = buy aggression = widen ask (you're getting lifted).
// negative OFI = sell aggression = widen bid (you're getting hit).
// the magnitude scaling by tick_size keeps the adjustment in price space.
static void compute_levels(double r, double ofi,
                           const PricingConfig& p, const BookConfig& b,
                           double& bid_out, double& ask_out) noexcept {
    const double half    = p.half_spread_ticks * b.tick_size;
    const double ofi_adj = p.ofi_alpha * std::abs(ofi) * b.tick_size;
    bid_out = r - half - (ofi > 0 ? 0.0 : ofi_adj);
    ask_out = r + half + (ofi > 0 ? ofi_adj : 0.0);
}

Action evaluate_reprice(const ActiveQuote& cur, double new_mid, double ofi,
                        const Config& cfg) noexcept {
    // recompute reserve price inline. same formula as tick_to_trade.
    const double r = new_mid
        - cfg.pricing.inventory * cfg.pricing.gamma
          * cfg.pricing.sigma * cfg.pricing.sigma * cfg.pricing.horizon_s
        + cfg.pricing.ofi_alpha * ofi;

    double new_bid, new_ask;
    compute_levels(r, ofi, cfg.pricing, cfg.book, new_bid, new_ask);

    if (!cur.active) {
        Action a{};
        a.type       = Action::Type::NEW_QUOTE;
        a.new_bid    = new_bid;
        a.new_ask    = new_ask;
        a.new_bid_id = g_next_id++;
        a.new_ask_id = g_next_id++;
        return a;
    }

    const double quote_mid = (cur.bid_price + cur.ask_price) * 0.5;
    const double drift     = std::abs(new_mid - quote_mid);
    const double threshold = cfg.pricing.reprice_threshold_ticks * cfg.book.tick_size;

    if (drift <= threshold) return {};  // stay put

    Action a{};
    a.type          = Action::Type::CANCEL_REPLACE;
    a.cancel_bid_id = cur.order_id_bid;
    a.cancel_ask_id = cur.order_id_ask;
    a.new_bid       = new_bid;
    a.new_ask       = new_ask;
    a.new_bid_id    = g_next_id++;
    a.new_ask_id    = g_next_id++;
    return a;
}

static void apply(ActiveQuote& q, const Action& a) noexcept {
    if (a.type == Action::Type::NONE) return;
    q.bid_price    = a.new_bid;
    q.ask_price    = a.new_ask;
    q.order_id_bid = a.new_bid_id;
    q.order_id_ask = a.new_ask_id;
    q.active       = true;
}

static uint64_t xor64(uint64_t& s) noexcept {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s;
}

void run(const Config& cfg) {
    alignas(64) ActiveQuote quote{};
    quote.bid_price    = cfg.book.base_price - cfg.pricing.half_spread_ticks * cfg.book.tick_size;
    quote.ask_price    = cfg.book.base_price + cfg.pricing.half_spread_ticks * cfg.book.tick_size;
    quote.bid_qty      = cfg.book.lot_size;
    quote.ask_qty      = cfg.book.lot_size;
    quote.order_id_bid = 100;
    quote.order_id_ask = 101;
    quote.active       = true;

    cache_warm_region(&quote, sizeof(quote));

    uint64_t rng = 0xfeedface12345678ULL;

    BenchConfig bc{};
    bc.warmup_iters    = cfg.run.warmup_iters;
    bc.measure_iters   = cfg.run.measure_iters;
    bc.pin_core        = cfg.run.pin_core;
    bc.spike_threshold = cfg.run.spike_threshold;

    const auto hist = run_bench([&]() noexcept {
        const uint64_t r     = xor64(rng);
        const double drift   = cfg.book.tick_size * (0.1 + (r & 0xF) * 0.07);
        const double new_mid = cfg.book.base_price + ((r & 1) ? drift : -drift);
        const double ofi     = (static_cast<double>(r & 0xFF) / 255.0) * 2.0 - 1.0;

        const auto action = evaluate_reprice(quote, new_mid, ofi, cfg);
        apply(quote, action);
    }, bc);

    const auto& tsc = global_tsc();
    const auto  rpt = LatencyReport::from_histogram(hist, tsc.ns_per_cycle, "cancel_replace");
    print_report(rpt);
    collect_report(rpt);
}

} // namespace cancel_replace
} // namespace bench

void bench_cancel_replace(const bench::Config& cfg) { bench::cancel_replace::run(cfg); }
