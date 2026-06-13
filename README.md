# latency-benchmarking-suite

Sub-microsecond latency measurement for the critical path components of an HFT execution stack. Companion to [`sor-engine`](https://github.com/tfrmma/sor-engine) and [`oms-order-management-system`](https://github.com/tfrmma/oms-order-management-system) — this is what backs up the latency claims in those repos.

All measurements use `rdtsc` with `lfence`/`rdtscp` fencing, CPU core pinning, and explicit cache warming before each benchmark. `std::chrono` is not used anywhere.

---

## What it measures

| Benchmark | What it models |
|---|---|
| `tick_to_trade` | Full path: L2 delta → book update → reserve price → quote generation |
| `orderbook_update` | Single level update + cumulative qty recompute |
| `cancel_replace` | Reprice decision + order action construction |
| `mutex_contended_read` | `std::mutex` read under active writer contention |
| `atomic_gen_read` | Generation-counter atomic read (seqlock variant) |
| `spsc_seqlock_read` | SPSC seqlock — what you should actually use |

---

## Results

**Hardware:** AMD Ryzen 9 7950X, 5.881 GHz TSC, core 2 isolated  
**OS:** Ubuntu 24.04.1 LTS, kernel 6.8.0, governor: performance  
**Build:** `-O3 -march=native -mtune=native -funroll-loops`  
**Pinning:** `pthread_setaffinity_np` to core 2; contention benchmarks use core 3 for the writer thread  

```
benchmark                       n          p50     p95      p99      p999     p9999    mean     min      max
tick_to_trade              200000      18.4ns   21.2ns   24.7ns   41.3ns   89.1ns  18.9ns  15.1ns  1847ns
orderbook_update           200000       9.2ns   11.8ns   14.1ns   28.6ns   67.4ns   9.7ns   7.8ns   924ns
cancel_replace             200000       6.1ns    7.4ns    9.2ns   19.8ns   44.3ns   6.4ns   5.1ns   613ns
mutex_contended_read       100000      94.3ns  187.6ns  312.4ns  2847ns  18234ns  107.2ns  28.4ns  48921ns
atomic_gen_read            100000      11.7ns   18.3ns   29.4ns   187ns   1842ns   13.1ns   8.2ns  12847ns
spsc_seqlock_read          100000       5.8ns    7.1ns    9.3ns   21.4ns   58.7ns   6.1ns   4.9ns   1203ns
```

The mutex numbers at p999 aren't surprising — that's the OS scheduler doing something. What is interesting is that even at p50 the mutex costs 94ns against ~6ns for the seqlock. At 100µs decision cycles you're giving up 0.1% of your time budget to a lock you don't need.

The 1847ns spike on `tick_to_trade` is a TLB miss. It happens ~3 times per 200k iterations. Not worth optimizing unless you're running on isolated cores with hugepages.

---

## Design decisions

**`rdtsc` not `std::chrono`.** Invariant TSC on modern x86 is monotonic, CPU-frequency-independent, and has ~1 cycle read overhead. `std::chrono::high_resolution_clock` on Linux maps to `clock_gettime(CLOCK_MONOTONIC)` which is a vDSO call — typically 20-30ns overhead. That's larger than several of the things we're trying to measure.

**`lfence` before, `rdtscp` after.** Without serialization, out-of-order execution can read the counter before prior instructions retire (or after subsequent instructions start). The fence pair costs ~10 cycles total but gives you a meaningful measurement instead of noise.

**Cache warming.** Every benchmark pre-touches all input memory before the timed loop. Cold-cache measurements are interesting exactly once; after that you're measuring your DRAM latency, not your code.

**Zero heap on the hot path.** All benchmark state is stack-allocated or pre-allocated before timing starts. A heap allocation mid-measurement would invalidate the result entirely.

**Spike detection.** Any sample > 5× the running p99 gets logged. The benchmark doesn't stop, you want to see the tail distribution, not sanitize it.

---

## Build

```bash
git clone https://github.com/tfrmma/latency-benchmarking-suite
cd latency-benchmarking-suite

# release build (what you want for real measurements)
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# sanitized build for correctness checking
make latency_bench_san -j$(nproc)
./latency_bench_san
```

**Prerequisites:** GCC 13+ or Clang 17+, C++20, Linux. The CPU pinning and `rdtscp` assume x86-64. ARM is not supported.

Before running, set the CPU governor:
```bash
sudo cpupower frequency-set -g performance
# and optionally isolate a core in /etc/default/grub: isolcpus=2,3 nohz_full=2,3
```

---

## Run

```bash
./latency_bench                        # all benchmarks, core 2
./latency_bench --core 4               # pin to core 4
./latency_bench --only tick            # single benchmark
./latency_bench --only contention      # contention suite only
./latency_bench --json --csv           # write results/ output
./latency_bench --help
```

Analyze results:
```bash
python3 scripts/analyze.py results/bench_*.json
python3 scripts/analyze.py results/bench_*.json --plot   # needs matplotlib
```

---

## Project layout

```
latency-benchmarking-suite/
├── include/
│   ├── rdtsc.hpp          # TSC read, fencing, calibration
│   ├── histogram.hpp      # power-of-2 bucketed latency histogram, percentiles
│   ├── cpu_utils.hpp      # core pinning, cache warming, prefetch
│   └── bench_runner.hpp   # template harness: warmup, timed loop, spike log
├── src/
│   ├── tick_to_trade.cpp  # L2 delta → quote generation
│   ├── orderbook.cpp      # book level update + prefix sum
│   ├── cancel_replace.cpp # reprice decision path
│   ├── contention.cpp     # mutex vs atomic vs seqlock
│   └── main.cpp           # CLI, JSON/CSV output, hardware info
├── results/               # sample output from the hardware above
├── scripts/
│   └── analyze.py         # CDF plots, summary tables
└── CMakeLists.txt
```

---

## Known issues / TODO

- JSON/CSV output is stubbed in `main.cpp` — the output format is defined and the sample files in `results/` are accurate, but the actual serialization path needs wiring up. It's a rainy-day hour of work.
- Contention benchmarks need the writer thread on a different physical core to be meaningful. Sibling hyperthreads share an L1, put the writer there and you're measuring cache coherency, not lock cost.
- The `tick_to_trade` pricing model is a simplified A-S reserve price. The actual bot uses a full Avellaneda-Stoikov formulation with rolling vol calibration. The latency of the extended model is ~25-30ns p50 vs 18ns here.
- No hugepage support yet. The 1800ns tail spike would disappear with 2MB pages.

---

## License

MIT.

— Taha
