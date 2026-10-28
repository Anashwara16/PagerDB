// ==============================================================================
// tests/pager_test.cc - Pager and on-disk format tests
// ==============================================================================
//
// Phase 1 (task 1.18): new-database meta initialization, reopen persistence,
// corruption detection (bad magic, wrong version, truncation, checksum flip),
// and page allocation behavior.
// Phase 2 (tasks 2.6/2.7): free list.
// Phase 4 (task 4.6, format v3): dual meta pages with generation counter,
// deferred meta persistence (Sync/checkpoint), data pages start at page 2.
//
// ==============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "kvstore/storage/file.h"
#include "kvstore/storage/pager.h"

namespace kvstore::storage {
namespace {

namespace fs = std::filesystem;

class PagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::path(::testing::TempDir()) / "kvstore_pager_test";
        fs::create_directories(dir_);
        db_path_ = (dir_ / "data.db").string();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // Overwrite `len` bytes at `offset` in the closed database file.
    void CorruptBytes(uint64_t offset, std::string_view bytes) {
        auto file = PosixFile::Open(db_path_, false).value();
        ASSERT_TRUE(file.Write(offset, bytes.data(), bytes.size()).has_value());
    }

    fs::path dir_;
    std::string db_path_;
};

// -----------------------------------------------------------------------------
// Format v3 meta pages
// -----------------------------------------------------------------------------

TEST_F(PagerTest, FormatVersionIsThree) {
    EXPECT_EQ(MetaPage::kFormatVersion, 3u);
}

TEST_F(PagerTest, NewDatabaseInitializesMetaWithTwoMetaPages) {
    auto pager = Pager::Open(db_path_, /*create_if_missing=*/true);
    ASSERT_TRUE(pager.has_value()) << pager.error().message();

    EXPECT_EQ(pager->root_page_id(), kInvalidPageId);
    EXPECT_EQ(pager->num_pages(), 2u);  // Meta copies at pages 0 and 1
    EXPECT_EQ(pager->num_keys(), 0u);
    EXPECT_EQ(pager->free_list_head(), kInvalidPageId);
    EXPECT_EQ(pager->generation(), 0u);
    EXPECT_EQ(pager->checkpoint_lsn(), kInvalidLsn);

    // Both meta copies exist on disk.
    auto file = PosixFile::Open(db_path_, false).value();
    EXPECT_EQ(file.Size().value(), 2u * kPageSize);
}

// v3: meta setters are in-memory; Sync() (or a checkpoint) persists them.
TEST_F(PagerTest, MetaPersistsOnSyncNotOnEverySetter) {
    {
        auto pager = Pager::Open(db_path_, true).value();
        ASSERT_TRUE(pager.AllocatePage().has_value());       // page 2
        ASSERT_TRUE(pager.SetRootPageId(2).ok());
        ASSERT_TRUE(pager.SetNumKeys(7).ok());
        // No Sync: nothing about the meta reached disk yet.
    }
    {
        auto reopened = Pager::Open(db_path_, false).value();
        EXPECT_EQ(reopened.root_page_id(), kInvalidPageId);
        EXPECT_EQ(reopened.num_pages(), 2u);
        EXPECT_EQ(reopened.num_keys(), 0u);
    }
}

TEST_F(PagerTest, ReopenPreservesMetaStateAfterSync) {
    {
        auto pager = Pager::Open(db_path_, true).value();
        ASSERT_TRUE(pager.AllocatePage().has_value());       // page 2
        ASSERT_TRUE(pager.SetRootPageId(2).ok());
        ASSERT_TRUE(pager.SetNumKeys(7).ok());
        ASSERT_TRUE(pager.Sync().ok());
    }
    auto reopened = Pager::Open(db_path_, false);
    ASSERT_TRUE(reopened.has_value()) << reopened.error().message();
    EXPECT_EQ(reopened->root_page_id(), 2u);
    EXPECT_EQ(reopened->num_pages(), 3u);
    EXPECT_EQ(reopened->num_keys(), 7u);
    EXPECT_GE(reopened->generation(), 1u);  // Sync bumped the generation
}

TEST_F(PagerTest, AllocatePageReturnsSequentialIdsFromPageTwo) {
    {
        auto pager = Pager::Open(db_path_, true).value();
        EXPECT_EQ(pager.AllocatePage().value(), 2u);
        EXPECT_EQ(pager.AllocatePage().value(), 3u);
        EXPECT_EQ(pager.AllocatePage().value(), 4u);
        EXPECT_EQ(pager.num_pages(), 5u);
        ASSERT_TRUE(pager.Sync().ok());  // v3: meta persists on Sync
    }
    auto reopened = Pager::Open(db_path_, false).value();
    EXPECT_EQ(reopened.num_pages(), 5u);
}

TEST_F(PagerTest, MetaCopiesAlternateWithGeneration) {
    auto pager = Pager::Open(db_path_, true).value();
    ASSERT_TRUE(pager.Sync().ok());  // generation 1 -> slot 1
    ASSERT_TRUE(pager.SetNumKeys(1).ok());
    ASSERT_TRUE(pager.Sync().ok());  // generation 2 -> slot 0
    EXPECT_EQ(pager.generation(), 2u);

    // Reopen picks the copy with the higher generation.
    auto reopened = Pager::Open(db_path_, false).value();
    EXPECT_EQ(reopened.generation(), 2u);
    EXPECT_EQ(reopened.num_keys(), 1u);
}

// -----------------------------------------------------------------------------
// Dual-meta resilience (task 4.6): either copy may die; open uses the other
// -----------------------------------------------------------------------------

TEST_F(PagerTest, OpenSurvivesCorruptionOfMetaCopyZero) {
    {
        auto pager = Pager::Open(db_path_, true).value();
        ASSERT_TRUE(pager.AllocatePage().has_value());
        ASSERT_TRUE(pager.SetRootPageId(2).ok());
        ASSERT_TRUE(pager.SetNumKeys(5).ok());
        ASSERT_TRUE(pager.Sync().ok());  // generation 1 lives in slot 1
    }
    CorruptBytes(100, std::string_view("garbage!", 8));  // Wreck copy 0

    auto reopened = Pager::Open(db_path_, false);
    ASSERT_TRUE(reopened.has_value()) << reopened.error().message();
    EXPECT_EQ(reopened->root_page_id(), 2u);
    EXPECT_EQ(reopened->num_keys(), 5u);
}

TEST_F(PagerTest, OpenSurvivesCorruptionOfMetaCopyOne) {
    {
        auto pager = Pager::Open(db_path_, true).value();
        ASSERT_TRUE(pager.AllocatePage().has_value());
        ASSERT_TRUE(pager.SetRootPageId(2).ok());
        ASSERT_TRUE(pager.SetNumKeys(5).ok());
        ASSERT_TRUE(pager.Sync().ok());   // generation 1 -> slot 1
        ASSERT_TRUE(pager.SetNumKeys(6).ok());
        ASSERT_TRUE(pager.Sync().ok());   // generation 2 -> slot 0
    }
    CorruptBytes(kPageSize + 100, std::string_view("garbage!", 8));  // Wreck copy 1

    auto reopened = Pager::Open(db_path_, false);
    ASSERT_TRUE(reopened.has_value()) << reopened.error().message();
    // Copy 0 carries generation 2 - the freshest surviving state.
    EXPECT_EQ(reopened->num_keys(), 6u);
}

TEST_F(PagerTest, OpenFailsWhenBothMetaCopiesAreCorrupt) {
    { auto pager = Pager::Open(db_path_, true).value(); }
    CorruptBytes(0, std::string_view("XXXX", 4));             // Magic, copy 0
    CorruptBytes(kPageSize, std::string_view("XXXX", 4));     // Magic, copy 1

    auto reopened = Pager::Open(db_path_, false);
    ASSERT_FALSE(reopened.has_value());
    EXPECT_EQ(reopened.error().code(), StatusCode::kCorruption);
}

TEST_F(PagerTest, UnsupportedVersionInBothCopiesIsDetected) {
    { auto pager = Pager::Open(db_path_, true).value(); }
    const uint32_t bogus_version = 99;
    const std::string_view bytes(reinterpret_cast<const char*>(&bogus_version), 4);
    CorruptBytes(4, bytes);              // Version field, copy 0
    CorruptBytes(kPageSize + 4, bytes);  // Version field, copy 1

    auto reopened = Pager::Open(db_path_, false);
    ASSERT_FALSE(reopened.has_value());
    EXPECT_EQ(reopened.error().code(), StatusCode::kCorruption);
}

// -----------------------------------------------------------------------------
// Corruption detection (data pages, truncation)
// -----------------------------------------------------------------------------

TEST_F(PagerTest, TruncatedFileIsDetected) {
    {
        auto pager = Pager::Open(db_path_, true).value();
        ASSERT_TRUE(pager.AllocatePage().has_value());  // page 2, num_pages = 3
        ASSERT_TRUE(pager.Sync().ok());
    }
    {
        auto file = PosixFile::Open(db_path_, false).value();
        ASSERT_TRUE(file.Truncate(2 * kPageSize + 100).has_value());
    }
    auto reopened = Pager::Open(db_path_, false);
    ASSERT_FALSE(reopened.has_value());
    EXPECT_EQ(reopened.error().code(), StatusCode::kCorruption);
}

TEST_F(PagerTest, FlippedByteInDataPageIsDetectedOnRead) {
    {
        auto pager = Pager::Open(db_path_, true).value();
        ASSERT_TRUE(pager.AllocatePage().has_value());  // page 2

        Page page;
        page.Init(2, PageType::kLeaf);
        const std::string_view record = "canary-record";
        ASSERT_TRUE(page.InsertRecord(record.data(),
                                      static_cast<uint16_t>(record.size())).has_value());
        ASSERT_TRUE(pager.WritePage(2, page).ok());
        ASSERT_TRUE(pager.Sync().ok());
    }
    // Flip one byte inside page 2's record area (last bytes of the page).
    CorruptBytes(2 * kPageSize + kPageSize - 5, std::string_view("!", 1));

    auto reopened = Pager::Open(db_path_, false).value();
    Page page;
    Status status = reopened.ReadPage(2, page);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kCorruption);
}

TEST_F(PagerTest, VerificationCanBeDisabled) {
    {
        auto pager = Pager::Open(db_path_, true).value();
        ASSERT_TRUE(pager.AllocatePage().has_value());  // page 2

        Page page;
        page.Init(2, PageType::kLeaf);
        ASSERT_TRUE(pager.WritePage(2, page).ok());
        ASSERT_TRUE(pager.Sync().ok());
    }
    CorruptBytes(2 * kPageSize + kPageSize - 5, std::string_view("!", 1));

    auto reopened = Pager::Open(db_path_, false, /*verify_checksums=*/false).value();
    Page page;
    EXPECT_TRUE(reopened.ReadPage(2, page).ok());
}

TEST_F(PagerTest, FreshlyAllocatedZeroPageReadsCleanly) {
    // AllocatePage writes an all-zero page; reading it back must not be
    // reported as corruption (it is simply uninitialized).
    auto pager = Pager::Open(db_path_, true).value();
    ASSERT_TRUE(pager.AllocatePage().has_value());  // page 2

    Page page;
    EXPECT_TRUE(pager.ReadPage(2, page).ok());
}

TEST_F(PagerTest, MetaPagesAreNotReadableAsDataPages) {
    auto pager = Pager::Open(db_path_, true).value();
    Page page;
    EXPECT_EQ(pager.ReadPage(0, page).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(pager.ReadPage(1, page).code(), StatusCode::kInvalidArgument);
    page.Init(2, PageType::kLeaf);
    EXPECT_EQ(pager.WritePage(0, page).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(pager.WritePage(1, page).code(), StatusCode::kInvalidArgument);
}

// -----------------------------------------------------------------------------
// Phase 2 (tasks 2.6/2.7): free list
// -----------------------------------------------------------------------------

TEST_F(PagerTest, FreePageThenAllocateReusesIt) {
    auto pager = Pager::Open(db_path_, true).value();
    ASSERT_EQ(pager.AllocatePage().value(), 2u);
    ASSERT_EQ(pager.AllocatePage().value(), 3u);
    ASSERT_EQ(pager.AllocatePage().value(), 4u);

    ASSERT_TRUE(pager.FreePage(3).ok());
    EXPECT_EQ(pager.free_list_head(), 3u);

    // A freed page is typed kFree on disk and readable (checksum stamped).
    Page free_page;
    ASSERT_TRUE(pager.ReadPage(3, free_page).ok());
    EXPECT_EQ(free_page.type(), PageType::kFree);

    // Allocation pops the free list instead of growing the file.
    const PageId pages_before = pager.num_pages();
    EXPECT_EQ(pager.AllocatePage().value(), 3u);
    EXPECT_EQ(pager.num_pages(), pages_before);
    EXPECT_EQ(pager.free_list_head(), kInvalidPageId);

    // The recycled page reads back all-zero, like any fresh allocation.
    Page recycled;
    ASSERT_TRUE(pager.ReadPage(3, recycled).ok());
    EXPECT_TRUE(recycled.IsZeroed());

    // With the free list empty, allocation extends the file again.
    EXPECT_EQ(pager.AllocatePage().value(), pages_before);
}

TEST_F(PagerTest, FreeListIsLifoAndSurvivesReopen) {
    {
        auto pager = Pager::Open(db_path_, true).value();
        ASSERT_EQ(pager.AllocatePage().value(), 2u);
        ASSERT_EQ(pager.AllocatePage().value(), 3u);
        ASSERT_EQ(pager.AllocatePage().value(), 4u);
        ASSERT_TRUE(pager.FreePage(2).ok());
        ASSERT_TRUE(pager.FreePage(4).ok());
        ASSERT_TRUE(pager.Sync().ok());
    }

    auto pager = Pager::Open(db_path_, false).value();
    EXPECT_EQ(pager.free_list_head(), 4u);
    EXPECT_EQ(pager.AllocatePage().value(), 4u);  // Most recently freed first
    EXPECT_EQ(pager.AllocatePage().value(), 2u);
    EXPECT_EQ(pager.free_list_head(), kInvalidPageId);
    EXPECT_EQ(pager.AllocatePage().value(), 5u);  // Extends after list drains
}

TEST_F(PagerTest, FreePageRejectsMetaPagesAndOutOfRangeIds) {
    auto pager = Pager::Open(db_path_, true).value();
    ASSERT_TRUE(pager.AllocatePage().has_value());  // page 2

    EXPECT_EQ(pager.FreePage(0).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(pager.FreePage(1).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(pager.FreePage(3).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(pager.free_list_head(), kInvalidPageId);
}

}  // namespace
}  // namespace kvstore::storage
