#pragma once

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <array>
#include <string_view>
#include <limits>

// HDR-ish latency histogram. power-of-2 bands, linear subs within each band.
// 64 bands x 64 subs = 4096 buckets, ~1.6% relative error. good enough.
//
// we don't use the real HdrHistogram because it's 4MB per instance and
// allocates on construction. this one is 32KB and lives on the stack.

namespace bench {

static constexpr uint32_t kHistBands   = 64;
static constexpr uint32_t kHistSubs    = 64;
static constexpr uint32_t kBuckets     = kHistBands * kHistSubs;  // 4096

struct Histogram {
    std::array<uint64_t, kBuckets> counts{};
    uint64_t total{0};
    uint64_t overflow{0};
    uint64_t min_val{std::numeric_limits<uint64_t>::max()};
    uint64_t max_val{0};
    uint64_t sum{0};

    void reset() noexcept {
        counts.fill(0);
        total = overflow = sum = 0;
        min_val = std::numeric_limits<uint64_t>::max();
        max_val = 0;
    }

    void record(uint64_t value) noexcept {
        if (value == 0) [[unlikely]] value = 1;

        const uint32_t band = (value < 2)
            ? 0u
            : static_cast<uint32_t>(63 - __builtin_clzll(value));

        if (band >= kHistBands) [[unlikely]] { ++overflow; ++total; return; }

        // band_start=1, band_width=1 for band 0 (value==1 only).
        // for band k>=1: start=2^k, width=2^k.
        const uint64_t band_start = (band == 0) ? 1ULL : (1ULL << band);
        const uint64_t band_width = (band == 0) ? 1ULL : (1ULL << band);
        const uint32_t sub = static_cast<uint32_t>(
            ((value - band_start) * kHistSubs) / band_width
        );

        ++counts[band * kHistSubs + std::min(sub, kHistSubs - 1u)];
        ++total;
        sum += value;
        if (value < min_val) min_val = value;
        if (value > max_val) max_val = value;
    }

    [[nodiscard]] uint64_t percentile(double q) const noexcept {
        if (total == 0) return 0;
        const uint64_t target = static_cast<uint64_t>(q * static_cast<double>(total));
        uint64_t running = 0;

        for (uint32_t i = 0; i < kBuckets; ++i) {
            if (counts[i] == 0) continue;
            running += counts[i];
            if (running > target) {
                const uint32_t b     = i / kHistSubs;
                const uint64_t bstart = (b == 0) ? 1ULL : (1ULL << b);
                const uint64_t bwidth = (b == 0) ? 1ULL : (1ULL << b);
                return bstart + (static_cast<uint64_t>(i % kHistSubs) * bwidth) / kHistSubs;
            }
        }
        return max_val;
    }

    [[nodiscard]] double mean() const noexcept {
        return total > 0 ? static_cast<double>(sum) / static_cast<double>(total) : 0.0;
    }

    // returns true if value is a likely outlier. used to log spikes without
    // stopping the benchmark. threshold_x=5 means "5x the current p99".
    // don't call this before you have at least 100 samples — p99 is garbage otherwise.
    [[nodiscard]] bool is_spike(uint64_t value, double threshold_x) const noexcept {
        if (total < 100) return false;
        return static_cast<double>(value) > threshold_x * static_cast<double>(percentile(0.99));
    }

    void merge(const Histogram& other) noexcept {
        for (uint32_t i = 0; i < kBuckets; ++i) counts[i] += other.counts[i];
        total    += other.total;
        overflow += other.overflow;
        sum      += other.sum;
        if (other.min_val < min_val) min_val = other.min_val;
        if (other.max_val > max_val) max_val = other.max_val;
    }
};

// flat report struct. built from a Histogram + TSC calibration after the run.
struct LatencyReport {
    std::string_view name;
    uint64_t samples{0};
    double p50_ns{0.0};
    double p95_ns{0.0};
    double p99_ns{0.0};
    double p999_ns{0.0};
    double p9999_ns{0.0};
    double mean_ns{0.0};
    double min_ns{0.0};
    double max_ns{0.0};
    uint64_t overflows{0};

    static LatencyReport from_histogram(
        const Histogram& h, double ns_per_cycle, std::string_view bench_name
    ) noexcept {
        LatencyReport r{};
        r.name     = bench_name;
        r.samples  = h.total;
        r.mean_ns  = h.mean() * ns_per_cycle;
        r.min_ns   = (h.min_val == std::numeric_limits<uint64_t>::max())
                       ? 0.0 : static_cast<double>(h.min_val) * ns_per_cycle;
        r.max_ns   = static_cast<double>(h.max_val) * ns_per_cycle;
        r.p50_ns   = static_cast<double>(h.percentile(0.50))   * ns_per_cycle;
        r.p95_ns   = static_cast<double>(h.percentile(0.95))   * ns_per_cycle;
        r.p99_ns   = static_cast<double>(h.percentile(0.99))   * ns_per_cycle;
        r.p999_ns  = static_cast<double>(h.percentile(0.999))  * ns_per_cycle;
        r.p9999_ns = static_cast<double>(h.percentile(0.9999)) * ns_per_cycle;
        r.overflows = h.overflow;
        return r;
    }
};

} // namespace bench
