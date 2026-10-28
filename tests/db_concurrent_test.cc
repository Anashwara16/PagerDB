// ==============================================================================
// tests/db_concurrent_test.cc - Concurrent DB-level writes (Phase 5.3+)
// ==============================================================================
//
// The tree and buffer pool are already tsan-clean (btree_concurrent_test,
// buffer_pool_concurrent_test), but those run at the tree level with
// wal=nullptr. This suite drives the FULL DbImpl path - WAL emission, meta
// (num_keys) updates, commit flushes - from many threads at once, which is
// where the LogWriter and Pager meta races live until 5.3a makes them safe.
//
// Run under tsan:
//   cmake --preset tsan && cmake --build build/tsan -j
//   ctest --preset tsan -R db_concurrent_test
//
// ==============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <memory>

#include "kvstore/db.h"

namespace kvstore {
namespace {

namespace fs = std::filesystem;

std::string Key(int thread_id, int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "t%02d-k%08d", thread_id, i);
    return buf;
}

class DbConcurrentTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::path(::testing::TempDir()) / "kvstore_db_concurrent_test";
        std::error_code ec;
        fs::remove_all(dir_, ec);
        db_path_ = dir_.string();
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    fs::path dir_;
    std::string db_path_;
};

// Each thread owns a disjoint key namespace and Puts through the public DB API
// (autocommit + WAL). No write should be lost or corrupted, num_keys must be
// exact, and tsan must find no race in the WAL / meta path.
TEST_F(DbConcurrentTest, DisjointAutocommitWritersRoundTrip) {
    Options options;
    options.create_if_missing = true;
    options.sync_on_write = false;         // Exercise Append without per-put fsync
    options.max_wal_bytes = 0;             // No mid-test auto-checkpoint
    options.buffer_pool_frames = 256;
    auto db = DB::Open(db_path_, options).value();

    constexpr int kThreads = 8;
    constexpr int kPerThread = 1500;

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                if (!db->Put(Key(t, i), "v" + std::to_string(i)).ok()) {
                    ++failures;
                    return;
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    ASSERT_EQ(failures.load(), 0);

    // Every key present with the right value.
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPerThread; ++i) {
            auto got = db->Get(Key(t, i));
            ASSERT_TRUE(got.has_value()) << Key(t, i);
            EXPECT_EQ(*got, "v" + std::to_string(i));
        }
    }
    EXPECT_EQ(db->ApproximateKeyCount(),
              static_cast<uint64_t>(kThreads) * kPerThread);
}

// Mixed concurrent Put / Get / Delete on disjoint namespaces: the meta key
// count must stay exact under concurrent increments and decrements.
TEST_F(DbConcurrentTest, MixedPutDeleteKeepsKeyCountExact) {
    Options options;
    options.create_if_missing = true;
    options.sync_on_write = false;
    options.max_wal_bytes = 0;
    options.buffer_pool_frames = 256;
    auto db = DB::Open(db_path_, options).value();

    constexpr int kThreads = 6;
    constexpr int kPerThread = 1000;

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            // Insert kPerThread, then delete the odd-indexed half back out.
            for (int i = 0; i < kPerThread; ++i) {
                if (!db->Put(Key(t, i), "v").ok()) { ++failures; return; }
            }
            for (int i = 1; i < kPerThread; i += 2) {
                if (!db->Delete(Key(t, i)).ok()) { ++failures; return; }
            }
        });
    }
    for (auto& th : threads) th.join();
    ASSERT_EQ(failures.load(), 0);

    // Each thread leaves ceil(kPerThread/2) keys (the even indices).
    const uint64_t per_thread_left = (kPerThread + 1) / 2;
    EXPECT_EQ(db->ApproximateKeyCount(),
              static_cast<uint64_t>(kThreads) * per_thread_left);
}

// Concurrent EXPLICIT transactions on disjoint namespaces. Each thread runs
// many multi-key transactions, committing most and aborting some; committed
// keys must be present, aborted keys absent, and the WAL/txn paths tsan-clean.
TEST_F(DbConcurrentTest, ConcurrentTransactionsCommitAndAbort) {
    Options options;
    options.create_if_missing = true;
    options.sync_on_write = false;
    options.max_wal_bytes = 0;
    options.buffer_pool_frames = 256;
    auto db = DB::Open(db_path_, options).value();

    constexpr int kThreads = 8;
    constexpr int kTxnsPerThread = 150;
    constexpr int kKeysPerTxn = 5;

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int n = 0; n < kTxnsPerThread; ++n) {
                auto txn_result = db->Begin();
                if (!txn_result) { ++failures; return; }
                auto txn = std::move(*txn_result);
                for (int k = 0; k < kKeysPerTxn; ++k) {
                    if (!txn->Put(Key(t, n * kKeysPerTxn + k), "v").ok()) {
                        ++failures;
                        return;
                    }
                }
                // Abort every 5th transaction; commit the rest.
                Status s = (n % 5 == 4) ? txn->Abort() : txn->Commit();
                if (!s.ok()) { ++failures; return; }
            }
        });
    }
    for (auto& th : threads) th.join();
    ASSERT_EQ(failures.load(), 0);

    for (int t = 0; t < kThreads; ++t) {
        for (int n = 0; n < kTxnsPerThread; ++n) {
            const bool committed = (n % 5 != 4);
            for (int k = 0; k < kKeysPerTxn; ++k) {
                auto got = db->Get(Key(t, n * kKeysPerTxn + k));
                EXPECT_EQ(got.has_value(), committed)
                    << "t=" << t << " n=" << n << " k=" << k;
            }
        }
    }
}

}  // namespace
}  // namespace kvstore
