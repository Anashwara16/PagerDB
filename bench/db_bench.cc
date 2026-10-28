// ==============================================================================
// bench/db_bench.cc - PagerDB benchmark suite (Phase 6, task 6.1)
// ==============================================================================
//
// Workloads (plan.md 6.1): sequential load, random load, uniform and Zipfian
// read-only, 95/5 mixed, scan-heavy, delete churn. Reported as ops/sec
// (items_per_second) plus hand-collected p50/p99 latency counters.
//
// Conventions (shared with sqlite_bench.cc via bench_util.h):
//   - 100,000 keys, 16-byte keys, 100-byte values, seed 42
//     (KVSTORE_BENCH_SEED overrides)
//   - sync_on_write=false everywhere except the *Sync variants: throughput
//     numbers measure the engine, not the disk's fsync latency. The Sync
//     variants exist precisely to show the fsync-bound number.
//   - read benchmarks run at two pool sizes: 64 frames (256 KB - constant
//     eviction, the OS page cache absorbs the misses) and 8192 frames
//     (32 MB - the whole tree stays resident).
//
// Run everything:  tools/run_bench.sh   (one command, saves results)
// Run one:         build/bench/db_bench --benchmark_filter=ReadUniform
//
// ==============================================================================

#include <benchmark/benchmark.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>

#include "bench_util.h"
#include "kvstore/db.h"

namespace {

using namespace kvbench;

constexpr uint64_t kNumKeys = 100'000;
constexpr int kBatchOps = 1'000;  // Point-op batch per benchmark iteration

std::unique_ptr<kvstore::DB> OpenDb(const std::filesystem::path& dir,
                                    bool sync_on_write, size_t pool_frames) {
    kvstore::Options options;
    options.create_if_missing = true;
    options.sync_on_write = sync_on_write;
    options.buffer_pool_frames = pool_frames;
    auto db = kvstore::DB::Open(dir.string(), options);
    if (!db.has_value()) {
        std::fprintf(stderr, "DB::Open(%s) failed: %s\n", dir.c_str(),
                     std::string(db.error().message()).c_str());
        std::abort();
    }
    return std::move(db.value());
}

void LoadKeys(kvstore::DB& db, uint64_t n, bool sequential, uint64_t seed) {
    std::vector<uint64_t> order(n);
    for (uint64_t i = 0; i < n; ++i) {
        order[i] = i;
    }
    if (!sequential) {
        std::mt19937_64 rng(seed);
        std::shuffle(order.begin(), order.end(), rng);
    }
    for (uint64_t index : order) {
        auto status = db.Put(MakeKey(index), MakeValue(index));
        if (!status.ok()) {
            std::fprintf(stderr, "preload Put failed: %s\n",
                         std::string(status.message()).c_str());
            std::abort();
        }
    }
}

// A database preloaded with kNumKeys entries, built once per process and
// copied for benchmarks that mutate it. Read-only benchmarks share the
// pristine copy.
class Preloaded {
public:
    static const std::filesystem::path& Dir() {
        static Preloaded instance;
        return instance.dir_->path();
    }

    // Copy the pristine database into `dst` (data.db + wal.log).
    static void CopyTo(const std::filesystem::path& dst) {
        std::filesystem::copy(Dir(), dst,
                              std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    }

private:
    Preloaded() : dir_(std::make_unique<TempDir>("preload")) {
        auto db = OpenDb(dir_->path(), /*sync=*/false, /*frames=*/8192);
        LoadKeys(*db, kNumKeys, /*sequential=*/true, BenchSeed());
        // Clean close checkpoints, so copies start with an empty WAL.
    }

    std::unique_ptr<TempDir> dir_;
};

// ------------------------------------------------------------------------
// Load benchmarks: fresh database per iteration, N puts timed.
// ------------------------------------------------------------------------

void RunLoad(benchmark::State& state, bool sequential, bool sync_on_write) {
    const auto n = static_cast<uint64_t>(state.range(0));
    LatencyRecorder lat;
    lat.Reserve(n * 4);
    uint64_t total_ops = 0;

    for (auto _ : state) {
        state.PauseTiming();
        TempDir dir(sequential ? "seqload" : "randload");
        std::vector<uint64_t> order(n);
        for (uint64_t i = 0; i < n; ++i) {
            order[i] = i;
        }
        if (!sequential) {
            std::mt19937_64 rng(BenchSeed());
            std::shuffle(order.begin(), order.end(), rng);
        }
        auto db = OpenDb(dir.path(), sync_on_write, 8192);
        state.ResumeTiming();

        for (uint64_t index : order) {
            const std::string key = MakeKey(index);
            const std::string value = MakeValue(index);
            LatencyRecorder::Timer timer(lat);
            auto status = db->Put(key, value);
            if (!status.ok()) {
                state.SkipWithError(std::string(status.message()).c_str());
                return;
            }
        }
        total_ops += n;

        state.PauseTiming();
        db.reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<int64_t>(total_ops));
    lat.Report(state);
}

void BM_SeqLoad(benchmark::State& state) { RunLoad(state, true, false); }
void BM_RandLoad(benchmark::State& state) { RunLoad(state, false, false); }
// fsync-bound durability number - each Put flushes the WAL to disk.
void BM_SeqLoadSync(benchmark::State& state) { RunLoad(state, true, true); }

BENCHMARK(BM_SeqLoad)->Arg(kNumKeys)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_RandLoad)->Arg(kNumKeys)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_SeqLoadSync)->Arg(2'000)->Unit(benchmark::kMillisecond);

// ------------------------------------------------------------------------
// Read-only benchmarks over the preloaded database.
// state.range(0) = buffer pool frames.
// ------------------------------------------------------------------------

void RunReadOnly(benchmark::State& state, bool zipfian) {
    const auto pool_frames = static_cast<size_t>(state.range(0));
    auto db = OpenDb(Preloaded::Dir(), /*sync=*/false, pool_frames);
    std::mt19937_64 rng(BenchSeed());
    std::uniform_int_distribution<uint64_t> uniform(0, kNumKeys - 1);
    ZipfianGenerator zipf(kNumKeys);

    LatencyRecorder lat;
    lat.Reserve(1 << 22);
    uint64_t ops = 0;
    for (auto _ : state) {
        for (int i = 0; i < kBatchOps; ++i) {
            const uint64_t index =
                zipfian ? zipf.NextIndex(rng) : uniform(rng);
            const std::string key = MakeKey(index);
            LatencyRecorder::Timer timer(lat);
            auto value = db->Get(key);
            if (!value.has_value()) {
                state.SkipWithError("Get failed");
                return;
            }
            benchmark::DoNotOptimize(value.value());
        }
        ops += kBatchOps;
    }
    state.SetItemsProcessed(static_cast<int64_t>(ops));
    const auto stats = db->GetStats();
    state.counters["pool_hit_pct"] =
        100.0 * static_cast<double>(stats.pool_hits) /
        static_cast<double>(stats.pool_hits + stats.pool_misses + 1);
    lat.Report(state);
}

void BM_ReadUniform(benchmark::State& state) { RunReadOnly(state, false); }
void BM_ReadZipfian(benchmark::State& state) { RunReadOnly(state, true); }

BENCHMARK(BM_ReadUniform)->Arg(64)->Arg(8192);
BENCHMARK(BM_ReadZipfian)->Arg(64)->Arg(8192);

// ------------------------------------------------------------------------
// 95% Get / 5% Put over a private copy of the preloaded database.
// ------------------------------------------------------------------------

void BM_Mixed95_5(benchmark::State& state) {
    TempDir dir("mixed");
    Preloaded::CopyTo(dir.path());
    auto db = OpenDb(dir.path(), /*sync=*/false,
                     static_cast<size_t>(state.range(0)));
    std::mt19937_64 rng(BenchSeed());
    std::uniform_int_distribution<uint64_t> pick(0, kNumKeys - 1);
    std::uniform_int_distribution<int> percent(0, 99);

    LatencyRecorder lat;
    lat.Reserve(1 << 22);
    uint64_t ops = 0;
    uint64_t generation = 1;
    for (auto _ : state) {
        for (int i = 0; i < kBatchOps; ++i) {
            const uint64_t index = pick(rng);
            const std::string key = MakeKey(index);
            if (percent(rng) < 5) {
                const std::string value = MakeValue(index, generation++);
                LatencyRecorder::Timer timer(lat);
                auto status = db->Put(key, value);
                if (!status.ok()) {
                    state.SkipWithError(std::string(status.message()).c_str());
                    return;
                }
            } else {
                LatencyRecorder::Timer timer(lat);
                auto value = db->Get(key);
                if (!value.has_value()) {
                    state.SkipWithError("Get failed");
                    return;
                }
                benchmark::DoNotOptimize(value.value());
            }
        }
        ops += kBatchOps;
    }
    state.SetItemsProcessed(static_cast<int64_t>(ops));
    lat.Report(state);
}

BENCHMARK(BM_Mixed95_5)->Arg(8192);

// ------------------------------------------------------------------------
// Overwrite: 100% updates of existing keys (same value size, new bytes).
// Isolates NodeView::UpdateLeaf - the mixed workload is 95% reads, so an
// O(n) page rebuild in the update path hides in its tail. Added when review
// caught UpdateLeaf still doing the vector<string> rebuild that Phase 6
// removed from the insert path.
// ------------------------------------------------------------------------

void BM_Overwrite(benchmark::State& state) {
    TempDir dir("overwrite");
    Preloaded::CopyTo(dir.path());
    auto db = OpenDb(dir.path(), /*sync=*/false, 8192);
    std::mt19937_64 rng(BenchSeed());
    std::uniform_int_distribution<uint64_t> pick(0, kNumKeys - 1);

    LatencyRecorder lat;
    lat.Reserve(1 << 22);
    uint64_t ops = 0;
    uint64_t generation = 1;
    for (auto _ : state) {
        for (int i = 0; i < kBatchOps; ++i) {
            const uint64_t index = pick(rng);
            const std::string key = MakeKey(index);
            const std::string value = MakeValue(index, generation++);
            LatencyRecorder::Timer timer(lat);
            auto status = db->Put(key, value);
            if (!status.ok()) {
                state.SkipWithError(std::string(status.message()).c_str());
                return;
            }
        }
        ops += kBatchOps;
    }
    state.SetItemsProcessed(static_cast<int64_t>(ops));
    lat.Report(state);
}

BENCHMARK(BM_Overwrite)->Arg(8192);

// ------------------------------------------------------------------------
// Scan-heavy: 100-entry range scans from random start keys.
// ------------------------------------------------------------------------

void BM_ScanHeavy(benchmark::State& state) {
    auto db = OpenDb(Preloaded::Dir(), /*sync=*/false, 8192);
    std::mt19937_64 rng(BenchSeed());
    std::uniform_int_distribution<uint64_t> pick(0, kNumKeys - 1);
    constexpr int kScanLength = 100;

    LatencyRecorder lat;
    lat.Reserve(1 << 20);
    uint64_t entries = 0;
    for (auto _ : state) {
        const std::string start = MakeKey(pick(rng));
        int remaining = kScanLength;
        LatencyRecorder::Timer timer(lat);
        auto status = db->Scan(
            start, {}, [&](std::string_view, std::string_view value) {
                benchmark::DoNotOptimize(value.data());
                return --remaining > 0;
            });
        if (!status.ok()) {
            state.SkipWithError(std::string(status.message()).c_str());
            return;
        }
        entries += static_cast<uint64_t>(kScanLength - remaining);
    }
    // items = entries visited, so this is comparable to point-read ops/sec.
    state.SetItemsProcessed(static_cast<int64_t>(entries));
    state.counters["scans_per_s"] = benchmark::Counter(
        static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
    lat.Report(state);  // per-scan latency, not per-entry
}

BENCHMARK(BM_ScanHeavy);

// ------------------------------------------------------------------------
// Delete churn: delete an existing key, reinsert it with a fresh value.
// Working set stays at kNumKeys; exercises tombstones, compaction, and the
// free list. Counted as 2 ops (delete + put).
// ------------------------------------------------------------------------

void BM_DeleteChurn(benchmark::State& state) {
    TempDir dir("churn");
    Preloaded::CopyTo(dir.path());
    auto db = OpenDb(dir.path(), /*sync=*/false, 8192);
    std::mt19937_64 rng(BenchSeed());
    std::uniform_int_distribution<uint64_t> pick(0, kNumKeys - 1);

    LatencyRecorder lat;
    lat.Reserve(1 << 22);
    uint64_t ops = 0;
    uint64_t generation = 1;
    for (auto _ : state) {
        for (int i = 0; i < kBatchOps / 2; ++i) {
            const uint64_t index = pick(rng);
            const std::string key = MakeKey(index);
            const std::string value = MakeValue(index, generation++);
            {
                LatencyRecorder::Timer timer(lat);
                auto status = db->Delete(key);
                if (!status.ok()) {
                    state.SkipWithError(std::string(status.message()).c_str());
                    return;
                }
            }
            {
                LatencyRecorder::Timer timer(lat);
                auto status = db->Put(key, value);
                if (!status.ok()) {
                    state.SkipWithError(std::string(status.message()).c_str());
                    return;
                }
            }
        }
        ops += kBatchOps;
    }
    state.SetItemsProcessed(static_cast<int64_t>(ops));
    lat.Report(state);
}

BENCHMARK(BM_DeleteChurn);

}  // namespace

BENCHMARK_MAIN();
