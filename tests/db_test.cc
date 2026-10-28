// ==============================================================================
// tests/db_test.cc - DB facade tests
// ==============================================================================
//
// Phase 1: Options plumbing (sync_on_write, verify_checksums), key counting,
// and the sync policy regression (defect 1.12: BPlusTree::Put must not sync;
// DbImpl is the sole owner of the sync decision).
//
// ==============================================================================

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "kvstore/db.h"
#include "kvstore/storage/file.h"

namespace kvstore {
namespace {

namespace fs = std::filesystem;

class DbTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::path(::testing::TempDir()) / "kvstore_db_test";
        std::error_code ec;
        fs::remove_all(dir_, ec);  // Stale state from a crashed prior run
        db_path_ = dir_.string();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    fs::path dir_;
    std::string db_path_;
};

// Regression (watched fail, then pass): a second open of the same database
// directory - as a second process would do - must be refused while the first
// handle is alive. Two live handles would each run recovery, checkpoint, and
// truncate each other's WAL. flock gives the same answer across processes as
// across two opens in one process, so this tests the real mechanism.
TEST_F(DbTest, SecondOpenOfLiveDatabaseIsRefused) {
    Options options;
    options.create_if_missing = true;
    auto db = DB::Open(db_path_, options).value();
    ASSERT_TRUE(db->Put("k", "v").ok());

    auto second = DB::Open(db_path_, options);
    ASSERT_FALSE(second.has_value()) << "second handle on a live database";
    EXPECT_EQ(second.error().code(), StatusCode::kBusy);
    EXPECT_EQ(db->Get("k").value(), "v");  // First handle unharmed

    // Releasing the first handle releases the lock: reopen succeeds.
    db.reset();
    auto reopened = DB::Open(db_path_, options);
    ASSERT_TRUE(reopened.has_value()) << reopened.error().message();
    EXPECT_EQ((*reopened)->Get("k").value(), "v");
}

// Regression (watched fail, then pass): a record that cannot fit in one page
// must be rejected up front with InvalidArgument and leave the handle fully
// usable. Before the fix it failed deep inside the split path with Internal
// AND poisoned the handle - every subsequent write demanded a reopen. The
// first thing a user with a >4 KB value hits (no overflow pages: documented
// limitation, Phase 7 candidate).
TEST_F(DbTest, OversizedPutIsRejectedCleanlyWithoutPoisoningTheHandle) {
    Options options;
    options.create_if_missing = true;
    auto db = DB::Open(db_path_, options).value();
    ASSERT_TRUE(db->Put("before", "v").ok());

    Status s = db->Put("big", std::string(8000, 'x'));
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_FALSE(db->Get("big").has_value());

    // The handle survives: autocommit writes and transactions still work.
    EXPECT_TRUE(db->Put("after", "v").ok());
    auto txn = std::move(db->Begin().value());
    EXPECT_EQ(txn->Put("big", std::string(8000, 'x')).code(),
              StatusCode::kInvalidArgument);
    EXPECT_TRUE(txn->Put("txn-key", "v").ok());  // Txn not poisoned either
    EXPECT_TRUE(txn->Commit().ok());
    EXPECT_EQ(db->Get("txn-key").value(), "v");
}

TEST_F(DbTest, PutGetRoundTripAcrossReopen) {
    Options options;
    options.create_if_missing = true;
    {
        auto db = DB::Open(db_path_, options).value();
        ASSERT_TRUE(db->Put("alpha", "one").ok());
        ASSERT_TRUE(db->Put("beta", "two").ok());
    }
    {
        options.create_if_missing = false;
        auto db = DB::Open(db_path_, options).value();
        EXPECT_EQ(db->Get("alpha").value(), "one");
        EXPECT_EQ(db->Get("beta").value(), "two");
        EXPECT_FALSE(db->Get("gamma").has_value());
    }
}

TEST_F(DbTest, ApproximateKeyCountTracksUniqueKeys) {
    Options options;
    options.create_if_missing = true;
    auto db = DB::Open(db_path_, options).value();
    ASSERT_TRUE(db->Put("alpha", "one").ok());
    ASSERT_TRUE(db->Put("beta", "two").ok());
    ASSERT_TRUE(db->Put("alpha", "updated").ok());
    EXPECT_EQ(db->ApproximateKeyCount(), 2u);
}

// Regression for defect 1.12: with sync_on_write=false, Puts must issue no
// fsync at all. BPlusTree::Put used to call pager_.Sync() unconditionally,
// taking the policy decision away from DbImpl.
TEST_F(DbTest, NoSyncsWhenSyncOnWriteDisabled) {
    Options options;
    options.create_if_missing = true;
    options.sync_on_write = false;
    auto db = DB::Open(db_path_, options).value();

    const uint64_t after_open = storage::PosixFile::TotalSyncs();
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(db->Put("key" + std::to_string(i), "value").ok());
    }
    EXPECT_EQ(storage::PosixFile::TotalSyncs(), after_open);
}

TEST_F(DbTest, SyncOnWriteIssuesSyncPerPut) {
    Options options;
    options.create_if_missing = true;
    options.sync_on_write = true;
    auto db = DB::Open(db_path_, options).value();

    const uint64_t before = storage::PosixFile::TotalSyncs();
    ASSERT_TRUE(db->Put("alpha", "one").ok());
    EXPECT_GE(storage::PosixFile::TotalSyncs(), before + 1);
}

// -----------------------------------------------------------------------------
// Phase 2 (task 2.3): Delete through the public API
// -----------------------------------------------------------------------------

TEST_F(DbTest, DeleteRemovesKeyAcrossReopen) {
    Options options;
    options.create_if_missing = true;
    {
        auto db = DB::Open(db_path_, options).value();
        ASSERT_TRUE(db->Put("alpha", "one").ok());
        ASSERT_TRUE(db->Put("beta", "two").ok());
        ASSERT_TRUE(db->Delete("alpha").ok());
        EXPECT_FALSE(db->Get("alpha").has_value());
        EXPECT_EQ(db->ApproximateKeyCount(), 1u);
    }
    {
        options.create_if_missing = false;
        auto db = DB::Open(db_path_, options).value();
        EXPECT_FALSE(db->Get("alpha").has_value());
        EXPECT_EQ(db->Get("beta").value(), "two");
        EXPECT_EQ(db->ApproximateKeyCount(), 1u);
    }
}

TEST_F(DbTest, DeleteMissingKeyIsNotFoundAndDoesNotSync) {
    Options options;
    options.create_if_missing = true;
    options.sync_on_write = true;
    auto db = DB::Open(db_path_, options).value();
    ASSERT_TRUE(db->Put("alpha", "one").ok());

    const uint64_t before = storage::PosixFile::TotalSyncs();
    EXPECT_EQ(db->Delete("missing").code(), StatusCode::kNotFound);
    // Nothing changed on disk, so the failed delete must not fsync.
    EXPECT_EQ(storage::PosixFile::TotalSyncs(), before);
}

TEST_F(DbTest, DeleteHonorsSyncOnWrite) {
    Options options;
    options.create_if_missing = true;
    options.sync_on_write = true;
    auto db = DB::Open(db_path_, options).value();
    ASSERT_TRUE(db->Put("alpha", "one").ok());

    const uint64_t before = storage::PosixFile::TotalSyncs();
    ASSERT_TRUE(db->Delete("alpha").ok());
    EXPECT_GE(storage::PosixFile::TotalSyncs(), before + 1);
}

// Phase 3: repeated reads of the same keys must be served from the buffer
// pool - hits grow, misses stay flat.
TEST_F(DbTest, RepeatReadsHitTheBufferPool) {
    Options options;
    options.create_if_missing = true;
    options.sync_on_write = false;
    options.buffer_pool_frames = 16;
    auto db = DB::Open(db_path_, options).value();
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(db->Put("key" + std::to_string(i), "value").ok());
    }

    const auto before = db->GetStats();
    for (int round = 0; round < 10; ++round) {
        for (int i = 0; i < 20; ++i) {
            ASSERT_TRUE(db->Get("key" + std::to_string(i)).has_value());
        }
    }
    const auto after = db->GetStats();

    EXPECT_GT(after.pool_hits, before.pool_hits);
    EXPECT_EQ(after.pool_misses, before.pool_misses);  // Everything cached
}

// -----------------------------------------------------------------------------
// Phase 2 (task 2.11): Scan
// -----------------------------------------------------------------------------

class DbScanTest : public DbTest {
protected:
    void SetUp() override {
        DbTest::SetUp();
        Options options;
        options.create_if_missing = true;
        options.sync_on_write = false;
        db_ = std::move(DB::Open(db_path_, options).value());
        for (int i = 0; i < 50; ++i) {
            ASSERT_TRUE(db_->Put(Key(i), "v" + std::to_string(i)).ok());
        }
    }

    static std::string Key(int i) {
        return "key" + std::to_string(i / 10) + std::to_string(i % 10);
    }

    std::unique_ptr<DB> db_;
};

TEST_F(DbScanTest, ScanReturnsHalfOpenRange) {
    std::vector<std::string> keys;
    ASSERT_TRUE(db_->Scan(Key(10), Key(20),
                          [&](std::string_view key, std::string_view value) {
                              keys.emplace_back(key);
                              EXPECT_FALSE(value.empty());
                              return true;
                          })
                    .ok());

    ASSERT_EQ(keys.size(), 10u);           // [10, 20): end is exclusive
    EXPECT_EQ(keys.front(), Key(10));
    EXPECT_EQ(keys.back(), Key(19));
}

TEST_F(DbScanTest, ScanWithEmptyEndRunsToTheLastKey) {
    std::vector<std::string> keys;
    ASSERT_TRUE(db_->Scan(Key(45), "",
                          [&](std::string_view key, std::string_view) {
                              keys.emplace_back(key);
                              return true;
                          })
                    .ok());

    ASSERT_EQ(keys.size(), 5u);
    EXPECT_EQ(keys.back(), Key(49));
}

TEST_F(DbScanTest, ScanWithEmptyStartBeginsAtFirstKey) {
    std::string first;
    ASSERT_TRUE(db_->Scan("", "",
                          [&](std::string_view key, std::string_view) {
                              if (first.empty()) {
                                  first = key;
                              }
                              return true;
                          })
                    .ok());
    EXPECT_EQ(first, Key(0));
}

TEST_F(DbScanTest, ScanStopsWhenCallbackReturnsFalse) {
    int count = 0;
    ASSERT_TRUE(db_->Scan("", "",
                          [&](std::string_view, std::string_view) {
                              return ++count < 7;
                          })
                    .ok());
    EXPECT_EQ(count, 7);
}

TEST_F(DbScanTest, ScanSkipsDeletedKeys) {
    ASSERT_TRUE(db_->Delete(Key(12)).ok());
    ASSERT_TRUE(db_->Delete(Key(13)).ok());

    std::vector<std::string> keys;
    ASSERT_TRUE(db_->Scan(Key(10), Key(15),
                          [&](std::string_view key, std::string_view) {
                              keys.emplace_back(key);
                              return true;
                          })
                    .ok());

    EXPECT_EQ(keys, (std::vector<std::string>{Key(10), Key(11), Key(14)}));
}

}  // namespace
}  // namespace kvstore
