#pragma once

#include "histogram.hpp"
#include <vector>

// global report collector. bench files push their LatencyReport here
// after printing so main.cpp can serialize to JSON/CSV.
// yes, it's a global. it's touched once per benchmark run, not on the hot path.

namespace bench {
    extern std::vector<LatencyReport> g_collected_reports;
    void collect_report(const LatencyReport& r);
}
