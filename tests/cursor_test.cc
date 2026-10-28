// ==============================================================================
// tests/cursor_test.cc - Cursor / range scan tests
// ==============================================================================
//
// Phase 2 (tasks 2.9/2.10): leaf-chain cursor. Full scans must visit every
// key in sorted order, Seek must land on the lower bound, and scans must
// skip keys removed by Delete - including leaves that became entirely empty
// (decision 2.8 keeps them in the sibling chain).
//
// ==============================================================================

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <format>
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

class CursorTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::path(::testing::TempDir()) / "kvstore_cursor_test";
        fs::create_directories(dir_);
        auto pager = storage::Pager::Open((dir_ / "data.db").string(), true);
        ASSERT_TRUE(pager.has_value()) << pager.error().message();
        pager_ = std::move(*pager);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    static std::string Key(int i) { return std::format("key{:04d}", i); }

    // ~90-byte values so 200 inserts split into several leaves.
    static std::string Value(int i) {
        return std::format("value-{:04d}-", i) + std::string(80, 'x');
    }

    // Collect the keys of every leaf, in chain order, one vector per leaf.
    // Flushes the pool first: this helper reads pages directly via the Pager.
    std::vector<std::vector<std::string>> LeafKeyGroups() {
        EXPECT_TRUE(pool_.FlushAll().ok());
        std::vector<std::vector<std::string>> groups;
        PageId current = pager_.root_page_id();
        // Descend to the leftmost leaf.
        while (current != kInvalidPageId) {
            storage::Page page;
            EXPECT_TRUE(pager_.ReadPage(current, page).ok());
            NodeView node(page);
            if (node.IsLeaf()) {
                break;
            }
            current = node.NumKeys() > 0 ? node.GetChild(0)
                                         : node.GetRightmostChild();
        }
        // Walk the chain.
        while (current != kInvalidPageId) {
            storage::Page page;
            EXPECT_TRUE(pager_.ReadPage(current, page).ok());
            NodeView node(page);
            auto& keys = groups.emplace_back();
            for (uint16_t i = 0; i < node.NumKeys(); ++i) {
                keys.emplace_back(node.GetKey(i));
            }
            current = node.GetNextLeaf();
        }
        return groups;
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

TEST_F(CursorTest, SeekToFirstOnEmptyTreeIsInvalid) {
    BPlusTree tree(pool_);
    Cursor cursor = tree.SeekToFirst();
    EXPECT_FALSE(cursor.Valid());
    EXPECT_TRUE(cursor.status().ok());  // Empty is not an error
}

TEST_F(CursorTest, FullScanVisitsAllKeysInSortedOrder) {
    BPlusTree tree(pool_);
    constexpr int kCount = 200;  // Spans multiple leaves
    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(tree.Put(Key(i), Value(i)).ok()) << Key(i);
    }

    int expected = 0;
    for (Cursor cursor = tree.SeekToFirst(); cursor.Valid(); cursor.Next()) {
        ASSERT_LT(expected, kCount);
        EXPECT_EQ(cursor.key(), Key(expected));
        EXPECT_EQ(cursor.value(), Value(expected));
        ++expected;
    }
    EXPECT_EQ(expected, kCount);
}

TEST_F(CursorTest, SeekLandsOnLowerBound) {
    BPlusTree tree(pool_);
    for (int i = 0; i < 100; i += 2) {  // Even keys only
        ASSERT_TRUE(tree.Put(Key(i), Value(i)).ok()) << Key(i);
    }

    Cursor exact = tree.Seek(Key(42));
    ASSERT_TRUE(exact.Valid());
    EXPECT_EQ(exact.key(), Key(42));

    Cursor between = tree.Seek(Key(43));  // Absent; next is 44
    ASSERT_TRUE(between.Valid());
    EXPECT_EQ(between.key(), Key(44));

    Cursor before = tree.Seek("aaa");  // Before all keys
    ASSERT_TRUE(before.Valid());
    EXPECT_EQ(before.key(), Key(0));

    Cursor past = tree.Seek(Key(99));  // Last key is 98
    EXPECT_FALSE(past.Valid());
    EXPECT_TRUE(past.status().ok());
}

TEST_F(CursorTest, ScanSkipsDeletedKeys) {
    BPlusTree tree(pool_);
    constexpr int kCount = 100;
    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(tree.Put(Key(i), Value(i)).ok()) << Key(i);
    }
    for (int i = 0; i < kCount; i += 3) {
        ASSERT_TRUE(tree.Delete(Key(i)).ok()) << Key(i);
    }

    std::vector<std::string> seen;
    for (Cursor cursor = tree.SeekToFirst(); cursor.Valid(); cursor.Next()) {
        seen.emplace_back(cursor.key());
    }

    std::vector<std::string> expected;
    for (int i = 0; i < kCount; ++i) {
        if (i % 3 != 0) {
            expected.push_back(Key(i));
        }
    }
    EXPECT_EQ(seen, expected);
}

// Deleting every key of an interior leaf leaves an empty page in the chain
// (decision 2.8). Both a full scan and a Seek into the emptied range must
// hop over it.
TEST_F(CursorTest, ScanAndSeekSkipEmptiedLeaves) {
    BPlusTree tree(pool_);
    constexpr int kCount = 200;
    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(tree.Put(Key(i), Value(i)).ok()) << Key(i);
    }

    auto groups = LeafKeyGroups();
    ASSERT_GE(groups.size(), 3u) << "test needs >= 3 leaves";

    // Empty out the second leaf entirely.
    const std::vector<std::string> doomed = groups[1];
    ASSERT_FALSE(doomed.empty());
    for (const auto& key : doomed) {
        ASSERT_TRUE(tree.Delete(key).ok()) << key;
    }

    // Full scan sees everything except the emptied leaf's keys, in order.
    std::vector<std::string> expected;
    for (size_t g = 0; g < groups.size(); ++g) {
        if (g == 1) {
            continue;
        }
        expected.insert(expected.end(), groups[g].begin(), groups[g].end());
    }
    std::vector<std::string> seen;
    for (Cursor cursor = tree.SeekToFirst(); cursor.Valid(); cursor.Next()) {
        seen.emplace_back(cursor.key());
    }
    EXPECT_EQ(seen, expected);

    // Seeking to the emptied leaf's first key must land on the first key of
    // the next non-empty leaf.
    Cursor hop = tree.Seek(doomed.front());
    ASSERT_TRUE(hop.Valid());
    EXPECT_EQ(hop.key(), groups[2].front());
}

}  // namespace
}  // namespace kvstore::btree
