// ==============================================================================
// tests/btree_test.cc - B+ tree behavior tests
// ==============================================================================
//
// Phase 1 task 1.19 plus regression tests for the Phase 1 defect fixes:
//   1.8  Put/Get descent mismatch on separator-equal keys
//   1.9  SplitLeaf dropping the left leaf's prev_page link
//
// ==============================================================================

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "kvstore/btree/btree.h"
#include "kvstore/btree/node.h"
#include "kvstore/storage/buffer_pool.h"
#include "kvstore/storage/pager.h"

namespace kvstore::btree {
namespace {

namespace fs = std::filesystem;

class BTreeTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::path(::testing::TempDir()) / "kvstore_btree_test";
        fs::create_directories(dir_);
        auto pager = storage::Pager::Open((dir_ / "data.db").string(), true);
        ASSERT_TRUE(pager.has_value()) << pager.error().message();
        pager_ = std::move(*pager);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // Keys sort lexicographically because of the fixed-width numeric suffix.
    static std::string Key(int i) { return std::format("key{:04d}", i); }

    // ~90-byte values so a leaf holds ~30 entries and 200 inserts split often.
    static std::string Value(int i) {
        return std::format("value-{:04d}-", i) + std::string(80, 'x');
    }

    static std::string UpdatedValue(int i) {
        return std::format("newval-{:04d}-", i) + std::string(80, 'y');
    }

    // Descend to the leftmost leaf by always taking the first child.
    // Flushes the pool first: callers read pages directly via the Pager.
    PageId LeftmostLeaf() {
        EXPECT_TRUE(pool_.FlushAll().ok());
        PageId current = pager_.root_page_id();
        while (true) {
            storage::Page page;
            EXPECT_TRUE(pager_.ReadPage(current, page).ok());
            NodeView node(page);
            if (node.IsLeaf()) {
                return current;
            }
            current = node.NumKeys() > 0 ? node.GetChild(0)
                                         : node.GetRightmostChild();
        }
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

// -----------------------------------------------------------------------------
// Basics
// -----------------------------------------------------------------------------

TEST_F(BTreeTest, GetOnEmptyTreeIsNotFound) {
    BPlusTree tree(pool_);
    auto result = tree.Get("missing");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), StatusCode::kNotFound);
}

TEST_F(BTreeTest, PutThenGetSingleKey) {
    BPlusTree tree(pool_);
    ASSERT_TRUE(tree.Put("alpha", "one").ok());
    EXPECT_EQ(tree.Get("alpha").value(), "one");
    EXPECT_FALSE(tree.Get("beta").has_value());
}

TEST_F(BTreeTest, UpdateWithinSingleLeaf) {
    BPlusTree tree(pool_);
    ASSERT_TRUE(tree.Put("alpha", "one").ok());
    ASSERT_TRUE(tree.Put("alpha", "two").ok());
    EXPECT_EQ(tree.Get("alpha").value(), "two");
}

// -----------------------------------------------------------------------------
// Splits
// -----------------------------------------------------------------------------

TEST_F(BTreeTest, AllKeysReadableAfterManySplits) {
    BPlusTree tree(pool_);
    constexpr int kCount = 200;
    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(tree.Put(Key(i), Value(i)).ok()) << Key(i);
    }
    for (int i = 0; i < kCount; ++i) {
        auto result = tree.Get(Key(i));
        ASSERT_TRUE(result.has_value()) << Key(i);
        EXPECT_EQ(result.value(), Value(i)) << Key(i);
    }
}

// Regression for defect 1.8: Put's descent (FindLeafPath) must pick the same
// child as Get's descent (FindChild) when the key equals an internal
// separator. Updating every key exercises separator-equal descents; a
// mismatch leaves a stale duplicate in the wrong leaf and Get sees the old
// value.
TEST_F(BTreeTest, UpdateEveryKeyAfterSplitsIsVisibleToGet) {
    BPlusTree tree(pool_);
    constexpr int kCount = 200;
    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(tree.Put(Key(i), Value(i)).ok()) << Key(i);
    }
    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(tree.Put(Key(i), UpdatedValue(i)).ok()) << Key(i);
    }
    for (int i = 0; i < kCount; ++i) {
        auto result = tree.Get(Key(i));
        ASSERT_TRUE(result.has_value()) << Key(i);
        EXPECT_EQ(result.value(), UpdatedValue(i)) << Key(i);
    }
}

// Regression for defect 1.9: SplitLeaf must preserve the left leaf's
// prev_page when it rebuilds the page. Walk the leaf chain forward, then
// backward, and require the same pages in reverse order.
TEST_F(BTreeTest, LeafChainIsConsistentInBothDirections) {
    BPlusTree tree(pool_);
    constexpr int kCount = 200;
    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(tree.Put(Key(i), Value(i)).ok()) << Key(i);
    }

    // Forward walk from the leftmost leaf.
    std::vector<PageId> forward;
    std::string prev_last_key;
    for (PageId current = LeftmostLeaf(); current != kInvalidPageId;) {
        storage::Page page;
        ASSERT_TRUE(pager_.ReadPage(current, page).ok());
        NodeView node(page);
        ASSERT_TRUE(node.IsLeaf());
        ASSERT_GT(node.NumKeys(), 0);
        // Keys across the chain must be globally sorted.
        EXPECT_LT(prev_last_key, std::string(node.GetKey(0)));
        prev_last_key = std::string(node.GetKey(node.NumKeys() - 1));

        forward.push_back(current);
        current = node.GetNextLeaf();
    }
    ASSERT_GE(forward.size(), 3u) << "test needs >= 3 leaves to be meaningful";

    // Backward walk from the last leaf must visit the same pages reversed.
    std::vector<PageId> backward;
    for (PageId current = forward.back(); current != kInvalidPageId;) {
        storage::Page page;
        ASSERT_TRUE(pager_.ReadPage(current, page).ok());
        NodeView node(page);
        backward.push_back(current);
        current = node.GetPrevLeaf();
    }

    std::vector<PageId> expected(forward.rbegin(), forward.rend());
    EXPECT_EQ(backward, expected);
}

// Regression for defect 1.10: meta num_keys must count unique keys - it
// increments on fresh inserts (including across splits) but not on updates.
TEST_F(BTreeTest, NumKeysCountsUniqueKeysNotUpdates) {
    BPlusTree tree(pool_);
    constexpr int kCount = 200;
    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(tree.Put(Key(i), Value(i)).ok()) << Key(i);
    }
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(tree.Put(Key(i), UpdatedValue(i)).ok()) << Key(i);
    }
    EXPECT_EQ(pager_.num_keys(), static_cast<uint64_t>(kCount));
}

// -----------------------------------------------------------------------------
// Phase 2 (task 2.2): Delete
// -----------------------------------------------------------------------------

TEST_F(BTreeTest, DeleteOnEmptyTreeIsNotFound) {
    BPlusTree tree(pool_);
    EXPECT_EQ(tree.Delete("missing").code(), StatusCode::kNotFound);
}

TEST_F(BTreeTest, DeleteExistingKeyThenGetIsNotFound) {
    BPlusTree tree(pool_);
    ASSERT_TRUE(tree.Put("alpha", "one").ok());
    ASSERT_TRUE(tree.Put("beta", "two").ok());

    ASSERT_TRUE(tree.Delete("alpha").ok());

    EXPECT_FALSE(tree.Get("alpha").has_value());
    EXPECT_EQ(tree.Get("beta").value(), "two");  // Neighbor untouched
}

TEST_F(BTreeTest, DeleteMissingKeyIsNotFoundAndChangesNothing) {
    BPlusTree tree(pool_);
    ASSERT_TRUE(tree.Put("alpha", "one").ok());

    EXPECT_EQ(tree.Delete("beta").code(), StatusCode::kNotFound);
    EXPECT_EQ(tree.Get("alpha").value(), "one");
    EXPECT_EQ(pager_.num_keys(), 1u);
}

TEST_F(BTreeTest, DeleteThenReinsertSameKey) {
    BPlusTree tree(pool_);
    ASSERT_TRUE(tree.Put("alpha", "one").ok());
    ASSERT_TRUE(tree.Delete("alpha").ok());
    ASSERT_TRUE(tree.Put("alpha", "reborn").ok());

    EXPECT_EQ(tree.Get("alpha").value(), "reborn");
    EXPECT_EQ(pager_.num_keys(), 1u);
}

TEST_F(BTreeTest, NumKeysDecrementsOnDelete) {
    BPlusTree tree(pool_);
    constexpr int kCount = 50;
    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(tree.Put(Key(i), Value(i)).ok()) << Key(i);
    }
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(tree.Delete(Key(i)).ok()) << Key(i);
    }
    EXPECT_EQ(pager_.num_keys(), static_cast<uint64_t>(kCount - 20));
}

// Deleting keys at and around leaf boundaries exercises descent to the
// correct leaf when the key equals an internal separator (equal goes right).
TEST_F(BTreeTest, DeleteAcrossSplitBoundaries) {
    BPlusTree tree(pool_);
    constexpr int kCount = 200;  // Splits into multiple leaves
    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(tree.Put(Key(i), Value(i)).ok()) << Key(i);
    }

    // Delete every third key, including whichever ones became separators.
    for (int i = 0; i < kCount; i += 3) {
        ASSERT_TRUE(tree.Delete(Key(i)).ok()) << Key(i);
    }

    for (int i = 0; i < kCount; ++i) {
        auto result = tree.Get(Key(i));
        if (i % 3 == 0) {
            EXPECT_FALSE(result.has_value()) << Key(i);
        } else {
            ASSERT_TRUE(result.has_value()) << Key(i);
            EXPECT_EQ(result.value(), Value(i)) << Key(i);
        }
    }
}

// Regression (found by the Phase 2 property test at op 3470, key "i"):
// updating an existing key with a value too large for its full leaf must
// split the leaf, not fail with Internal. Entry cost here is 319 bytes
// (slot 8 + key_len 2 + key 7 + value_len 2 + value 300), so 12 entries
// fill a 4056-byte payload to 3828 bytes; regrowing one value to 600 bytes
// overflows the rebuild (11*319 + 619 = 4128 > 4056).
TEST_F(BTreeTest, UpdateThatOutgrowsFullLeafSplitsInsteadOfFailing) {
    BPlusTree tree(pool_);
    const std::string small(300, 'a');
    for (int i = 0; i < 12; ++i) {
        ASSERT_TRUE(tree.Put(Key(i), small).ok()) << Key(i);
    }
    ASSERT_EQ(pager_.num_pages(), 3u) << "setup must not split yet";  // 2 meta + 1 leaf

    const std::string large(600, 'b');
    ASSERT_TRUE(tree.Put(Key(5), large).ok());

    EXPECT_EQ(tree.Get(Key(5)).value(), large);
    for (int i = 0; i < 12; ++i) {
        if (i != 5) {
            EXPECT_EQ(tree.Get(Key(i)).value(), small) << Key(i);
        }
    }
    // An update, even one that forces a split, must not change num_keys.
    EXPECT_EQ(pager_.num_keys(), 12u);
}

// Checkpoint C2 gate: under a put/delete churn workload the file must stop
// growing. Reinserted keys must land back in the leaves their deletes left
// dead bytes in (reclaimed by compaction) instead of forcing new splits.
TEST_F(BTreeTest, FileSizeStopsGrowingUnderPutDeleteChurn) {
    BPlusTree tree(pool_);
    constexpr int kCount = 300;  // Several leaves and an internal level

    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(tree.Put(Key(i), Value(i)).ok()) << Key(i);
    }
    const PageId pages_after_load = pager_.num_pages();

    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < kCount; ++i) {
            ASSERT_TRUE(tree.Delete(Key(i)).ok()) << "round " << round << " " << Key(i);
        }
        for (int i = 0; i < kCount; ++i) {
            ASSERT_TRUE(tree.Put(Key(i), Value(i)).ok()) << "round " << round << " " << Key(i);
        }
    }

    EXPECT_EQ(pager_.num_pages(), pages_after_load);
    EXPECT_EQ(pager_.num_keys(), static_cast<uint64_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        ASSERT_EQ(tree.Get(Key(i)).value(), Value(i)) << Key(i);
    }
}

}  // namespace
}  // namespace kvstore::btree
