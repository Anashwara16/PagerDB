// ==============================================================================
// tests/page_test.cc - Slotted page smoke tests
// ==============================================================================
//
// Phase 0 smoke tests for Page / PageHeader / SlotEntry: layout asserts,
// Init state, insert/read round-trip, free-space accounting, and the
// tombstone flag on DeleteRecord. Phase 1 (task 1.17) extends this file with
// byte-exact accounting, boundary sizes, and Validate() coverage.
//
// ==============================================================================

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "kvstore/storage/page_format.h"

namespace kvstore::storage {
namespace {

TEST(PageFormatTest, LayoutConstantsHold) {
    static_assert(sizeof(PageHeader) == 40);
    static_assert(sizeof(SlotEntry) == 8);
    static_assert(sizeof(Page) == kPageSize);
    EXPECT_EQ(kPageSize, 4096u);
}

TEST(PageTest, InitProducesEmptyPageWithFullFreeSpace) {
    Page page;
    page.Init(7, PageType::kLeaf);

    EXPECT_EQ(page.page_id(), 7u);
    EXPECT_EQ(page.type(), PageType::kLeaf);
    EXPECT_EQ(page.num_slots(), 0u);
    EXPECT_EQ(page.header()->free_space_start, sizeof(PageHeader));
    EXPECT_EQ(page.header()->free_space_end, kPageSize);
    EXPECT_EQ(page.FreeSpace(), kPageSize - sizeof(PageHeader));
    EXPECT_EQ(page.header()->next_page, kInvalidPageId);
    EXPECT_EQ(page.header()->prev_page, kInvalidPageId);
}

TEST(PageTest, InsertRecordRoundTrip) {
    Page page;
    page.Init(1, PageType::kLeaf);

    const std::string_view record = "key=value";
    auto slot = page.InsertRecord(record.data(), static_cast<uint16_t>(record.size()));
    ASSERT_TRUE(slot.has_value());
    EXPECT_EQ(*slot, 0u);
    EXPECT_EQ(page.num_slots(), 1u);
    EXPECT_EQ(page.GetRecordView(*slot), record);
}

TEST(PageTest, InsertConsumesSlotPlusRecordBytes) {
    Page page;
    page.Init(1, PageType::kLeaf);
    const uint16_t before = page.FreeSpace();

    const std::string_view record = "0123456789";  // 10 bytes
    ASSERT_TRUE(page.InsertRecord(record.data(), static_cast<uint16_t>(record.size())).has_value());

    EXPECT_EQ(page.FreeSpace(), before - sizeof(SlotEntry) - record.size());
}

TEST(PageTest, InsertFailsWithNoSpaceWhenFull) {
    Page page;
    page.Init(1, PageType::kLeaf);

    const std::string record(512, 'a');
    // Fill the page until CanFit says no.
    while (page.CanFit(static_cast<uint16_t>(record.size()))) {
        ASSERT_TRUE(
            page.InsertRecord(record.data(), static_cast<uint16_t>(record.size())).has_value());
    }

    auto overflow = page.InsertRecord(record.data(), static_cast<uint16_t>(record.size()));
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error().code(), StatusCode::kNoSpace);
}

TEST(PageTest, DeleteRecordMarksSlotDeleted) {
    Page page;
    page.Init(1, PageType::kLeaf);

    const std::string_view record = "doomed";
    auto slot = page.InsertRecord(record.data(), static_cast<uint16_t>(record.size()));
    ASSERT_TRUE(slot.has_value());

    ASSERT_TRUE(page.DeleteRecord(*slot).has_value());
    EXPECT_FALSE(page.slot(*slot).IsValid());
    EXPECT_EQ(page.GetRecordPtr(*slot), nullptr);   // Tombstoned records are unreadable
    EXPECT_EQ(page.num_slots(), 1u);                // Slot itself remains until compaction

    // Deleting again reports NotFound.
    auto again = page.DeleteRecord(*slot);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code(), StatusCode::kNotFound);
}

}  // namespace
}  // namespace kvstore::storage
