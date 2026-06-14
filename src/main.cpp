#include "../include/rdtsc.hpp"
#include "../include/histogram.hpp"
#include "../include/cpu_utils.hpp"
#include "../include/bench_config.hpp"
#include "../include/report_collector.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <string>
#include <vector>
#include <sys/stat.h>

// forward declarations
void bench_tick_to_trade(const bench::Config& cfg);
void bench_orderbook(const bench::Config& cfg);
void bench_cancel_replace(const bench::Config& cfg);
void bench_contention(const bench::Config& cfg);

// --------------------------------------------------------------------
// system info

static void get_cpu_model(char* buf, size_t len) noexcept {
    buf[0] = '\0';
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (!f) { strncpy(buf, "unknown", len); return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "model name", 10) != 0) continue;
        const char* colon = strchr(line, ':');
        if (!colon) continue;
        const char* start = colon + 2; // skip ": "
        strncpy(buf, start, len - 1);
        buf[len - 1] = '\0';
        size_t slen = strlen(buf);
        while (slen > 0 && (buf[slen-1] == '\n' || buf[slen-1] == '\r'))
            buf[--slen] = '\0';
        break;
    }
    fclose(f);
    if (buf[0] == '\0') strncpy(buf, "unknown", len);
}

static void get_os_info(char* buf, size_t len) noexcept {
    buf[0] = '\0';
    FILE* f = fopen("/etc/os-release", "r");
    if (!f) { strncpy(buf, "unknown", len); return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "PRETTY_NAME=", 12) != 0) continue;
        const char* start = line + 12;
        if (*start == '"') ++start;
        strncpy(buf, start, len - 1);
        buf[len - 1] = '\0';
        size_t slen = strlen(buf);
        while (slen > 0 && (buf[slen-1] == '"' || buf[slen-1] == '\n' || buf[slen-1] == '\r'))
            buf[--slen] = '\0';
        break;
    }
    fclose(f);
    if (buf[0] == '\0') strncpy(buf, "unknown", len);
}

static void get_kernel(char* buf, size_t len) noexcept {
    FILE* f = popen("uname -r", "r");
    if (!f) { strncpy(buf, "unknown", len); return; }
    if (!fgets(buf, static_cast<int>(len), f)) strncpy(buf, "unknown", len);
    fclose(f);
    size_t slen = strlen(buf);
    while (slen > 0 && (buf[slen-1] == '\n' || buf[slen-1] == '\r'))
        buf[--slen] = '\0';
}

static void get_timestamp(char* buf, size_t len) noexcept {
    time_t t = time(nullptr);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%S", localtime(&t));
}

// --------------------------------------------------------------------
// output

static void ensure_dir(const std::string& dir) noexcept {
    struct stat st{};
    if (stat(dir.c_str(), &st) != 0)
        mkdir(dir.c_str(), 0755);
}

static void write_json(
    const std::string& path,
    const char* cpu, const char* os, const char* kernel,
    const char* timestamp, const bench::TscCalibration& tsc,
    const bench::Config& cfg,
    const std::vector<bench::LatencyReport>& reports
) noexcept {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        fprintf(stderr, "[output] failed to open %s: %s\n", path.c_str(), strerror(errno));
        return;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"meta\": {\n");
    fprintf(f, "    \"timestamp\": \"%s\",\n", timestamp);
    fprintf(f, "    \"cpu\": \"%s\",\n", cpu);
    fprintf(f, "    \"os\": \"%s\",\n", os);
    fprintf(f, "    \"kernel\": \"%s\",\n", kernel);
    fprintf(f, "    \"tsc_hz\": %lu,\n", static_cast<unsigned long>(tsc.hz));
    fprintf(f, "    \"tsc_ghz\": %.4f,\n", static_cast<double>(tsc.hz) / 1e9);
    fprintf(f, "    \"tsc_calibration_method\": \"%s\",\n", tsc.method);
    fprintf(f, "    \"pin_core\": %d,\n", cfg.run.pin_core);
    fprintf(f, "    \"writer_core\": %d,\n", cfg.run.writer_core);
    fprintf(f, "    \"warmup_iters\": %u,\n", cfg.run.warmup_iters);
    fprintf(f, "    \"measure_iters\": %u,\n", cfg.run.measure_iters);
    fprintf(f, "    \"book_depth\": %u,\n", cfg.book.depth);
    fprintf(f, "    \"tick_size\": %.4f,\n", cfg.book.tick_size);
    fprintf(f, "    \"lot_size\": %.6f,\n", cfg.book.lot_size);
    fprintf(f, "    \"base_price\": %.2f,\n", cfg.book.base_price);
    fprintf(f, "    \"ofi_alpha\": %.6f\n", cfg.pricing.ofi_alpha);
    fprintf(f, "  },\n");
    fprintf(f, "  \"benchmarks\": [\n");

    for (size_t i = 0; i < reports.size(); ++i) {
        const auto& r = reports[i];
        fprintf(f, "    {\n");
        fprintf(f, "      \"name\": \"%s\",\n", r.name.data());
        fprintf(f, "      \"samples\": %lu,\n", static_cast<unsigned long>(r.samples));
        fprintf(f, "      \"p50_ns\": %.2f,\n",   r.p50_ns);
        fprintf(f, "      \"p95_ns\": %.2f,\n",   r.p95_ns);
        fprintf(f, "      \"p99_ns\": %.2f,\n",   r.p99_ns);
        fprintf(f, "      \"p999_ns\": %.2f,\n",  r.p999_ns);
        fprintf(f, "      \"p9999_ns\": %.2f,\n", r.p9999_ns);
        fprintf(f, "      \"mean_ns\": %.2f,\n",  r.mean_ns);
        fprintf(f, "      \"min_ns\": %.2f,\n",   r.min_ns);
        fprintf(f, "      \"max_ns\": %.2f,\n",   r.max_ns);
        fprintf(f, "      \"overflows\": %lu\n",  static_cast<unsigned long>(r.overflows));
        fprintf(f, "    }%s\n", (i + 1 < reports.size()) ? "," : "");
    }

    fprintf(f, "  ]\n}\n");
    fclose(f);
    printf("[output] json -> %s\n", path.c_str());
}

static void write_csv(
    const std::string& path,
    const char* cpu, const char* timestamp,
    const bench::Config& cfg,
    const std::vector<bench::LatencyReport>& reports
) noexcept {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        fprintf(stderr, "[output] failed to open %s: %s\n", path.c_str(), strerror(errno));
        return;
    }

    fprintf(f, "timestamp,cpu,pin_core,depth,tick_size,ofi_alpha,"
               "benchmark,samples,p50_ns,p95_ns,p99_ns,p999_ns,p9999_ns,"
               "mean_ns,min_ns,max_ns,overflows\n");

    for (const auto& r : reports) {
        fprintf(f, "%s,%s,%d,%u,%.4f,%.6f,%s,%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%lu\n",
                timestamp, cpu, cfg.run.pin_core, cfg.book.depth,
                cfg.book.tick_size, cfg.pricing.ofi_alpha,
                r.name.data(),
                static_cast<unsigned long>(r.samples),
                r.p50_ns, r.p95_ns, r.p99_ns, r.p999_ns, r.p9999_ns,
                r.mean_ns, r.min_ns, r.max_ns,
                static_cast<unsigned long>(r.overflows));
    }

    fclose(f);
    printf("[output] csv  -> %s\n", path.c_str());
}

// --------------------------------------------------------------------
// CLI

static void print_usage(const char* prog) noexcept {
    printf(
        "Usage: %s [options]\n\n"
        "Benchmark selection:\n"
        "  --only <name>         run only: tick | book | cancel | contention\n"
        "                        (repeatable: --only tick --only book)\n\n"
        "CPU / scheduling:\n"
        "  --core N              reader core (default: 2)\n"
        "  --writer-core N       writer core for contention benchmarks (default: 3)\n"
        "  --rt                  set SCHED_FIFO priority 80 (needs CAP_SYS_NICE)\n\n"
        "Iteration counts:\n"
        "  --warmup N            warmup iterations (default: 10000)\n"
        "  --iters N             measurement iterations (default: 100000)\n"
        "  --spike-threshold X   flag spike if sample > X * p99 (default: 5.0)\n\n"
        "Book / pricing parameters:\n"
        "  --depth N             book depth per side (default: 20)\n"
        "  --tick-size F         tick size in USD (default: 0.5)\n"
        "  --lot-size F          lot size in BTC (default: 0.001)\n"
        "  --base-price F        mid price seed (default: 65000.0)\n"
        "  --spread-ticks F      initial half-spread in ticks (default: 1.0)\n"
        "  --gamma F             A-S risk aversion (default: 0.1)\n"
        "  --sigma F             realized vol/s (default: 0.002)\n"
        "  --horizon F           quoting horizon in seconds (default: 1.0)\n"
        "  --ofi-alpha F         OFI coefficient in reserve price (default: 0.0002)\n"
        "  --inventory F         initial inventory in lots (default: 0.15)\n"
        "  --reprice-ticks F     reprice threshold in ticks (default: 0.5)\n\n"
        "Output:\n"
        "  --json                write JSON to out-dir\n"
        "  --csv                 write CSV to out-dir\n"
        "  --out-dir <path>      output directory (default: results)\n\n"
        "  --help                this\n",
        prog
    );
}

static bench::Config parse_args(int argc, char** argv) {
    bench::Config cfg{};
    bool only_set = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];

        auto next_str = [&]() -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "missing argument for %s\n", a); exit(1); }
            return argv[++i];
        };
        auto next_double = [&]() -> double { return atof(next_str()); };
        auto next_int    = [&]() -> int    { return atoi(next_str()); };
        auto next_uint   = [&]() -> uint32_t { return static_cast<uint32_t>(atoi(next_str())); };

        if (!strcmp(a, "--help"))          { print_usage(argv[0]); exit(0); }
        else if (!strcmp(a, "--json"))     { cfg.output.json = true; }
        else if (!strcmp(a, "--csv"))      { cfg.output.csv  = true; }
        else if (!strcmp(a, "--rt"))       { cfg.run.set_rt_priority = true; }
        else if (!strcmp(a, "--core"))          { cfg.run.pin_core      = next_int(); }
        else if (!strcmp(a, "--writer-core"))   { cfg.run.writer_core   = next_int(); }
        else if (!strcmp(a, "--warmup"))        { cfg.run.warmup_iters  = next_uint(); }
        else if (!strcmp(a, "--iters"))         { cfg.run.measure_iters = next_uint(); }
        else if (!strcmp(a, "--spike-threshold")){ cfg.run.spike_threshold = next_double(); }
        else if (!strcmp(a, "--out-dir"))       { cfg.output.out_dir    = next_str(); }
        else if (!strcmp(a, "--depth"))         { cfg.book.depth        = next_uint(); }
        else if (!strcmp(a, "--tick-size"))     { cfg.book.tick_size    = next_double(); }
        else if (!strcmp(a, "--lot-size"))      { cfg.book.lot_size     = next_double(); }
        else if (!strcmp(a, "--base-price"))    { cfg.book.base_price   = next_double(); }
        else if (!strcmp(a, "--spread-ticks"))  { cfg.book.spread_ticks = next_double(); }
        else if (!strcmp(a, "--gamma"))         { cfg.pricing.gamma     = next_double(); }
        else if (!strcmp(a, "--sigma"))         { cfg.pricing.sigma     = next_double(); }
        else if (!strcmp(a, "--horizon"))       { cfg.pricing.horizon_s = next_double(); }
        else if (!strcmp(a, "--ofi-alpha"))     { cfg.pricing.ofi_alpha = next_double(); }
        else if (!strcmp(a, "--inventory"))     { cfg.pricing.inventory = next_double(); }
        else if (!strcmp(a, "--reprice-ticks")) { cfg.pricing.reprice_threshold_ticks = next_double(); }
        else if (!strcmp(a, "--only")) {
            if (!only_set) {
                cfg.run_tick = cfg.run_book = cfg.run_cancel = cfg.run_contention = false;
                only_set = true;
            }
            const char* which = next_str();
            if      (!strcmp(which, "tick"))       cfg.run_tick       = true;
            else if (!strcmp(which, "book"))       cfg.run_book       = true;
            else if (!strcmp(which, "cancel"))     cfg.run_cancel     = true;
            else if (!strcmp(which, "contention")) cfg.run_contention = true;
            else { fprintf(stderr, "unknown benchmark: %s\n", which); exit(1); }
        }
        else { fprintf(stderr, "unknown option: %s\n", a); exit(1); }
    }
    return cfg;
}

// --------------------------------------------------------------------
// report collection -- defined here, declared extern in report_collector.hpp

namespace bench {
    std::vector<LatencyReport> g_collected_reports;

    void collect_report(const LatencyReport& r) {
        g_collected_reports.push_back(r);
    }
}

int main(int argc, char** argv) {
    const bench::Config cfg = parse_args(argc, argv);

    if (!bench::has_invariant_tsc()) {
        fprintf(stderr,
            "[warn] invariant TSC not reported (CPUID 0x80000007 bit 8).\n"
            "       rdtsc measurements may drift under frequency scaling.\n"
        );
    }

    const auto& tsc = bench::global_tsc();

    char cpu[128], os[128], kernel[64], timestamp[32];
    get_cpu_model(cpu, sizeof(cpu));
    get_os_info(os, sizeof(os));
    get_kernel(kernel, sizeof(kernel));
    get_timestamp(timestamp, sizeof(timestamp));

    printf("latency-benchmarking-suite\n");
    printf("  cpu        : %s\n", cpu);
    printf("  os         : %s\n", os);
    printf("  kernel     : %s\n", kernel);
    printf("  tsc        : %.4f GHz (%s)\n",
           static_cast<double>(tsc.hz) / 1e9, tsc.method);
    printf("  core       : %d  writer: %d\n", cfg.run.pin_core, cfg.run.writer_core);
    printf("  iters      : warmup=%u  measure=%u\n",
           cfg.run.warmup_iters, cfg.run.measure_iters);
    printf("  book       : depth=%u  tick=%.2f  lot=%.4f  base=%.2f\n",
           cfg.book.depth, cfg.book.tick_size, cfg.book.lot_size, cfg.book.base_price);
    printf("  pricing    : gamma=%.3f  sigma=%.4f  ofi_alpha=%.6f  inv=%.3f\n",
           cfg.pricing.gamma, cfg.pricing.sigma,
           cfg.pricing.ofi_alpha, cfg.pricing.inventory);
    printf("  time       : %s\n\n", timestamp);

    printf("%-30s  %s\n", "benchmark",
           "n          p50      p95      p99      p999     p9999    mean     min      max");
    printf("%s\n", std::string(120, '-').c_str());

    if (cfg.run_tick)       bench_tick_to_trade(cfg);
    if (cfg.run_book)       bench_orderbook(cfg);
    if (cfg.run_cancel)     bench_cancel_replace(cfg);
    if (cfg.run_contention) bench_contention(cfg);

    // write output files if requested
    const auto& reports = bench::g_collected_reports;

    if (!reports.empty() && (cfg.output.json || cfg.output.csv)) {
        ensure_dir(cfg.output.out_dir);

        // build a filename from timestamp so runs don't clobber each other
        char ts_safe[32];
        strncpy(ts_safe, timestamp, sizeof(ts_safe));
        for (char* p = ts_safe; *p; ++p)
            if (*p == ':') *p = '-';

        if (cfg.output.json) {
            const std::string path = cfg.output.out_dir + "/bench_" + ts_safe + ".json";
            write_json(path, cpu, os, kernel, timestamp, tsc, cfg, reports);
        }
        if (cfg.output.csv) {
            const std::string path = cfg.output.out_dir + "/bench_" + ts_safe + ".csv";
            write_csv(path, cpu, timestamp, cfg, reports);
        }
    } else if (cfg.output.json || cfg.output.csv) {
        printf("[output] no reports collected — did the benchmarks run?\n");
    }

    printf("\ndone.\n");
    return 0;
}
