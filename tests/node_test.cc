// ==============================================================================
// tests/node_test.cc - NodeView (B+ tree node encoding) tests
// ==============================================================================
//
// Phase 1 task 1.19: sorted insert order, FindKeyIndex edge cases, record
// round-trips for leaf and internal nodes, update-in-place variants, and
// format-v2 rightmost_child handling (which must NOT alias next_page).
//
// ==============================================================================

#include <gtest/gtest.h>

#include <format>
#include <string>
#include <vector>

#include "kvstore/btree/node.h"
#include "kvstore/storage/page_format.h"

namespace kvstore::btree {
namespace {

using storage::Page;
using storage::PageType;

// -----------------------------------------------------------------------------
// Format v2: internal nodes keep rightmost_child separate from next_page
// -----------------------------------------------------------------------------

TEST(NodeViewInternalTest, RightmostChildDoesNotAliasNextPage) {
    Page page;
    page.Init(5, PageType::kInternal);
    NodeView node(page);
    node.InitInternal();

    node.SetRightmostChild(42);

    EXPECT_EQ(node.GetRightmostChild(), 42u);
    // v2 invariant: the leaf-chain field must stay untouched on internal nodes.
    EXPECT_EQ(page.header()->next_page, kInvalidPageId);
}

TEST(NodeViewInternalTest, HeaderInitZeroesDeadBytesAndRightmostChild) {
    Page page;
    page.Init(9, PageType::kInternal);

    EXPECT_EQ(page.header()->dead_bytes, 0u);
    EXPECT_EQ(page.header()->rightmost_child, kInvalidPageId);
}

// -----------------------------------------------------------------------------
// Leaf nodes: sorted order, lookup edges, updates
// -----------------------------------------------------------------------------

TEST(NodeViewLeafTest, InsertsKeepKeysSorted) {
    Page page;
    page.Init(1, PageType::kLeaf);
    NodeView node(page);
    node.InitLeaf();

    for (std::string_view key : {"melon", "apple", "zebra", "kiwi", "banana"}) {
        ASSERT_TRUE(node.InsertLeaf(key, "v"));
    }

    ASSERT_EQ(node.NumKeys(), 5u);
    for (uint16_t i = 1; i < node.NumKeys(); ++i) {
        EXPECT_LT(node.GetKey(static_cast<uint16_t>(i - 1)), node.GetKey(i));
    }
}

TEST(NodeViewLeafTest, FindKeyIndexEdgeCases) {
    Page page;
    page.Init(1, PageType::kLeaf);
    NodeView node(page);
    node.InitLeaf();

    bool found = true;
    EXPECT_EQ(node.FindKeyIndex("anything", found), 0u);  // Empty node
    EXPECT_FALSE(found);

    ASSERT_TRUE(node.InsertLeaf("bb", "1"));
    ASSERT_TRUE(node.InsertLeaf("dd", "2"));
    ASSERT_TRUE(node.InsertLeaf("ff", "3"));

    EXPECT_EQ(node.FindKeyIndex("bb", found), 0u);  // First
    EXPECT_TRUE(found);
    EXPECT_EQ(node.FindKeyIndex("ff", found), 2u);  // Last
    EXPECT_TRUE(found);
    EXPECT_EQ(node.FindKeyIndex("aa", found), 0u);  // Before all
    EXPECT_FALSE(found);
    EXPECT_EQ(node.FindKeyIndex("cc", found), 1u);  // Between
    EXPECT_FALSE(found);
    EXPECT_EQ(node.FindKeyIndex("zz", found), 3u);  // After all
    EXPECT_FALSE(found);
}

TEST(NodeViewLeafTest, ValueRoundTripIncludingEmptyValue) {
    Page page;
    page.Init(1, PageType::kLeaf);
    NodeView node(page);
    node.InitLeaf();

    ASSERT_TRUE(node.InsertLeaf("empty", ""));
    ASSERT_TRUE(node.InsertLeaf("full", "some-value"));

    EXPECT_EQ(node.GetValue("empty").value(), "");
    EXPECT_EQ(node.GetValue("full").value(), "some-value");
    EXPECT_FALSE(node.GetValue("missing").has_value());
}

TEST(NodeViewLeafTest, UpdateLeafHandlesSameSmallerAndLargerValues) {
    Page page;
    page.Init(1, PageType::kLeaf);
    NodeView node(page);
    node.InitLeaf();

    ASSERT_TRUE(node.InsertLeaf("alpha", "aaa"));
    ASSERT_TRUE(node.InsertLeaf("beta", "bbb"));

    ASSERT_TRUE(node.UpdateLeaf("alpha", "xxx"));           // Same size
    EXPECT_EQ(node.GetValue("alpha").value(), "xxx");
    ASSERT_TRUE(node.UpdateLeaf("alpha", "y"));             // Smaller
    EXPECT_EQ(node.GetValue("alpha").value(), "y");
    ASSERT_TRUE(node.UpdateLeaf("alpha", "much-longer-value"));  // Larger
    EXPECT_EQ(node.GetValue("alpha").value(), "much-longer-value");

    // Untouched neighbor survives all rebuilds.
    EXPECT_EQ(node.GetValue("beta").value(), "bbb");
    EXPECT_FALSE(node.UpdateLeaf("missing", "nope"));
}

// The in-place UpdateLeaf's compaction path: fill the page nearly full, then
// grow one record when contiguous free space is short but dead bytes (from
// earlier updates) make the total sufficient. Also churns grow/shrink cycles
// to prove dead-byte accounting reclaims - a leak would make updates start
// failing on a page that logically has room.
TEST(NodeViewLeafTest, UpdateLeafGrowsViaCompactionAndChurnDoesNotLeak) {
    Page page;
    page.Init(1, PageType::kLeaf);
    NodeView node(page);
    node.InitLeaf();

    // ~14 records x ~260 bytes fills most of the 4 KB page.
    constexpr int kRecords = 14;
    for (int i = 0; i < kRecords; ++i) {
        ASSERT_TRUE(node.InsertLeaf("key" + std::to_string(i),
                                    std::string(250, 'a')));
    }

    // Shrink several records: page now has plenty of DEAD bytes but little
    // contiguous free space for a large grow.
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(node.UpdateLeaf("key" + std::to_string(i),
                                    std::string(10, 'b')));
    }

    // Grow one record past the contiguous free space - must compact and fit.
    ASSERT_TRUE(node.UpdateLeaf("key0", std::string(900, 'c')))
        << "grow-under-fragmentation failed: compaction path broken";
    EXPECT_EQ(node.GetValue("key0").value(), std::string(900, 'c'));

    // Churn: repeated grow/shrink cycles on one key. If dead bytes leaked,
    // free space would shrink monotonically and some cycle would fail.
    for (int cycle = 0; cycle < 200; ++cycle) {
        ASSERT_TRUE(node.UpdateLeaf("key1", std::string(400, 'd')))
            << "cycle " << cycle;
        ASSERT_TRUE(node.UpdateLeaf("key1", std::string(20, 'e')))
            << "cycle " << cycle;
    }

    // Every other record is intact after all the shuffling.
    EXPECT_EQ(node.GetValue("key0").value(), std::string(900, 'c'));
    for (int i = 2; i < 8; ++i) {
        EXPECT_EQ(node.GetValue("key" + std::to_string(i)).value(),
                  std::string(10, 'b'));
    }
    for (int i = 8; i < kRecords; ++i) {
        EXPECT_EQ(node.GetValue("key" + std::to_string(i)).value(),
                  std::string(250, 'a'));
    }

    // A grow that cannot fit even after compaction must be refused (the
    // caller's split path handles it), leaving the record untouched.
    EXPECT_FALSE(node.UpdateLeaf("key1", std::string(4000, 'f')));
    EXPECT_EQ(node.GetValue("key1").value(), std::string(20, 'e'));
}

// -----------------------------------------------------------------------------
// Phase 2 (task 2.1): DeleteAt - slot removal with dead-byte accounting
// -----------------------------------------------------------------------------

TEST(NodeViewLeafTest, DeleteAtShiftsSlotsAndCountsDeadBytes) {
    Page page;
    page.Init(1, PageType::kLeaf);
    NodeView node(page);
    node.InitLeaf();

    ASSERT_TRUE(node.InsertLeaf("aa", "v1"));
    ASSERT_TRUE(node.InsertLeaf("bb", "v2"));
    ASSERT_TRUE(node.InsertLeaf("cc", "v3"));

    const uint16_t record_len = page.GetRecordLength(1);  // "bb" record
    const uint16_t free_before = page.FreeSpace();

    ASSERT_TRUE(node.DeleteAt(1));

    // Remaining keys shift down and stay sorted; the deleted key is gone.
    ASSERT_EQ(node.NumKeys(), 2u);
    EXPECT_EQ(node.GetKey(0), "aa");
    EXPECT_EQ(node.GetKey(1), "cc");
    EXPECT_EQ(node.GetValue("cc").value(), "v3");
    EXPECT_FALSE(node.GetValue("bb").has_value());

    // Slot space is reclaimed immediately (directory shrinks); the record
    // bytes stay behind as dead bytes until compaction.
    EXPECT_EQ(page.header()->dead_bytes, record_len);
    EXPECT_EQ(page.FreeSpace(),
              static_cast<uint16_t>(free_before + sizeof(storage::SlotEntry)));
    EXPECT_TRUE(page.Validate());
}

TEST(NodeViewLeafTest, DeleteAtRejectsOutOfRangeIndex) {
    Page page;
    page.Init(1, PageType::kLeaf);
    NodeView node(page);
    node.InitLeaf();

    EXPECT_FALSE(node.DeleteAt(0));  // Empty node

    ASSERT_TRUE(node.InsertLeaf("aa", "v1"));
    EXPECT_FALSE(node.DeleteAt(1));  // One past the end
    EXPECT_EQ(node.NumKeys(), 1u);
}

TEST(NodeViewLeafTest, DeleteAllThenReinsertWorks) {
    Page page;
    page.Init(1, PageType::kLeaf);
    NodeView node(page);
    node.InitLeaf();

    ASSERT_TRUE(node.InsertLeaf("aa", "v1"));
    ASSERT_TRUE(node.InsertLeaf("bb", "v2"));
    ASSERT_TRUE(node.DeleteAt(0));
    ASSERT_TRUE(node.DeleteAt(0));
    EXPECT_TRUE(node.IsEmpty());

    ASSERT_TRUE(node.InsertLeaf("cc", "v3"));
    EXPECT_EQ(node.GetValue("cc").value(), "v3");
    // Phase 6: the in-place slot-shift insert does NOT rebuild the page, so
    // the two deleted records' bytes stay counted until a compaction needs
    // them (see InsertReclaimsDeadBytesWhenFreeSpaceIsInsufficient below).
    EXPECT_EQ(page.header()->dead_bytes, 2 * (2 + 2 + 2 + 2));
    page.Compact();
    EXPECT_EQ(page.header()->dead_bytes, 0u);
    EXPECT_EQ(node.GetValue("cc").value(), "v3");
    EXPECT_TRUE(page.Validate());
}

// Phase 2 (task 2.5): when contiguous free space is too small but dead bytes
// would cover the shortfall, the insert must compact the page and succeed.
TEST(NodeViewLeafTest, InsertReclaimsDeadBytesWhenFreeSpaceIsInsufficient) {
    Page page;
    page.Init(1, PageType::kLeaf);
    NodeView node(page);
    node.InitLeaf();

    // 500-byte values: each entry costs 515 bytes, so 7 fit and ~451 remain.
    const std::string big(500, 'x');
    int count = 0;
    while (node.InsertLeaf(std::format("k{:02d}", count), big)) {
        ++count;
    }
    ASSERT_GE(count, 5);
    ASSERT_FALSE(node.InsertLeaf("zz", big));  // Genuinely full

    // Deleting one entry frees only 8 slot bytes of contiguous space; the
    // ~507 dead record bytes are what make the next insert possible.
    ASSERT_TRUE(node.DeleteAt(0));
    ASSERT_LT(page.FreeSpace(), 500u);

    EXPECT_TRUE(node.InsertLeaf("zz", big));
    EXPECT_EQ(node.GetValue("zz").value(), big);
    EXPECT_EQ(page.header()->dead_bytes, 0u);  // Reclaimed by the rebuild
    EXPECT_TRUE(page.Validate());
}

// -----------------------------------------------------------------------------
// Internal nodes: child descent
// -----------------------------------------------------------------------------

TEST(NodeViewInternalTest, InsertInternalRoundTripAndDescent) {
    Page page;
    page.Init(1, PageType::kInternal);
    NodeView node(page);
    node.InitInternal();

    // children: [10] < "gg" <= [20] < "nn" <= [30]
    ASSERT_TRUE(node.InsertInternal("gg", 10));
    ASSERT_TRUE(node.InsertInternal("nn", 20));
    node.SetRightmostChild(30);

    EXPECT_EQ(node.GetChild(0), 10u);
    EXPECT_EQ(node.GetChild(1), 20u);

    EXPECT_EQ(node.FindChild("aa"), 10u);   // Below first separator
    EXPECT_EQ(node.FindChild("gg"), 20u);   // Equal descends RIGHT
    EXPECT_EQ(node.FindChild("hh"), 20u);   // Between separators
    EXPECT_EQ(node.FindChild("nn"), 30u);   // Equal to last -> rightmost
    EXPECT_EQ(node.FindChild("zz"), 30u);   // Above all

    EXPECT_EQ(node.FindChildIndex("aa"), 0u);
    EXPECT_EQ(node.FindChildIndex("gg"), 1u);
    EXPECT_EQ(node.FindChildIndex("nn"), 2u);  // NumKeys() == rightmost
}

}  // namespace
}  // namespace kvstore::btree
