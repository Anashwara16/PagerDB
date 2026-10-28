// ==============================================================================
// tests/btree_concurrent_test.cc - Latch-crabbing stress (Phase 5.2)
// ==============================================================================
// Run under tsan: cmake --preset tsan && cmake --build build/tsan -j
//                 ctest --preset tsan -R btree_concurrent_test
// ==============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kvstore/btree/btree.h"
#include "kvstore/storage/buffer_pool.h"
#include "kvstore/storage/pager.h"

namespace {

using namespace kvstore;

struct TreeFixture {
    std::string dir;
    std::optional<storage::Pager> pager;
    std::optional<storage::BufferPool> pool;
    std::optional<btree::BPlusTree> tree;

    explicit TreeFixture(const std::string& d) : dir(d) {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        pager.emplace(storage::Pager::Open((std::filesystem::path(dir) / "data.db").string(),
                                           /*create=*/true, /*verify=*/true).value());
        pool.emplace(*pager, /*num_frames=*/128, /*wal=*/nullptr);
        tree.emplace(*pool, /*wal=*/nullptr);
    }
};

std::string Key(int thread_id, int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "t%02d-k%08d", thread_id, i);
    return buf;
}

// (a) Disjoint key ranges: each thread owns a key namespace and validates its
// own writes against a private std::map — no lost/torn writes.
TEST(BTreeConcurrent, DisjointRangesRoundTrip) {
    TreeFixture fx("/tmp/kvstore_btree_concurrent_disjoint");
    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;

    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::map<std::string, std::string> ref;
            std::mt19937 rng(static_cast<unsigned>(t + 1));
            for (int i = 0; i < kPerThread; ++i) {
                std::string k = Key(t, static_cast<int>(rng() % 500));
                std::string v = std::to_string(rng());
                if (!fx.tree->Put(k, v).ok()) { ++failures; return; }
                ref[k] = v;
            }
            for (const auto& [k, v] : ref) {
                auto got = fx.tree->Get(k);
                if (!got || *got != v) { ++failures; return; }
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(failures.load(), 0);
}

// (b) The Phase 5.2 gate: 8 threads of MIXED put/get/scan/delete, each owning
// a key namespace and checking every operation against its private std::map.
// A thread's own ops are sequential, so at any scan point its namespace's
// true contents equal its map exactly - even while the 7 other threads split
// and mutate leaves under the scan.
TEST(BTreeConcurrent, MixedPutGetScanDeleteMatchesPerThreadModel) {
    TreeFixture fx("/tmp/kvstore_btree_concurrent_mixed");
    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 3000;

    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::map<std::string, std::string> ref;
            std::mt19937 rng(static_cast<unsigned>(1000 + t));
            // Half-open namespace bounds for this thread's scans: keys are
            // "tNN-..." so the exclusive end "tNN." (('.' = '-'+1) works.
            const std::string ns_start = Key(t, 0).substr(0, 4);
            std::string ns_end = ns_start;
            ns_end.back() = static_cast<char>(ns_end.back() + 1);

            for (int i = 0; i < kOpsPerThread; ++i) {
                const int roll = static_cast<int>(rng() % 100);
                std::string k = Key(t, static_cast<int>(rng() % 400));
                if (roll < 50) {  // Put
                    std::string v = std::to_string(rng());
                    if (!fx.tree->Put(k, v).ok()) { ++failures; return; }
                    ref[k] = v;
                } else if (roll < 70) {  // Delete (absent -> NotFound is fine)
                    Status s = fx.tree->Delete(k);
                    if (!s.ok() && s.code() != StatusCode::kNotFound) {
                        ++failures;
                        return;
                    }
                    ref.erase(k);
                } else if (roll < 95) {  // Get
                    auto got = fx.tree->Get(k);
                    auto it = ref.find(k);
                    const bool want = it != ref.end();
                    if (got.has_value() != want ||
                        (want && *got != it->second)) {
                        ++failures;
                        return;
                    }
                } else {
                    // Scan: full chain walk filtered to this namespace, which
                    // must equal the model exactly (this thread's ops are
                    // sequential; other threads only touch foreign keys).
                    // A full walk from the first leaf is used instead of
                    // Seek(ns_start): Seek's start index is computed without
                    // latch continuity, so a concurrent insert/delete in a
                    // boundary leaf shared with a neighbor namespace can
                    // shift it. The copy-based chain walk itself is split-
                    // safe (a copied leaf subsumes its post-split halves).
                    std::map<std::string, std::string> seen;
                    auto cursor = fx.tree->SeekToFirst();
                    for (; cursor.Valid(); cursor.Next()) {
                        if (cursor.key() >= ns_end) break;
                        if (cursor.key() >= ns_start) {
                            seen.emplace(cursor.key(), cursor.value());
                        }
                    }
                    if (!cursor.status().ok() || seen != ref) {
                        ++failures;
                        return;
                    }
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(failures.load(), 0);
}

// (c) Shared keys + concurrent readers: readers never observe a torn page
// (Get either finds a well-formed value or NotFound; never crashes / garbage).
TEST(BTreeConcurrent, SharedKeysReadersStayConsistent) {
    TreeFixture fx("/tmp/kvstore_btree_concurrent_shared");
    constexpr int kKeys = 400;
    for (int i = 0; i < kKeys; ++i) {
        ASSERT_TRUE(fx.tree->Put(Key(0, i), "v0").ok());
    }

    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 6; ++r) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                for (int i = 0; i < kKeys; ++i) {
                    auto got = fx.tree->Get(Key(0, i));
                    if (got && got->substr(0, 1) != "v") ++bad;  // garbage value
                }
            }
        });
    }
    std::thread writer([&] {
        for (int round = 0; round < 50; ++round) {
            for (int i = 0; i < kKeys; ++i) {
                (void)fx.tree->Put(Key(0, i), "v" + std::to_string(round));
            }
        }
        stop.store(true);
    });
    writer.join();
    for (auto& th : readers) th.join();
    EXPECT_EQ(bad.load(), 0);
}

// (d) Regression: Seek must read root_page_id under root_latch. It used to
// short-circuit through an UNLATCHED IsEmpty() - a data race against
// SetRootPageId (first insert, root splits) that tsan flags here, and a
// functional bug: a scan racing tree growth could spuriously report an
// empty tree. Scanners run against a tree growing from EMPTY in the same
// key space (the other stress tests use disjoint namespaces and pre-grown
// trees, which is exactly why they never caught this).
TEST(BTreeConcurrent, SeekRacingFirstInsertAndRootSplitsIsLatched) {
    TreeFixture fx("/tmp/kvstore_btree_concurrent_seek_race");
    constexpr int kKeys = 3000;  // Enough for several root changes

    std::atomic<int> keys_confirmed{0};
    std::atomic<int> failures{0};

    std::vector<std::thread> scanners;
    for (int r = 0; r < 4; ++r) {
        scanners.emplace_back([&] {
            while (keys_confirmed.load(std::memory_order_acquire) < kKeys) {
                const int confirmed =
                    keys_confirmed.load(std::memory_order_acquire);
                auto cursor = fx.tree->Seek("");
                if (!cursor.status().ok()) { ++failures; return; }
                // Once at least one Put has returned, the tree is non-empty
                // forever (nothing deletes) - Seek("") must land on a key.
                if (confirmed > 0 && !cursor.Valid()) {
                    ++failures;  // Spurious "empty tree" - the old race
                    return;
                }
            }
        });
    }
    std::thread writer([&] {
        for (int i = 0; i < kKeys; ++i) {
            if (!fx.tree->Put(Key(0, i), "v").ok()) { ++failures; return; }
            keys_confirmed.store(i + 1, std::memory_order_release);
        }
    });
    writer.join();
    for (auto& th : scanners) th.join();
    EXPECT_EQ(failures.load(), 0);
}

}  // namespace
