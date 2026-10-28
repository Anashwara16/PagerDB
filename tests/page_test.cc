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

#include <sstream>
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

// -----------------------------------------------------------------------------
// Validate (Phase 1 task 1.13)
// -----------------------------------------------------------------------------

TEST(PageValidateTest, AcceptsFreshAndPopulatedPages) {
    Page page;
    page.Init(1, PageType::kLeaf);
    EXPECT_TRUE(page.Validate());

    for (std::string_view record : {"one", "twotwo", "threethree"}) {
        ASSERT_TRUE(page.InsertRecord(record.data(),
                                      static_cast<uint16_t>(record.size())).has_value());
    }
    EXPECT_TRUE(page.Validate());
}

TEST(PageValidateTest, RejectsSlotDirectoryMismatch) {
    Page page;
    page.Init(1, PageType::kLeaf);
    const std::string_view record = "payload";
    ASSERT_TRUE(page.InsertRecord(record.data(),
                                  static_cast<uint16_t>(record.size())).has_value());

    // free_space_start must equal header + num_slots * slot size.
    page.header()->free_space_start += sizeof(SlotEntry);
    EXPECT_FALSE(page.Validate());
}

TEST(PageValidateTest, RejectsCrossedFreeSpacePointers) {
    Page page;
    page.Init(1, PageType::kLeaf);
    page.header()->free_space_end = sizeof(PageHeader) - 8;  // Before slot area
    EXPECT_FALSE(page.Validate());
}

TEST(PageValidateTest, RejectsSlotPointingOutsideRecordArea) {
    Page page;
    page.Init(1, PageType::kLeaf);
    const std::string_view record = "payload";
    ASSERT_TRUE(page.InsertRecord(record.data(),
                                  static_cast<uint16_t>(record.size())).has_value());

    // Point the slot into the header area.
    page.slot(0).offset = 10;
    EXPECT_FALSE(page.Validate());
}

TEST(PageValidateTest, RejectsOverlappingRecords) {
    Page page;
    page.Init(1, PageType::kLeaf);
    const std::string_view record = "0123456789";
    ASSERT_TRUE(page.InsertRecord(record.data(), 10).has_value());
    ASSERT_TRUE(page.InsertRecord(record.data(), 10).has_value());

    // Make slot 1 overlap slot 0's record bytes.
    page.slot(1).offset = page.slot(0).offset;
    EXPECT_FALSE(page.Validate());
}

// -----------------------------------------------------------------------------
// Dump (Phase 1 task 1.14)
// -----------------------------------------------------------------------------

TEST(PageDumpTest, WritesHeaderAndSlotSummary) {
    Page page;
    page.Init(42, PageType::kLeaf);
    const std::string_view record = "key=value";
    ASSERT_TRUE(page.InsertRecord(record.data(),
                                  static_cast<uint16_t>(record.size())).has_value());

    std::ostringstream out;
    page.Dump(out);
    const std::string text = out.str();

    EXPECT_NE(text.find("page_id=42"), std::string::npos) << text;
    EXPECT_NE(text.find("type=leaf"), std::string::npos) << text;
    EXPECT_NE(text.find("num_slots=1"), std::string::npos) << text;
    EXPECT_NE(text.find("slot[0]"), std::string::npos) << text;
}

// -----------------------------------------------------------------------------
// Phase 2 (task 2.4): in-page compaction
// -----------------------------------------------------------------------------

TEST(PageCompactTest, DeleteRecordCountsDeadBytes) {
    Page page;
    page.Init(7, PageType::kLeaf);
    auto slot = page.InsertRecord("alpha", 5).value();

    ASSERT_TRUE(page.DeleteRecord(slot).has_value());
    EXPECT_EQ(page.header()->dead_bytes, 5u);
}

TEST(PageCompactTest, CompactPacksLiveRecordsAndZeroesDeadBytes) {
    Page page;
    page.Init(7, PageType::kLeaf);
    const SlotId s0 = page.InsertRecord("alpha", 5).value();
    const SlotId s1 = page.InsertRecord("beta!", 5).value();
    const SlotId s2 = page.InsertRecord("gamma", 5).value();

    ASSERT_TRUE(page.DeleteRecord(s1).has_value());
    const uint16_t free_before = page.FreeSpace();

    page.Compact();

    // Live records survive with their slot IDs; the dead record's bytes are gone.
    EXPECT_EQ(page.GetRecordView(s0), "alpha");
    EXPECT_EQ(page.GetRecordView(s2), "gamma");
    EXPECT_EQ(page.GetRecordView(s1), "");
    EXPECT_EQ(page.header()->dead_bytes, 0u);
    EXPECT_EQ(page.FreeSpace(), free_before + 5);
    EXPECT_EQ(page.header()->free_space_end,
              static_cast<uint16_t>(kPageSize - 10));
    EXPECT_TRUE(page.Validate());
}

TEST(PageCompactTest, CompactOnCleanPageIsANoOp) {
    Page page;
    page.Init(7, PageType::kLeaf);
    const SlotId s0 = page.InsertRecord("alpha", 5).value();
    const uint16_t free_before = page.FreeSpace();

    page.Compact();

    EXPECT_EQ(page.GetRecordView(s0), "alpha");
    EXPECT_EQ(page.FreeSpace(), free_before);
    EXPECT_TRUE(page.Validate());
}

}  // namespace
}  // namespace kvstore::storage
