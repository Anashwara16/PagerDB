// ==============================================================================
// tests/recovery_test.cc - WAL crash recovery tests
// ==============================================================================
//
// Phase 4 (tasks 4.7/4.8 + defect #9): committed operations survive a crash
// that skips the shutdown checkpoint; recovery is idempotent (recovering the
// same crashed image repeatedly yields the same state, and re-opening an
// already-recovered database changes nothing); a crash *during* recovery is
// itself recoverable; checkpoints bound the WAL; and a crash in the middle
// of allocation-heavy splits (the Phase 1 torn-meta defect #9) recovers to
// a consistent tree.
//
// "Crash" here = arm the torn-write fault hook so the shutdown checkpoint
// (and everything after) fails, leaving data.db partially flushed and
// wal.log as the only complete history.
//
// ==============================================================================

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <map>
#include <string>
#include <vector>

#include "kvstore/db.h"
#include "kvstore/storage/file.h"

namespace kvstore {
namespace {

namespace fs = std::filesystem;

class RecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        storage::PosixFile::ClearWriteFailure();  // Hygiene from prior tests
        dir_ = fs::path(::testing::TempDir()) / "kvstore_recovery_test";
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
    }

    void TearDown() override {
        storage::PosixFile::ClearWriteFailure();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    static std::string Key(int i) { return std::format("key{:05d}", i); }
    static std::string Value(int i) {
        return std::format("value-{:05d}-", i) + std::string(60, 'v');
    }

    static Options CrashyOptions() {
        Options options;
        options.create_if_missing = true;
        options.sync_on_write = true;   // Every op is committed when acked
        options.buffer_pool_frames = 8; // Small pool: constant dirty evictions
        options.max_wal_bytes = 0;      // No auto checkpoint: WAL holds it all
        return options;
    }

    // Build a database with `count` committed puts (every 7th key deleted
    // again), then simulate a crash: all writes start failing, so the
    // shutdown checkpoint cannot run.
    std::map<std::string, std::string> BuildAndCrash(const fs::path& dir,
                                                     int count) {
        std::map<std::string, std::string> expected;
        auto db = DB::Open(dir.string(), CrashyOptions()).value();
        for (int i = 0; i < count; ++i) {
            EXPECT_TRUE(db->Put(Key(i), Value(i)).ok()) << Key(i);
            expected[Key(i)] = Value(i);
        }
        for (int i = 0; i < count; i += 7) {
            EXPECT_TRUE(db->Delete(Key(i)).ok()) << Key(i);
            expected.erase(Key(i));
        }
        storage::PosixFile::FailAfterNWrites(0);  // Crash: nothing lands anymore
        db.reset();                                // Shutdown checkpoint fails
        storage::PosixFile::ClearWriteFailure();
        return expected;
    }

    // Full state via the public API: every pair in sorted order.
    static std::map<std::string, std::string> Dump(DB& db) {
        std::map<std::string, std::string> state;
        std::string last;
        EXPECT_TRUE(db.Scan("", "",
                            [&](std::string_view key, std::string_view value) {
                                EXPECT_LT(last, std::string(key))
                                    << "scan must be strictly ascending";
                                last = std::string(key);
                                state.emplace(key, value);
                                return true;
                            })
                        .ok());
        return state;
    }

    fs::path dir_;
};

TEST_F(RecoveryTest, CommittedOpsSurviveCrashWithoutShutdownCheckpoint) {
    auto expected = BuildAndCrash(dir_, 400);
    ASSERT_GT(fs::file_size(dir_ / "wal.log"), 0u) << "crash must leave a WAL";

    Options options;
    options.create_if_missing = false;
    auto db = DB::Open(dir_.string(), options).value();

    EXPECT_EQ(Dump(*db), expected);
    EXPECT_EQ(db->ApproximateKeyCount(), expected.size());  // Recounted

    // Point lookups agree with the scan.
    for (const auto& [key, value] : expected) {
        auto result = db->Get(key);
        ASSERT_TRUE(result.has_value()) << key;
        EXPECT_EQ(result.value(), value) << key;
    }

    // The end-of-recovery checkpoint truncated the log.
    EXPECT_EQ(fs::file_size(dir_ / "wal.log"), 0u);
}

TEST_F(RecoveryTest, RecoveryIsIdempotentAcrossCopiesAndReopens) {
    auto expected = BuildAndCrash(dir_, 200);

    // Freeze the crashed image before any recovery touches it.
    const fs::path copy_dir = dir_.parent_path() / "kvstore_recovery_copy";
    std::error_code ec;
    fs::remove_all(copy_dir, ec);
    fs::copy(dir_, copy_dir, fs::copy_options::recursive);

    Options options;
    options.create_if_missing = false;

    // Recover the original.
    std::map<std::string, std::string> first;
    {
        auto db = DB::Open(dir_.string(), options).value();
        first = Dump(*db);
    }
    EXPECT_EQ(first, expected);

    // Re-opening the recovered database changes nothing (nothing to replay).
    {
        auto db = DB::Open(dir_.string(), options).value();
        EXPECT_EQ(Dump(*db), first);
    }

    // Recovering the frozen crashed copy independently gives the same state.
    {
        auto db = DB::Open(copy_dir.string(), options).value();
        EXPECT_EQ(Dump(*db), first);
    }
    fs::remove_all(copy_dir, ec);
}

TEST_F(RecoveryTest, CrashDuringRecoveryIsItselfRecoverable) {
    auto expected = BuildAndCrash(dir_, 300);

    Options options;
    options.create_if_missing = false;

    // First recovery attempt dies partway through (writes start failing
    // after a handful succeed - mid-replay / mid-checkpoint).
    storage::PosixFile::FailAfterNWrites(5);
    auto crashed_open = DB::Open(dir_.string(), options);
    storage::PosixFile::ClearWriteFailure();
    ASSERT_FALSE(crashed_open.has_value())
        << "open was expected to fail mid-recovery";

    // Second attempt must complete and yield exactly the committed state.
    auto db = DB::Open(dir_.string(), options).value();
    EXPECT_EQ(Dump(*db), expected);
}

TEST_F(RecoveryTest, CheckpointsBoundTheWalDuringAWorkload) {
    Options options;
    options.create_if_missing = true;
    options.sync_on_write = false;
    options.buffer_pool_frames = 32;
    options.max_wal_bytes = 64 * 1024;  // Aggressive auto-checkpointing

    auto db = DB::Open(dir_.string(), options).value();
    uint64_t max_seen = 0;
    for (int i = 0; i < 2000; ++i) {
        ASSERT_TRUE(db->Put(Key(i), Value(i)).ok()) << Key(i);
        if (i % 50 == 0 && fs::exists(dir_ / "wal.log")) {
            max_seen = std::max(max_seen,
                                static_cast<uint64_t>(fs::file_size(dir_ / "wal.log")));
        }
    }
    // The WAL never grows far past the threshold (one burst of slack: a
    // single splitting Put can append a few page images past the limit).
    EXPECT_LT(max_seen, 128u * 1024u);

    // A manual checkpoint empties it entirely.
    ASSERT_TRUE(db->Checkpoint().ok());
    EXPECT_EQ(fs::file_size(dir_ / "wal.log"), 0u);
}

// Defect #9 regression (Phase 1 list): a crash in the middle of the
// allocation + meta activity of splitting inserts must never leave an
// inconsistent database. Sweep several fault points to land in different
// spots of the split/alloc sequence.
TEST_F(RecoveryTest, CrashMidAllocationHeavySplitsRecoversConsistently) {
    for (const int64_t fault_after : {3, 7, 13, 29, 61}) {
        const fs::path dir =
            dir_.parent_path() /
            ("kvstore_recovery_alloc_" + std::to_string(fault_after));
        std::error_code ec;
        fs::remove_all(dir, ec);

        std::map<std::string, std::string> committed;
        {
            auto db = DB::Open(dir.string(), CrashyOptions()).value();
            // Large values force frequent splits (and page allocations).
            const std::string big(700, 'b');
            storage::PosixFile::FailAfterNWrites(fault_after);
            for (int i = 0; i < 200; ++i) {
                Status s = db->Put(Key(i), big);
                if (!s.ok()) {
                    break;  // "Crashed" mid-operation
                }
                committed[Key(i)] = big;
            }
            // Keep failing through the destructor's checkpoint attempt.
            db.reset();
            storage::PosixFile::ClearWriteFailure();
        }

        Options options;
        options.create_if_missing = false;
        auto reopened = DB::Open(dir.string(), options);
        ASSERT_TRUE(reopened.has_value())
            << "fault_after=" << fault_after << ": "
            << reopened.error().message();
        auto db = std::move(*reopened);

        // Every acknowledged key must be present; the in-flight key may or
        // may not have made it (its WAL flush raced the fault).
        auto state = Dump(*db);
        const std::string in_flight = Key(static_cast<int>(committed.size()));
        for (const auto& [key, value] : committed) {
            auto it = state.find(key);
            ASSERT_NE(it, state.end())
                << "fault_after=" << fault_after << " lost " << key;
            EXPECT_EQ(it->second, value) << key;
        }
        for (const auto& [key, value] : state) {
            EXPECT_TRUE(committed.contains(key) || key == in_flight)
                << "fault_after=" << fault_after << " phantom key " << key;
        }
        fs::remove_all(dir, ec);
    }
}

}  // namespace
}  // namespace kvstore
