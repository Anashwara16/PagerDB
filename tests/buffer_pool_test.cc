// ==============================================================================
// tests/buffer_pool_test.cc - BufferPool / PageGuard / LRU tests
// ==============================================================================
//
// Phase 3 (tasks 3.1-3.3, 3.6-3.7): pin counting via RAII guards, LRU
// eviction of unpinned frames only, dirty write-back on eviction, Busy when
// every frame is pinned, and hit/miss/eviction statistics.
//
// ==============================================================================

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "kvstore/storage/buffer_pool.h"
#include "kvstore/storage/pager.h"

namespace kvstore::storage {
namespace {

namespace fs = std::filesystem;

class BufferPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::path(::testing::TempDir()) / "kvstore_buffer_pool_test";
        fs::create_directories(dir_);
        auto pager = Pager::Open((dir_ / "data.db").string(), true);
        ASSERT_TRUE(pager.has_value()) << pager.error().message();
        pager_ = std::move(*pager);
        // Pre-allocate pages 2..9 with a recognizable record in each
        // (v3: pages 0 and 1 are the meta copies).
        for (PageId id = 2; id <= 9; ++id) {
            ASSERT_TRUE(pager_.AllocatePage().has_value());
            Page page;
            page.Init(id, PageType::kLeaf);
            const std::string payload = "page-" + std::to_string(id);
            ASSERT_TRUE(page.InsertRecord(payload.data(),
                                          static_cast<uint16_t>(payload.size()))
                            .has_value());
            ASSERT_TRUE(pager_.WritePage(id, page).ok());
        }
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    fs::path dir_;
    Pager pager_;
};

TEST_F(BufferPoolTest, FetchSamePageTwiceSharesOneFrame) {
    BufferPool pool(pager_, 4);

    auto a = pool.FetchPage(2);
    ASSERT_TRUE(a.has_value()) << a.error().message();
    auto b = pool.FetchPage(2);
    ASSERT_TRUE(b.has_value());

    // Same frame: both guards see the same Page object.
    EXPECT_EQ(&a->page(), &b->page());
    EXPECT_EQ(pool.PinCount(2), 2u);
    EXPECT_EQ(pool.stats().hits, 1u);    // Second fetch was a hit
    EXPECT_EQ(pool.stats().misses, 1u);  // First fetch was a miss
}

TEST_F(BufferPoolTest, GuardDestructorAndDropUnpin) {
    BufferPool pool(pager_, 4);
    {
        auto guard = pool.FetchPage(2).value();
        EXPECT_EQ(pool.PinCount(2), 1u);
    }
    EXPECT_EQ(pool.PinCount(2), 0u);  // Destructor unpinned

    auto guard = pool.FetchPage(2).value();
    EXPECT_EQ(pool.PinCount(2), 1u);
    guard.Drop();
    EXPECT_EQ(pool.PinCount(2), 0u);  // Early release
    EXPECT_FALSE(guard.valid());
    guard.Drop();                      // Idempotent
    EXPECT_EQ(pool.PinCount(2), 0u);
}

TEST_F(BufferPoolTest, GuardMoveTransfersOwnership) {
    BufferPool pool(pager_, 4);

    auto a = pool.FetchPage(2).value();
    PageGuard b = std::move(a);
    EXPECT_FALSE(a.valid());  // NOLINT(bugprone-use-after-move): spec'd
    EXPECT_TRUE(b.valid());
    EXPECT_EQ(b.page_id(), 2u);
    EXPECT_EQ(pool.PinCount(2), 1u);  // Still exactly one pin

    PageGuard c = pool.FetchPage(3).value();
    c = std::move(b);  // c's pin on page 3 is released by the assignment
    EXPECT_EQ(pool.PinCount(3), 0u);
    EXPECT_EQ(pool.PinCount(2), 1u);
    EXPECT_EQ(c.page_id(), 2u);
}

TEST_F(BufferPoolTest, PinnedPageSurvivesEvictionPressure) {
    BufferPool pool(pager_, 4);

    auto pinned = pool.FetchPage(2).value();
    const std::string_view record = pinned.page().GetRecordView(0);
    EXPECT_EQ(record, "page-2");

    // Cycle more distinct pages than the pool has frames.
    for (PageId id = 3; id <= 9; ++id) {
        auto guard = pool.FetchPage(id);
        ASSERT_TRUE(guard.has_value()) << "page " << id;
    }

    // The pinned frame was never evicted: still resident, contents intact.
    EXPECT_TRUE(pool.IsResident(2));
    EXPECT_EQ(pinned.page().GetRecordView(0), "page-2");
    EXPECT_GT(pool.stats().evictions, 0u);
}

TEST_F(BufferPoolTest, AllFramesPinnedReturnsBusy) {
    BufferPool pool(pager_, 4);  // kMinFrames is the smallest legal pool

    std::vector<PageGuard> pins;
    for (PageId id = 2; id <= 5; ++id) {
        pins.push_back(pool.FetchPage(id).value());
    }

    auto overflow = pool.FetchPage(6);
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error().code(), StatusCode::kBusy);

    // Releasing one pin makes the fetch succeed again.
    pins.back().Drop();
    EXPECT_TRUE(pool.FetchPage(6).has_value());
}

TEST_F(BufferPoolTest, DirtyEvictionWritesBack) {
    BufferPool pool(pager_, 4);
    {
        auto guard = pool.FetchPage(2).value();
        const std::string_view updated = "updated-record";
        ASSERT_TRUE(guard.page()
                        .InsertRecord(updated.data(),
                                      static_cast<uint16_t>(updated.size()))
                        .has_value());
        guard.MarkDirty();
    }
    // Force page 2 out of the pool (4 frames, 4 newer pages: 2 is the LRU).
    for (PageId id = 3; id <= 6; ++id) {
        ASSERT_TRUE(pool.FetchPage(id).has_value());
    }
    ASSERT_FALSE(pool.IsResident(2));

    // The mutation must have reached disk (checksum stamped by the Pager).
    Page on_disk;
    ASSERT_TRUE(pager_.ReadPage(2, on_disk).ok());
    EXPECT_EQ(on_disk.GetRecordView(1), "updated-record");
}

TEST_F(BufferPoolTest, FlushAllPersistsDirtyFramesWithoutEviction) {
    BufferPool pool(pager_, 4);
    auto guard = pool.FetchPage(3).value();
    const std::string_view extra = "flushed";
    ASSERT_TRUE(guard.page()
                    .InsertRecord(extra.data(),
                                  static_cast<uint16_t>(extra.size()))
                    .has_value());
    guard.MarkDirty();

    ASSERT_TRUE(pool.FlushAll().ok());

    Page on_disk;
    ASSERT_TRUE(pager_.ReadPage(3, on_disk).ok());
    EXPECT_EQ(on_disk.GetRecordView(1), "flushed");
    EXPECT_TRUE(pool.IsResident(3));  // Flushing does not evict
}

TEST_F(BufferPoolTest, NewPageAllocatesInitiallyZeroedPinnedFrame) {
    BufferPool pool(pager_, 4);
    const PageId before = pager_.num_pages();

    auto guard = pool.NewPage();
    ASSERT_TRUE(guard.has_value()) << guard.error().message();
    EXPECT_EQ(guard->page_id(), before);
    EXPECT_EQ(pager_.num_pages(), before + 1);
    EXPECT_TRUE(guard->page().IsZeroed());
    EXPECT_EQ(pool.PinCount(guard->page_id()), 1u);
}

}  // namespace
}  // namespace kvstore::storage
