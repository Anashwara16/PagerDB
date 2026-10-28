// ==============================================================================
// tests/buffer_pool_concurrent_test.cc - Thread-safety tests (Phase 5.1)
// ==============================================================================
//
// The pool's structures (page table, free list, replacer, pins) are guarded
// by an internal mutex; page CONTENT is guarded by a per-frame shared_mutex
// acquired through the FetchPageRead / FetchPageWrite guard variants.
//
// Torn-access detection: writers keep two copies of a counter inside the
// page payload and increment both under the exclusive latch; readers assert
// the copies match. Any torn read/write or lost update trips the assertion,
// and TSAN (the `tsan` preset) checks the same tests for data races.
//
// ==============================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <random>
#include <thread>
#include <vector>

#include "kvstore/storage/buffer_pool.h"
#include "kvstore/storage/pager.h"

namespace kvstore::storage {
namespace {

namespace fs = std::filesystem;

constexpr PageId kFirstPage = 2;
constexpr PageId kPageCount = 16;  // Pages 2..17
constexpr size_t kCounterOffsetA = 100;
constexpr size_t kCounterOffsetB = 2100;

uint64_t LoadCounter(const Page& page, size_t offset) {
    uint64_t value;
    std::memcpy(&value, page.RawData() + offset, sizeof(value));
    return value;
}

void StoreCounter(Page& page, size_t offset, uint64_t value) {
    std::memcpy(page.RawData() + offset, &value, sizeof(value));
}

class BufferPoolConcurrentTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::path(::testing::TempDir()) / "kvstore_bp_concurrent_test";
        fs::create_directories(dir_);
        auto pager = Pager::Open((dir_ / "data.db").string(), true);
        ASSERT_TRUE(pager.has_value()) << pager.error().message();
        pager_ = std::move(*pager);
        for (PageId id = kFirstPage; id < kFirstPage + kPageCount; ++id) {
            ASSERT_TRUE(pager_.AllocatePage().has_value());
            Page page;
            page.Init(id, PageType::kLeaf);
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

TEST_F(BufferPoolConcurrentTest, WriteGuardExcludesReadersUntilDropped) {
    BufferPool pool(pager_, 8);

    auto writer = pool.FetchPageWrite(kFirstPage);
    ASSERT_TRUE(writer.has_value()) << writer.error().message();
    StoreCounter(writer->page(), kCounterOffsetA, 41);
    StoreCounter(writer->page(), kCounterOffsetB, 40);  // Deliberately torn
    writer->MarkDirty();

    std::atomic<bool> reader_done{false};
    std::thread reader([&] {
        auto guard = pool.FetchPageRead(kFirstPage).value();  // Blocks
        // By the time the reader gets the shared latch, the writer must
        // have finished repairing the page.
        EXPECT_EQ(LoadCounter(guard.page(), kCounterOffsetA),
                  LoadCounter(guard.page(), kCounterOffsetB));
        reader_done.store(true);
    });

    // Give the reader a moment to block on the latch, then repair + drop.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(reader_done.load()) << "reader got in while writer held X";
    StoreCounter(writer->page(), kCounterOffsetB, 41);
    writer->Drop();

    reader.join();
    EXPECT_TRUE(reader_done.load());
}

TEST_F(BufferPoolConcurrentTest, TwoReadGuardsShareAPage) {
    BufferPool pool(pager_, 8);
    auto a = pool.FetchPageRead(kFirstPage);
    ASSERT_TRUE(a.has_value());
    auto b = pool.FetchPageRead(kFirstPage);  // Must not deadlock
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(&a->page(), &b->page());
    EXPECT_EQ(pool.PinCount(kFirstPage), 2u);
}

// The core stress: 8 threads hammer 16 pages through a 6-frame pool
// (constant eviction) with mixed read/write guards. Writers increment both
// counter copies under the exclusive latch; readers require the copies to
// match. With 8 threads pinning one page each against 6 frames, transient
// Status::Busy is legitimate pool behavior (documented in buffer_pool.h) -
// retry those; any other fetch error or a torn counter pair is a failure.
// Run under the tsan preset for race detection.
TEST_F(BufferPoolConcurrentTest, MixedFetchStressKeepsPagesConsistent) {
    BufferPool pool(pager_, 6);

    constexpr int kThreads = 8;
    constexpr int kItersPerThread = 1500;
    std::atomic<int> torn_reads{0};
    std::atomic<int> fetch_errors{0};
    std::atomic<long> busy_retries{0};

    auto fetch_with_retry = [&](auto&& fetch) {
        using GuardResult = std::invoke_result_t<decltype(fetch)>;
        for (;;) {
            GuardResult guard = fetch();
            if (guard || guard.error().code() != StatusCode::kBusy) {
                return guard;
            }
            busy_retries.fetch_add(1);
            std::this_thread::yield();
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(static_cast<uint32_t>(t) * 977u + 13u);
            std::uniform_int_distribution<PageId> pick(
                kFirstPage, kFirstPage + kPageCount - 1);
            std::uniform_int_distribution<int> coin(0, 3);
            for (int i = 0; i < kItersPerThread; ++i) {
                const PageId page_id = pick(rng);
                if (coin(rng) == 0) {  // 25% writers
                    auto guard = fetch_with_retry(
                        [&] { return pool.FetchPageWrite(page_id); });
                    if (!guard) { fetch_errors.fetch_add(1); continue; }
                    const uint64_t v = LoadCounter(guard->page(), kCounterOffsetA);
                    if (v != LoadCounter(guard->page(), kCounterOffsetB)) {
                        torn_reads.fetch_add(1);
                    }
                    StoreCounter(guard->page(), kCounterOffsetA, v + 1);
                    StoreCounter(guard->page(), kCounterOffsetB, v + 1);
                    guard->MarkDirty();
                } else {
                    auto guard = fetch_with_retry(
                        [&] { return pool.FetchPageRead(page_id); });
                    if (!guard) { fetch_errors.fetch_add(1); continue; }
                    if (LoadCounter(guard->page(), kCounterOffsetA) !=
                        LoadCounter(guard->page(), kCounterOffsetB)) {
                        torn_reads.fetch_add(1);
                    }
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(torn_reads.load(), 0);
    EXPECT_EQ(fetch_errors.load(), 0);

    // Every page survived eviction round-trips with matching copies.
    for (PageId id = kFirstPage; id < kFirstPage + kPageCount; ++id) {
        auto guard = pool.FetchPageRead(id).value();
        EXPECT_EQ(LoadCounter(guard.page(), kCounterOffsetA),
                  LoadCounter(guard.page(), kCounterOffsetB))
            << "page " << id;
    }
}

TEST_F(BufferPoolConcurrentTest, ConcurrentNewPageYieldsDistinctIds) {
    BufferPool pool(pager_, 64);

    constexpr int kThreads = 4;
    constexpr int kPagesPerThread = 25;
    std::vector<std::vector<PageId>> ids(kThreads);

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPagesPerThread; ++i) {
                auto guard = pool.NewPage();
                if (guard) {
                    ids[static_cast<size_t>(t)].push_back(guard->page_id());
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    std::vector<PageId> all;
    for (const auto& per_thread : ids) {
        all.insert(all.end(), per_thread.begin(), per_thread.end());
    }
    ASSERT_EQ(all.size(),
              static_cast<size_t>(kThreads) * kPagesPerThread);
    std::sort(all.begin(), all.end());
    EXPECT_EQ(std::adjacent_find(all.begin(), all.end()), all.end())
        << "duplicate page id handed out";
}

}  // namespace
}  // namespace kvstore::storage
