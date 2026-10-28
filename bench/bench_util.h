// ==============================================================================
// bench/bench_util.h - Shared workload machinery for db_bench and sqlite_bench
// ==============================================================================
//
// Both benchmark binaries must generate IDENTICAL workloads so the numbers in
// docs/BENCHMARKS.md compare like with like:
//   - keys:    16-byte zero-padded decimal ("0000000000000042") so
//              lexicographic order == numeric order in both engines
//   - values:  kValueSize bytes, deterministically derived from the key index
//   - randomness: std::mt19937_64 seeded from KVSTORE_BENCH_SEED (default 42)
//   - skew:    YCSB-style scrambled Zipfian (theta 0.99)
//
// Percentiles are hand-collected (plan.md 6.1): every operation inside the
// timed region is clocked with steady_clock and the sample vector is reduced
// to p50/p99 after the run. On this machine the clock pair costs ~40 ns,
// which is noise against microsecond-scale storage ops; the overhead is byte
// identical in both binaries, so comparisons are unaffected.
//
// ==============================================================================

#pragma once

#include <benchmark/benchmark.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace kvbench {

inline constexpr size_t kKeySize = 16;
inline constexpr size_t kValueSize = 100;

// ------------------------------------------------------------------------
// Deterministic key/value generation
// ------------------------------------------------------------------------

inline std::string MakeKey(uint64_t index) {
    char buf[kKeySize + 1];
    std::snprintf(buf, sizeof(buf), "%016llu",
                  static_cast<unsigned long long>(index));
    return std::string(buf, kKeySize);
}

// Value bytes are a function of (index, generation) so updates in the mixed
// and churn workloads write different bytes than the initial load.
inline std::string MakeValue(uint64_t index, uint64_t generation = 0) {
    std::string value(kValueSize, '\0');
    uint64_t x = index * 0x9E3779B97F4A7C15ULL + generation * 0xC2B2AE3D27D4EB4FULL + 1;
    for (size_t i = 0; i < kValueSize; ++i) {
        x ^= x >> 33;
        x *= 0xFF51AFD7ED558CCDULL;
        value[i] = static_cast<char>('a' + (x % 26));
    }
    return value;
}

inline uint64_t BenchSeed() {
    if (const char* env = std::getenv("KVSTORE_BENCH_SEED")) {
        return std::strtoull(env, nullptr, 10);
    }
    return 42;
}

// ------------------------------------------------------------------------
// Scrambled Zipfian (Gray et al. / YCSB): rank 0 is the hottest item; ranks
// are scattered over the keyspace with a Fibonacci hash so hot keys do not
// cluster in one B+ tree leaf.
// ------------------------------------------------------------------------

class ZipfianGenerator {
public:
    explicit ZipfianGenerator(uint64_t n, double theta = 0.99)
        : n_(n), theta_(theta) {
        zeta_n_ = Zeta(n, theta);
        const double zeta2 = Zeta(2, theta);
        alpha_ = 1.0 / (1.0 - theta);
        eta_ = (1.0 - std::pow(2.0 / static_cast<double>(n), 1.0 - theta)) /
               (1.0 - zeta2 / zeta_n_);
    }

    // Zipf-distributed rank in [0, n): rank 0 most popular.
    uint64_t NextRank(std::mt19937_64& rng) {
        const double u =
            std::uniform_real_distribution<double>(0.0, 1.0)(rng);
        const double uz = u * zeta_n_;
        if (uz < 1.0) {
            return 0;
        }
        if (uz < 1.0 + std::pow(0.5, theta_)) {
            return 1;
        }
        return static_cast<uint64_t>(
            static_cast<double>(n_) *
            std::pow(eta_ * u - eta_ + 1.0, alpha_));
    }

    // Scrambled: popularity ranks scattered across [0, n).
    uint64_t NextIndex(std::mt19937_64& rng) {
        const uint64_t rank = NextRank(rng);
        return (rank * 0x9E3779B97F4A7C15ULL) % n_;
    }

private:
    static double Zeta(uint64_t n, double theta) {
        double sum = 0.0;
        for (uint64_t i = 1; i <= n; ++i) {
            sum += 1.0 / std::pow(static_cast<double>(i), theta);
        }
        return sum;
    }

    uint64_t n_;
    double theta_;
    double zeta_n_;
    double alpha_;
    double eta_;
};

// ------------------------------------------------------------------------
// Hand-collected latency percentiles (plan.md 6.1)
// ------------------------------------------------------------------------

class LatencyRecorder {
public:
    void Reserve(size_t n) { samples_ns_.reserve(n); }

    struct Timer {
        explicit Timer(LatencyRecorder& rec)
            : rec_(rec), start_(std::chrono::steady_clock::now()) {}
        ~Timer() {
            const auto end = std::chrono::steady_clock::now();
            rec_.samples_ns_.push_back(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end - start_).count()));
        }
        Timer(const Timer&) = delete;
        Timer& operator=(const Timer&) = delete;

    private:
        LatencyRecorder& rec_;
        std::chrono::steady_clock::time_point start_;
    };

    // Attach p50/p99/max counters (microseconds) to the benchmark output.
    void Report(benchmark::State& state) {
        if (samples_ns_.empty()) {
            return;
        }
        std::sort(samples_ns_.begin(), samples_ns_.end());
        state.counters["p50_us"] = PercentileUs(50.0);
        state.counters["p99_us"] = PercentileUs(99.0);
        state.counters["max_us"] = static_cast<double>(samples_ns_.back()) / 1000.0;
    }

private:
    double PercentileUs(double pct) const {
        const size_t idx = std::min(
            samples_ns_.size() - 1,
            static_cast<size_t>(pct / 100.0 *
                                static_cast<double>(samples_ns_.size())));
        return static_cast<double>(samples_ns_[idx]) / 1000.0;
    }

    std::vector<uint64_t> samples_ns_;
};

// ------------------------------------------------------------------------
// Scratch directories
// ------------------------------------------------------------------------

class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        static std::atomic<uint64_t> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("kvstore_bench_" + tag + "_" +
                 std::to_string(::getpid()) + "_" +
                 std::to_string(counter.fetch_add(1)));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace kvbench
