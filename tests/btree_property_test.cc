// ==============================================================================
// tests/btree_property_test.cc - Randomized B+ tree property tests
// ==============================================================================
//
// Phase 1 task 1.20: 10k seeded random operations checked against std::map,
// then a full leaf-chain scan compared with the sorted reference. This is the
// broadest correctness net for descent, splits, and the sibling chain.
//
// Phase 2: 50k interleaved Put/Delete ops with periodic cursor scans, again
// against std::map - the delete/compaction/cursor machinery under load.
//
// ==============================================================================

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "kvstore/btree/btree.h"
#include "kvstore/btree/cursor.h"
#include "kvstore/btree/node.h"
#include "kvstore/storage/buffer_pool.h"
#include "kvstore/storage/pager.h"

namespace kvstore::btree {
namespace {

namespace fs = std::filesystem;

class BTreePropertyTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::path(::testing::TempDir()) / "kvstore_btree_property_test";
        fs::create_directories(dir_);
        auto pager = storage::Pager::Open((dir_ / "data.db").string(), true);
        ASSERT_TRUE(pager.has_value()) << pager.error().message();
        pager_ = std::move(*pager);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    fs::path dir_;
    storage::Pager pager_;
    storage::BufferPool pool_{pager_, PoolFrames()};

    // Pool size for the fixture: KVSTORE_POOL_FRAMES env (the Phase 3
    // pool-size regression gate) or a 64-frame default. Clamped to 16 like
    // DbImpl: a splitting Put pins path + rewritten pages until its WAL
    // op-group is complete (up to 2*depth+2 frames).
    static size_t PoolFrames() {
        size_t frames = 64;
        const char* env = std::getenv("KVSTORE_POOL_FRAMES");
        if (env != nullptr && *env != '\0') {
            long parsed = std::strtol(env, nullptr, 10);
            if (parsed > 0) {
                frames = static_cast<size_t>(parsed);
            }
        }
        return frames < 16 ? 16 : frames;
    }

};

TEST_F(BTreePropertyTest, TenThousandRandomPutsMatchStdMap) {
    BPlusTree tree(pool_);
    std::map<std::string, std::string> reference;

    std::mt19937 rng(20260705);  // Fixed seed: failures must be reproducible
    std::uniform_int_distribution<int> key_len(1, 24);
    std::uniform_int_distribution<int> value_len(0, 64);
    std::uniform_int_distribution<int> letter(0, 25);

    auto random_string = [&](int len) {
        std::string s;
        s.reserve(static_cast<size_t>(len));
        for (int i = 0; i < len; ++i) {
            s.push_back(static_cast<char>('a' + letter(rng)));
        }
        return s;
    };

    // Random keys collide occasionally, so this also exercises updates.
    constexpr int kOps = 10000;
    for (int i = 0; i < kOps; ++i) {
        std::string key = random_string(key_len(rng));
        std::string value = random_string(value_len(rng));
        ASSERT_TRUE(tree.Put(key, value).ok()) << "op " << i << " key " << key;
        reference[key] = std::move(value);
    }

    // Point lookups: every reference entry must be visible with its latest value.
    for (const auto& [key, value] : reference) {
        auto result = tree.Get(key);
        ASSERT_TRUE(result.has_value()) << key;
        EXPECT_EQ(result.value(), value) << key;
    }

    // Keys that were never inserted must stay absent.
    for (int i = 0; i < 100; ++i) {
        std::string missing = random_string(key_len(rng)) + "!";  // '!' < 'a', never generated
        EXPECT_FALSE(tree.Get(missing).has_value()) << missing;
    }

    // Key count statistic matches the number of unique keys.
    EXPECT_EQ(pager_.num_keys(), reference.size());

    // Full leaf-chain scan via raw pager reads: flush the pool first.
    ASSERT_TRUE(pool_.FlushAll().ok());
    PageId current = pager_.root_page_id();
    ASSERT_NE(current, kInvalidPageId);
    // Descend to the leftmost leaf.
    for (;;) {
        storage::Page page;
        ASSERT_TRUE(pager_.ReadPage(current, page).ok());
        NodeView node(page);
        if (node.IsLeaf()) {
            break;
        }
        current = node.NumKeys() > 0 ? node.GetChild(0) : node.GetRightmostChild();
    }

    auto it = reference.begin();
    size_t scanned = 0;
    while (current != kInvalidPageId) {
        storage::Page page;
        ASSERT_TRUE(pager_.ReadPage(current, page).ok());
        NodeView node(page);
        ASSERT_TRUE(node.IsLeaf());
        for (uint16_t i = 0; i < node.NumKeys(); ++i, ++it, ++scanned) {
            ASSERT_NE(it, reference.end()) << "chain has more keys than reference";
            EXPECT_EQ(node.GetKey(i), it->first);
            EXPECT_EQ(node.GetValueAt(i), it->second);
        }
        current = node.GetNextLeaf();
    }
    EXPECT_EQ(scanned, reference.size());
    EXPECT_EQ(it, reference.end());
}

// Phase 2 property test: 50k interleaved Put/Delete operations mirrored into
// std::map. Deletes target existing keys (via a recycled candidate pool, so
// double-deletes occur naturally and must return NotFound) as well as keys
// that never existed. Every 10k ops a full cursor scan must reproduce the
// reference map exactly.
TEST_F(BTreePropertyTest, FiftyThousandInterleavedPutsAndDeletesMatchStdMap) {
    BPlusTree tree(pool_);
    std::map<std::string, std::string> reference;

    std::mt19937 rng(20260706);  // Fixed seed: failures must be reproducible
    std::uniform_int_distribution<int> key_len(1, 24);
    std::uniform_int_distribution<int> value_len(0, 64);
    std::uniform_int_distribution<int> letter(0, 25);
    std::uniform_int_distribution<int> op_roll(0, 99);

    auto random_string = [&](int len) {
        std::string s;
        s.reserve(static_cast<size_t>(len));
        for (int i = 0; i < len; ++i) {
            s.push_back(static_cast<char>('a' + letter(rng)));
        }
        return s;
    };

    // Delete candidates: every inserted key lands here; picks may be stale
    // (already deleted), which exercises the NotFound path under load.
    std::vector<std::string> candidates;

    auto check_full_scan = [&](int op_index) {
        auto it = reference.begin();
        size_t scanned = 0;
        for (Cursor cursor = tree.SeekToFirst(); cursor.Valid(); cursor.Next()) {
            ASSERT_NE(it, reference.end())
                << "op " << op_index << ": scan has extra key " << cursor.key();
            EXPECT_EQ(cursor.key(), it->first) << "op " << op_index;
            EXPECT_EQ(cursor.value(), it->second) << "op " << op_index;
            ++it;
            ++scanned;
        }
        EXPECT_EQ(scanned, reference.size()) << "op " << op_index;
        EXPECT_EQ(it, reference.end()) << "op " << op_index;
    };

    constexpr int kOps = 50000;
    for (int i = 0; i < kOps; ++i) {
        const int roll = op_roll(rng);
        if (roll < 60 || candidates.empty()) {
            // Put (fresh insert or update on collision)
            std::string key = random_string(key_len(rng));
            std::string value = random_string(value_len(rng));
            ASSERT_TRUE(tree.Put(key, value).ok()) << "op " << i << " key " << key;
            reference[key] = std::move(value);
            candidates.push_back(std::move(key));
        } else if (roll < 90) {
            // Delete a candidate (existing or stale)
            std::uniform_int_distribution<size_t> pick(0, candidates.size() - 1);
            const size_t idx = pick(rng);
            std::string key = std::move(candidates[idx]);
            candidates[idx] = std::move(candidates.back());
            candidates.pop_back();

            const bool was_present = reference.erase(key) > 0;
            Status s = tree.Delete(key);
            if (was_present) {
                ASSERT_TRUE(s.ok()) << "op " << i << " key " << key;
            } else {
                ASSERT_EQ(s.code(), StatusCode::kNotFound)
                    << "op " << i << " key " << key;
            }
        } else {
            // Delete a key that never existed ('!' sorts below 'a')
            std::string key = random_string(key_len(rng)) + "!";
            ASSERT_EQ(tree.Delete(key).code(), StatusCode::kNotFound)
                << "op " << i << " key " << key;
        }

        if ((i + 1) % 10000 == 0) {
            check_full_scan(i + 1);
            ASSERT_EQ(pager_.num_keys(), reference.size()) << "op " << i + 1;
        }
    }

    // Final verification: point lookups for every surviving key...
    for (const auto& [key, value] : reference) {
        auto result = tree.Get(key);
        ASSERT_TRUE(result.has_value()) << key;
        EXPECT_EQ(result.value(), value) << key;
    }
    // ...and a sample of bounded scans (half-open [start, end)).
    for (int i = 0; i < 20; ++i) {
        std::string start = random_string(key_len(rng));
        std::string end = random_string(key_len(rng));
        if (end < start) {
            std::swap(start, end);
        }
        auto it = reference.lower_bound(start);
        for (Cursor cursor = tree.Seek(start); cursor.Valid(); cursor.Next()) {
            if (cursor.key() >= end) {
                break;
            }
            ASSERT_NE(it, reference.end()) << "bounded scan " << i;
            ASSERT_LT(it->first, end) << "bounded scan " << i;
            EXPECT_EQ(cursor.key(), it->first) << "bounded scan " << i;
            ++it;
        }
        // The reference iterator must also have reached the bound.
        EXPECT_TRUE(it == reference.end() || it->first >= end)
            << "bounded scan " << i;
    }
}

}  // namespace
}  // namespace kvstore::btree
