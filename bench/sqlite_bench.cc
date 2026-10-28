// ==============================================================================
// bench/sqlite_bench.cc - SQLite baseline for docs/BENCHMARKS.md (task 6.1)
// ==============================================================================
//
// Runs the same workloads as db_bench.cc (same keys, values, seed, op mix -
// see bench_util.h) against SQLite so PagerDB's numbers have a professional
// reference point. Configuration chosen to be the closest analogue of each
// PagerDB mode, not to handicap SQLite:
//
//   - PRAGMA journal_mode=WAL        - SQLite's WAL, like our Phase 4 log
//   - CREATE TABLE ... WITHOUT ROWID - clustered B-tree keyed on `k`, the
//                                      same shape as our B+ tree (a rowid
//                                      table would add an indirection)
//   - PRAGMA synchronous=OFF         - matches sync_on_write=false (WAL is
//                                      written but not fsynced per commit)
//   - PRAGMA synchronous=FULL        - the *Sync variants, matching
//                                      sync_on_write=true
//   - single connection, autocommit  - matches our autocommit-only API
//   - prepared statements reused     - any real client would
//
// Known gaps to keep in mind when reading the numbers (also written up in
// docs/BENCHMARKS.md): SQLite parses/plans even prepared statements' bindings,
// but amortized; its page cache defaults to ~2 MB (we set 32 MB to mirror the
// 8192-frame runs); it maintains full ACID machinery we don't have yet.
//
// LMDB baseline omitted: not installed on the benchmark machine and adding a
// source drop just for a second baseline was judged not worth the dependency;
// plan.md 6.1 allows "SQLite and/or LMDB".
//
// ==============================================================================

#include <benchmark/benchmark.h>
#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>

#include "bench_util.h"

namespace {

using namespace kvbench;

constexpr uint64_t kNumKeys = 100'000;
constexpr int kBatchOps = 1'000;

// Thin RAII wrapper: open, pragmas, prepared statements.
class SqliteDb {
public:
    SqliteDb(const std::filesystem::path& dir, bool synchronous_full) {
        const std::string file = (dir / "sqlite.db").string();
        if (sqlite3_open(file.c_str(), &db_) != SQLITE_OK) {
            Fail("open");
        }
        Exec("PRAGMA journal_mode=WAL");
        Exec(synchronous_full ? "PRAGMA synchronous=FULL"
                              : "PRAGMA synchronous=OFF");
        Exec("PRAGMA cache_size=-32768");  // 32 MB, mirrors the 8192-frame runs
        Exec("CREATE TABLE IF NOT EXISTS kv ("
             "k BLOB PRIMARY KEY, v BLOB NOT NULL) WITHOUT ROWID");
        Prepare(&put_, "INSERT INTO kv(k, v) VALUES(?, ?) "
                       "ON CONFLICT(k) DO UPDATE SET v=excluded.v");
        Prepare(&get_, "SELECT v FROM kv WHERE k = ?");
        Prepare(&del_, "DELETE FROM kv WHERE k = ?");
        Prepare(&scan_, "SELECT k, v FROM kv WHERE k >= ? ORDER BY k LIMIT ?");
    }

    ~SqliteDb() {
        sqlite3_finalize(put_);
        sqlite3_finalize(get_);
        sqlite3_finalize(del_);
        sqlite3_finalize(scan_);
        sqlite3_close(db_);
    }
    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;

    bool Put(const std::string& key, const std::string& value) {
        sqlite3_reset(put_);
        sqlite3_bind_blob(put_, 1, key.data(), static_cast<int>(key.size()),
                          SQLITE_STATIC);
        sqlite3_bind_blob(put_, 2, value.data(),
                          static_cast<int>(value.size()), SQLITE_STATIC);
        return sqlite3_step(put_) == SQLITE_DONE;
    }

    bool Get(const std::string& key, std::string& value_out) {
        sqlite3_reset(get_);
        sqlite3_bind_blob(get_, 1, key.data(), static_cast<int>(key.size()),
                          SQLITE_STATIC);
        if (sqlite3_step(get_) != SQLITE_ROW) {
            return false;
        }
        const void* blob = sqlite3_column_blob(get_, 0);
        const int len = sqlite3_column_bytes(get_, 0);
        value_out.assign(static_cast<const char*>(blob),
                         static_cast<size_t>(len));
        return true;
    }

    bool Delete(const std::string& key) {
        sqlite3_reset(del_);
        sqlite3_bind_blob(del_, 1, key.data(), static_cast<int>(key.size()),
                          SQLITE_STATIC);
        return sqlite3_step(del_) == SQLITE_DONE;
    }

    // Returns number of entries visited (up to `limit`).
    int Scan(const std::string& start, int limit) {
        sqlite3_reset(scan_);
        sqlite3_bind_blob(scan_, 1, start.data(),
                          static_cast<int>(start.size()), SQLITE_STATIC);
        sqlite3_bind_int(scan_, 2, limit);
        int count = 0;
        while (sqlite3_step(scan_) == SQLITE_ROW) {
            benchmark::DoNotOptimize(sqlite3_column_blob(scan_, 1));
            ++count;
        }
        return count;
    }

    void Exec(const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::fprintf(stderr, "sqlite exec failed: %s: %s\n", sql,
                         err ? err : "?");
            std::abort();
        }
    }

private:
    void Prepare(sqlite3_stmt** stmt, const char* sql) {
        if (sqlite3_prepare_v2(db_, sql, -1, stmt, nullptr) != SQLITE_OK) {
            Fail(sql);
        }
    }

    void Fail(const char* what) {
        std::fprintf(stderr, "sqlite error at %s: %s\n", what,
                     sqlite3_errmsg(db_));
        std::abort();
    }

    sqlite3* db_ = nullptr;
    sqlite3_stmt* put_ = nullptr;
    sqlite3_stmt* get_ = nullptr;
    sqlite3_stmt* del_ = nullptr;
    sqlite3_stmt* scan_ = nullptr;
};

void LoadKeys(SqliteDb& db, uint64_t n, bool sequential, uint64_t seed) {
    std::vector<uint64_t> order(n);
    for (uint64_t i = 0; i < n; ++i) {
        order[i] = i;
    }
    if (!sequential) {
        std::mt19937_64 rng(seed);
        std::shuffle(order.begin(), order.end(), rng);
    }
    for (uint64_t index : order) {
        if (!db.Put(MakeKey(index), MakeValue(index))) {
            std::fprintf(stderr, "sqlite preload Put failed\n");
            std::abort();
        }
    }
}

// Preloaded pristine database, built once per process (mirrors db_bench).
class Preloaded {
public:
    static const std::filesystem::path& Dir() {
        static Preloaded instance;
        return instance.dir_->path();
    }

    static void CopyTo(const std::filesystem::path& dst) {
        std::filesystem::copy(Dir(), dst,
                              std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    }

private:
    Preloaded() : dir_(std::make_unique<TempDir>("sqlite_preload")) {
        SqliteDb db(dir_->path(), /*synchronous_full=*/false);
        LoadKeys(db, kNumKeys, /*sequential=*/true, BenchSeed());
        db.Exec("PRAGMA wal_checkpoint(TRUNCATE)");
    }

    std::unique_ptr<TempDir> dir_;
};

// ------------------------------------------------------------------------
// Workloads - mirror db_bench.cc one for one.
// ------------------------------------------------------------------------

void RunLoad(benchmark::State& state, bool sequential, bool synchronous_full) {
    const auto n = static_cast<uint64_t>(state.range(0));
    LatencyRecorder lat;
    lat.Reserve(n * 4);
    uint64_t total_ops = 0;

    for (auto _ : state) {
        state.PauseTiming();
        TempDir dir(sequential ? "sq_seqload" : "sq_randload");
        std::vector<uint64_t> order(n);
        for (uint64_t i = 0; i < n; ++i) {
            order[i] = i;
        }
        if (!sequential) {
            std::mt19937_64 rng(BenchSeed());
            std::shuffle(order.begin(), order.end(), rng);
        }
        auto db = std::make_unique<SqliteDb>(dir.path(), synchronous_full);
        state.ResumeTiming();

        for (uint64_t index : order) {
            const std::string key = MakeKey(index);
            const std::string value = MakeValue(index);
            LatencyRecorder::Timer timer(lat);
            if (!db->Put(key, value)) {
                state.SkipWithError("sqlite Put failed");
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
void BM_SeqLoadSync(benchmark::State& state) { RunLoad(state, true, true); }

BENCHMARK(BM_SeqLoad)->Arg(kNumKeys)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_RandLoad)->Arg(kNumKeys)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_SeqLoadSync)->Arg(2'000)->Unit(benchmark::kMillisecond);

void RunReadOnly(benchmark::State& state, bool zipfian) {
    SqliteDb db(Preloaded::Dir(), /*synchronous_full=*/false);
    std::mt19937_64 rng(BenchSeed());
    std::uniform_int_distribution<uint64_t> uniform(0, kNumKeys - 1);
    ZipfianGenerator zipf(kNumKeys);

    LatencyRecorder lat;
    lat.Reserve(1 << 22);
    uint64_t ops = 0;
    std::string value;
    for (auto _ : state) {
        for (int i = 0; i < kBatchOps; ++i) {
            const uint64_t index =
                zipfian ? zipf.NextIndex(rng) : uniform(rng);
            const std::string key = MakeKey(index);
            LatencyRecorder::Timer timer(lat);
            if (!db.Get(key, value)) {
                state.SkipWithError("sqlite Get miss");
                return;
            }
            benchmark::DoNotOptimize(value);
        }
        ops += kBatchOps;
    }
    state.SetItemsProcessed(static_cast<int64_t>(ops));
    lat.Report(state);
}

void BM_ReadUniform(benchmark::State& state) { RunReadOnly(state, false); }
void BM_ReadZipfian(benchmark::State& state) { RunReadOnly(state, true); }

BENCHMARK(BM_ReadUniform);
BENCHMARK(BM_ReadZipfian);

void BM_Mixed95_5(benchmark::State& state) {
    TempDir dir("sq_mixed");
    Preloaded::CopyTo(dir.path());
    SqliteDb db(dir.path(), /*synchronous_full=*/false);
    std::mt19937_64 rng(BenchSeed());
    std::uniform_int_distribution<uint64_t> pick(0, kNumKeys - 1);
    std::uniform_int_distribution<int> percent(0, 99);

    LatencyRecorder lat;
    lat.Reserve(1 << 22);
    uint64_t ops = 0;
    uint64_t generation = 1;
    std::string value;
    for (auto _ : state) {
        for (int i = 0; i < kBatchOps; ++i) {
            const uint64_t index = pick(rng);
            const std::string key = MakeKey(index);
            if (percent(rng) < 5) {
                const std::string new_value = MakeValue(index, generation++);
                LatencyRecorder::Timer timer(lat);
                if (!db.Put(key, new_value)) {
                    state.SkipWithError("sqlite Put failed");
                    return;
                }
            } else {
                LatencyRecorder::Timer timer(lat);
                if (!db.Get(key, value)) {
                    state.SkipWithError("sqlite Get miss");
                    return;
                }
                benchmark::DoNotOptimize(value);
            }
        }
        ops += kBatchOps;
    }
    state.SetItemsProcessed(static_cast<int64_t>(ops));
    lat.Report(state);
}

BENCHMARK(BM_Mixed95_5);

void BM_ScanHeavy(benchmark::State& state) {
    SqliteDb db(Preloaded::Dir(), /*synchronous_full=*/false);
    std::mt19937_64 rng(BenchSeed());
    std::uniform_int_distribution<uint64_t> pick(0, kNumKeys - 1);
    constexpr int kScanLength = 100;

    LatencyRecorder lat;
    lat.Reserve(1 << 20);
    uint64_t entries = 0;
    for (auto _ : state) {
        const std::string start = MakeKey(pick(rng));
        LatencyRecorder::Timer timer(lat);
        entries += static_cast<uint64_t>(db.Scan(start, kScanLength));
    }
    state.SetItemsProcessed(static_cast<int64_t>(entries));
    state.counters["scans_per_s"] = benchmark::Counter(
        static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
    lat.Report(state);
}

BENCHMARK(BM_ScanHeavy);

void BM_DeleteChurn(benchmark::State& state) {
    TempDir dir("sq_churn");
    Preloaded::CopyTo(dir.path());
    SqliteDb db(dir.path(), /*synchronous_full=*/false);
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
                if (!db.Delete(key)) {
                    state.SkipWithError("sqlite Delete failed");
                    return;
                }
            }
            {
                LatencyRecorder::Timer timer(lat);
                if (!db.Put(key, value)) {
                    state.SkipWithError("sqlite Put failed");
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
