#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

// all benchmark parameters in one place. nothing hardcoded in the bench files.
// populated by main.cpp from CLI args, then passed down.

namespace bench {

struct BookConfig {
    uint32_t depth{20};          // levels per side
    double   tick_size{0.5};     // price tick in USD
    double   lot_size{0.001};    // qty lot in BTC
    double   base_price{65000.0};
    double   spread_ticks{1.0};  // initial bid-ask spread in ticks
};

struct PricingConfig {
    double gamma{0.1};           // A-S risk aversion
    double sigma{0.002};         // realized vol per second (~0.2% for BTC)
    double horizon_s{1.0};       // quoting horizon in seconds
    double half_spread_ticks{1.0};
    double reprice_threshold_ticks{0.5};
    double ofi_alpha{0.0002};    // OFI coefficient in reserve price
    double inventory{0.15};      // initial inventory in lots (signed)
};

struct RunConfig {
    uint32_t warmup_iters{10'000};
    uint32_t measure_iters{100'000};
    int      pin_core{2};
    int      writer_core{3};     // for contention benchmarks
    bool     set_rt_priority{false};
    double   spike_threshold{5.0};
};

struct OutputConfig {
    bool        json{false};
    bool        csv{false};
    std::string out_dir{"results"};
};

// everything in one place so main.cpp can build it from CLI and pass it around.
struct Config {
    RunConfig     run{};
    BookConfig    book{};
    PricingConfig pricing{};
    OutputConfig  output{};

    bool run_tick{true};
    bool run_book{true};
    bool run_cancel{true};
    bool run_contention{true};
};

} // namespace bench
