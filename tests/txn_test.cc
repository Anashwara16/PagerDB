// ==============================================================================
// tests/txn_test.cc - Explicit transactions (Phase 5.3)
// ==============================================================================
//
// Begin/Commit/Abort semantics at the DB facade: commit durability across
// reopen, single-transaction rollback (insert, update, delete), read-your-own
// -writes, and autocommit coexistence. Crash-in-flight recovery (a transaction
// that neither commits nor aborts before a crash) is exercised in Phase 5.4.
//
// ==============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "kvstore/db.h"
#include "kvstore/storage/file.h"
#include "kvstore/txn/lock_manager.h"

namespace kvstore {
namespace {

namespace fs = std::filesystem;

class TxnTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::path(::testing::TempDir()) / "kvstore_txn_test";
        std::error_code ec;
        fs::remove_all(dir_, ec);
        db_path_ = dir_.string();
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    std::unique_ptr<DB> Open(bool create) {
        Options options;
        options.create_if_missing = create;
        auto db = DB::Open(db_path_, options);
        EXPECT_TRUE(db.has_value()) << (db ? "" : db.error().message());
        return db ? std::move(*db) : nullptr;
    }

    fs::path dir_;
    std::string db_path_;
};

TEST_F(TxnTest, CommitPersistsAllWritesAcrossReopen) {
    {
        auto db = Open(true);
        auto txn = std::move(db->Begin().value());
        ASSERT_TRUE(txn->Put("alpha", "1").ok());
        ASSERT_TRUE(txn->Put("beta", "2").ok());
        ASSERT_TRUE(txn->Put("gamma", "3").ok());
        ASSERT_TRUE(txn->Commit().ok());
    }
    {
        auto db = Open(false);
        EXPECT_EQ(db->Get("alpha").value(), "1");
        EXPECT_EQ(db->Get("beta").value(), "2");
        EXPECT_EQ(db->Get("gamma").value(), "3");
    }
}

TEST_F(TxnTest, AbortDiscardsFreshInserts) {
    auto db = Open(true);
    ASSERT_TRUE(db->Put("keep", "yes").ok());  // Autocommit, survives

    auto txn = std::move(db->Begin().value());
    ASSERT_TRUE(txn->Put("temp1", "a").ok());
    ASSERT_TRUE(txn->Put("temp2", "b").ok());
    // Reads its own uncommitted writes.
    EXPECT_EQ(txn->Get("temp1").value(), "a");
    ASSERT_TRUE(txn->Abort().ok());

    EXPECT_EQ(db->Get("keep").value(), "yes");
    EXPECT_FALSE(db->Get("temp1").has_value());
    EXPECT_FALSE(db->Get("temp2").has_value());
    EXPECT_EQ(db->ApproximateKeyCount(), 1u);  // Only "keep"
}

TEST_F(TxnTest, AbortRestoresPriorValueOnUpdate) {
    auto db = Open(true);
    ASSERT_TRUE(db->Put("k", "original").ok());

    auto txn = std::move(db->Begin().value());
    ASSERT_TRUE(txn->Put("k", "modified").ok());
    EXPECT_EQ(txn->Get("k").value(), "modified");  // Own write
    ASSERT_TRUE(txn->Abort().ok());

    EXPECT_EQ(db->Get("k").value(), "original");
    EXPECT_EQ(db->ApproximateKeyCount(), 1u);
}

TEST_F(TxnTest, AbortRestoresDeletedKey) {
    auto db = Open(true);
    ASSERT_TRUE(db->Put("k", "v").ok());

    auto txn = std::move(db->Begin().value());
    ASSERT_TRUE(txn->Delete("k").ok());
    EXPECT_FALSE(txn->Get("k").has_value());  // Gone within the txn
    ASSERT_TRUE(txn->Abort().ok());

    EXPECT_EQ(db->Get("k").value(), "v");  // Restored
}

TEST_F(TxnTest, CommittedDeleteStaysDeletedAcrossReopen) {
    {
        auto db = Open(true);
        ASSERT_TRUE(db->Put("k", "v").ok());
        auto txn = std::move(db->Begin().value());
        ASSERT_TRUE(txn->Delete("k").ok());
        ASSERT_TRUE(txn->Commit().ok());
    }
    {
        auto db = Open(false);
        EXPECT_FALSE(db->Get("k").has_value());
        EXPECT_EQ(db->ApproximateKeyCount(), 0u);
    }
}

TEST_F(TxnTest, AbortIsDurableAcrossReopen) {
    {
        auto db = Open(true);
        ASSERT_TRUE(db->Put("k", "orig").ok());
        auto txn = std::move(db->Begin().value());
        ASSERT_TRUE(txn->Put("k", "changed").ok());
        ASSERT_TRUE(txn->Put("fresh", "x").ok());
        ASSERT_TRUE(txn->Abort().ok());
    }
    {
        auto db = Open(false);
        EXPECT_EQ(db->Get("k").value(), "orig");
        EXPECT_FALSE(db->Get("fresh").has_value());
    }
}

TEST_F(TxnTest, DestructorAbortsAbandonedTransaction) {
    auto db = Open(true);
    ASSERT_TRUE(db->Put("k", "v").ok());
    {
        auto txn = std::move(db->Begin().value());
        ASSERT_TRUE(txn->Put("k", "dirty").ok());
        ASSERT_TRUE(txn->Put("leaked", "x").ok());
        // txn goes out of scope without Commit -> auto-abort.
    }
    EXPECT_EQ(db->Get("k").value(), "v");
    EXPECT_FALSE(db->Get("leaked").has_value());
}

TEST_F(TxnTest, EmptyTransactionCommitAndAbortAreNoops) {
    auto db = Open(true);
    ASSERT_TRUE(db->Put("k", "v").ok());
    {
        auto txn = std::move(db->Begin().value());
        ASSERT_TRUE(txn->Commit().ok());
    }
    {
        auto txn = std::move(db->Begin().value());
        ASSERT_TRUE(txn->Abort().ok());
    }
    EXPECT_EQ(db->Get("k").value(), "v");
    EXPECT_EQ(db->ApproximateKeyCount(), 1u);
}

TEST_F(TxnTest, OperationsAfterCommitFail) {
    auto db = Open(true);
    auto txn = std::move(db->Begin().value());
    ASSERT_TRUE(txn->Put("k", "v").ok());
    ASSERT_TRUE(txn->Commit().ok());
    EXPECT_FALSE(txn->Put("k", "again").ok());  // Spent
    EXPECT_FALSE(txn->Commit().ok());
    EXPECT_FALSE(txn->Get("k").has_value());
}

// A transaction large enough to split leaves commits atomically and durably.
TEST_F(TxnTest, LargeTransactionCommitsAcrossSplits) {
    {
        auto db = Open(true);
        auto txn = std::move(db->Begin().value());
        for (int i = 0; i < 500; ++i) {
            ASSERT_TRUE(txn->Put("key" + std::to_string(i),
                                 "val" + std::to_string(i))
                            .ok());
        }
        ASSERT_TRUE(txn->Commit().ok());
    }
    {
        auto db = Open(false);
        for (int i = 0; i < 500; ++i) {
            auto got = db->Get("key" + std::to_string(i));
            ASSERT_TRUE(got.has_value()) << i;
            EXPECT_EQ(*got, "val" + std::to_string(i));
        }
        EXPECT_EQ(db->ApproximateKeyCount(), 500u);
    }
}

// A large transaction that aborts leaves the tree as it was (rollback across
// the split structure - the structure stays, the keys do not).
TEST_F(TxnTest, LargeTransactionAbortRollsBackKeys) {
    auto db = Open(true);
    auto txn = std::move(db->Begin().value());
    for (int i = 0; i < 500; ++i) {
        ASSERT_TRUE(txn->Put("key" + std::to_string(i), "v").ok());
    }
    ASSERT_TRUE(txn->Abort().ok());
    for (int i = 0; i < 500; ++i) {
        EXPECT_FALSE(db->Get("key" + std::to_string(i)).has_value()) << i;
    }
    EXPECT_EQ(db->ApproximateKeyCount(), 0u);
}

// ==============================================================================
// Phase 5.5: strict 2PL isolation - lock lifecycle and anomaly tests
// ==============================================================================

// Regression (watched fail, then pass): before ReleaseAll was wired into
// Commit/Abort, a committed transaction's X lock leaked forever, so the NEXT
// transaction touching the same key died with Busy. Strict 2PL releases at
// commit - sequential transactions on one key must never conflict.
TEST_F(TxnTest, SequentialTransactionsOnSameKeyDoNotConflict) {
    auto db = Open(true);
    for (int round = 0; round < 3; ++round) {
        auto txn = std::move(db->Begin().value());
        ASSERT_TRUE(txn->Put("hot", "v" + std::to_string(round)).ok())
            << "round " << round << ": lock leaked from a previous commit?";
        ASSERT_TRUE(txn->Commit().ok());
    }
    {
        auto txn = std::move(db->Begin().value());
        ASSERT_TRUE(txn->Put("hot", "aborted").ok());
        ASSERT_TRUE(txn->Abort().ok());
    }
    // Abort released its locks too.
    auto txn = std::move(db->Begin().value());
    ASSERT_TRUE(txn->Delete("hot").ok())
        << "lock leaked from the aborted transaction?";
    ASSERT_TRUE(txn->Commit().ok());
}

// Dirty read: T2 must not observe T1's uncommitted write. T2 is younger than
// the conflicting holder T1, so wait-die kills it immediately (Busy).
TEST_F(TxnTest, DirtyReadIsPrevented) {
    auto db = Open(true);
    ASSERT_TRUE(db->Put("k", "committed").ok());

    auto t1 = std::move(db->Begin().value());
    auto t2 = std::move(db->Begin().value());
    ASSERT_TRUE(t1->Put("k", "uncommitted").ok());

    auto read = t2->Get("k");
    ASSERT_FALSE(read.has_value()) << "dirty read: saw '" << *read << "'";
    EXPECT_EQ(read.error().code(), StatusCode::kBusy);

    ASSERT_TRUE(t2->Abort().ok());   // wait-die: the younger party retries
    ASSERT_TRUE(t1->Commit().ok());
    EXPECT_EQ(db->Get("k").value(), "uncommitted");
}

// Write-write conflict: two transactions writing the same key serialize; the
// younger one dies rather than clobbering or deadlocking.
TEST_F(TxnTest, WriteWriteConflictKillsTheYounger) {
    auto db = Open(true);
    auto t1 = std::move(db->Begin().value());
    auto t2 = std::move(db->Begin().value());

    ASSERT_TRUE(t1->Put("k", "from-t1").ok());
    Status s = t2->Put("k", "from-t2");
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kBusy);

    ASSERT_TRUE(t2->Abort().ok());
    ASSERT_TRUE(t1->Commit().ok());
    EXPECT_EQ(db->Get("k").value(), "from-t1");
}

// Lost update: both read, both try to write. The second writer (younger, and
// blocked by the first reader's S lock) dies; the survivor's update wins and
// nothing is silently overwritten.
TEST_F(TxnTest, LostUpdateIsPrevented) {
    auto db = Open(true);
    ASSERT_TRUE(db->Put("counter", "10").ok());

    auto t1 = std::move(db->Begin().value());
    auto t2 = std::move(db->Begin().value());
    ASSERT_EQ(t1->Get("counter").value(), "10");  // S lock for t1
    ASSERT_EQ(t2->Get("counter").value(), "10");  // S lock for t2

    // t2 (younger) needs X but t1 holds S: wait-die -> Busy.
    Status s = t2->Put("counter", "12");
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kBusy);
    ASSERT_TRUE(t2->Abort().ok());

    // t1 is now the sole holder: its S -> X upgrade succeeds.
    ASSERT_TRUE(t1->Put("counter", "11").ok());
    ASSERT_TRUE(t1->Commit().ok());
    EXPECT_EQ(db->Get("counter").value(), "11");
}

// Deletes take X locks too: a younger transaction cannot delete a key an
// older transaction has written (regression: Delete skipped the lock manager).
TEST_F(TxnTest, DeleteConflictsWithUncommittedWrite) {
    auto db = Open(true);
    ASSERT_TRUE(db->Put("k", "v").ok());

    auto t1 = std::move(db->Begin().value());
    auto t2 = std::move(db->Begin().value());
    ASSERT_TRUE(t1->Put("k", "new").ok());

    Status s = t2->Delete("k");
    ASSERT_FALSE(s.ok()) << "Delete ignored t1's exclusive lock";
    EXPECT_EQ(s.code(), StatusCode::kBusy);

    ASSERT_TRUE(t2->Abort().ok());
    ASSERT_TRUE(t1->Commit().ok());
    EXPECT_EQ(db->Get("k").value(), "new");
}

// Wait-die's WAIT half: an OLDER transaction blocks (rather than dying) until
// the younger holder commits, then proceeds.
TEST_F(TxnTest, OlderWriterWaitsForYoungerHolderToCommit) {
    auto db = Open(true);
    auto t1 = std::move(db->Begin().value());  // Older (smaller txid)
    auto t2 = std::move(db->Begin().value());  // Younger
    ASSERT_TRUE(t2->Put("k", "from-t2").ok()); // t2 holds X

    // Commit t2 from another thread while t1 blocks in Acquire.
    std::thread committer([&t2] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ASSERT_TRUE(t2->Commit().ok());
    });

    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(t1->Put("k", "from-t1").ok());  // Waits for t2's release
    const auto waited = std::chrono::steady_clock::now() - start;
    committer.join();

    EXPECT_GE(waited, std::chrono::milliseconds(50)) << "t1 never blocked";
    ASSERT_TRUE(t1->Commit().ok());
    EXPECT_EQ(db->Get("k").value(), "from-t1");  // t1 wrote after t2: t1 wins
}

// Executable documentation of the isolation boundary (db.h states it; these
// pin it): Scan takes no key locks and is read-uncommitted - it CAN see
// another transaction's uncommitted write. If this ever starts failing,
// the isolation level changed and every doc stating it must change too.
TEST_F(TxnTest, ScanIsReadUncommittedByDesign) {
    auto db = Open(true);
    ASSERT_TRUE(db->Put("a", "committed").ok());

    auto txn = std::move(db->Begin().value());
    ASSERT_TRUE(txn->Put("b", "uncommitted").ok());

    std::vector<std::string> seen;
    ASSERT_TRUE(db->Scan("", "", [&](auto k, auto) {
        seen.emplace_back(k);
        return true;
    }).ok());
    EXPECT_EQ(seen, (std::vector<std::string>{"a", "b"}))
        << "Scan no longer sees uncommitted writes - the documented "
           "read-uncommitted boundary changed";
    ASSERT_TRUE(txn->Abort().ok());
}

// Executable documentation: autocommit writes take no key locks, so one can
// land on a key an explicit transaction holds X-locked - and the
// transaction's rollback then restores its before-image, erasing the
// committed autocommit write. This is the documented reason autocommit
// traffic and transactions must use disjoint keys.
TEST_F(TxnTest, AutocommitWriteOnLockedKeyIsErasedByRollbackByDesign) {
    auto db = Open(true);
    ASSERT_TRUE(db->Put("k", "original").ok());

    auto txn = std::move(db->Begin().value());
    ASSERT_TRUE(txn->Put("k", "txn-write").ok());     // X lock on "k"
    ASSERT_TRUE(db->Put("k", "autocommit").ok());     // No lock: clobbers
    ASSERT_TRUE(txn->Abort().ok());                   // Restores "original"

    EXPECT_EQ(db->Get("k").value(), "original")
        << "rollback semantics on a mixed autocommit/txn key changed - "
           "update the documented isolation statement";
}

// Regression (watched fail, then pass): an idle open transaction must not
// block Checkpoint(). Before the fix, a transaction held the checkpoint
// latch shared for its whole lifetime, so this checkpoint deadlocked until
// the transaction ended - one forgotten handle meant unbounded log growth.
// Now the latch is per-operation: an idle transaction holds nothing.
TEST_F(TxnTest, IdleTransactionDoesNotBlockCheckpoint) {
    auto db = Open(true);
    auto txn = std::move(db->Begin().value());
    ASSERT_TRUE(txn->Put("k", "v").ok());  // Begun, now idle

    std::atomic<bool> done{false};
    std::thread checkpointer([&] {
        ASSERT_TRUE(db->Checkpoint().ok());
        done.store(true);
    });
    // Give the checkpoint ample time; without the fix it can't finish until
    // the commit below.
    for (int i = 0; i < 200 && !done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(done.load()) << "Checkpoint blocked behind an idle transaction";

    // Autocommit traffic must also flow alongside the open transaction.
    EXPECT_TRUE(db->Put("autocommit", "x").ok());

    ASSERT_TRUE(txn->Commit().ok());  // Unblocks the old behavior so join returns
    checkpointer.join();
    EXPECT_EQ(db->Get("k").value(), "v");
}

// The fuzzy-checkpoint truncation rule: while a begun transaction is in
// flight, Checkpoint() still flushes pages but must KEEP the log - it holds
// the records a crash-recovery undo of that transaction would need (its
// uncommitted pages may already be on disk via steal). After the transaction
// ends, checkpointing truncates as usual.
TEST_F(TxnTest, CheckpointKeepsTheLogWhileATxnIsInFlight) {
    auto db = Open(true);
    const fs::path wal_path = dir_ / "wal.log";

    auto txn = std::move(db->Begin().value());
    ASSERT_TRUE(txn->Put("k", "uncommitted").ok());
    ASSERT_TRUE(db->Checkpoint().ok());
    EXPECT_GT(fs::file_size(wal_path), 0u)
        << "checkpoint truncated the log out from under an in-flight txn";

    ASSERT_TRUE(txn->Commit().ok());
    ASSERT_TRUE(db->Checkpoint().ok());
    EXPECT_EQ(fs::file_size(wal_path), 0u)
        << "checkpoint failed to truncate once no txn was in flight";
}

// Gate sanity under load: a checkpoint must complete while autocommit writers
// hammer the shared latch (std::shared_mutex promises no writer preference,
// so without the gate this can starve indefinitely on reader-preferring
// implementations). Also a tsan workout for the gate itself.
TEST_F(TxnTest, CheckpointCompletesUnderSustainedAutocommitTraffic) {
    auto db = Open(true);

    std::atomic<bool> stop{false};
    std::atomic<int> failures{0};
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t) {
        writers.emplace_back([&, t] {
            for (int i = 0; !stop.load(std::memory_order_relaxed); ++i) {
                if (!db->Put("w" + std::to_string(t) + "-" + std::to_string(i),
                             "v").ok()) {
                    ++failures;
                    return;
                }
            }
        });
    }
    // A few checkpoints while the writers run; each must come back.
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(db->Checkpoint().ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    stop.store(true);
    for (auto& w : writers) w.join();
    EXPECT_EQ(failures.load(), 0);
}

// Regression (watched fail, then pass): a wait-die victim retried via
// Begin(txn->id()) keeps its original age, so against a holder that began
// AFTER it, the retry waits and wins instead of dying again. Without
// inheritance every retry restarts ever-younger and can starve forever.
TEST_F(TxnTest, RetryWithInheritedAgeWaitsInsteadOfDyingAgain) {
    auto db = Open(true);

    auto t1 = std::move(db->Begin().value());  // Oldest
    auto victim = std::move(db->Begin().value());
    ASSERT_TRUE(t1->Put("k", "t1").ok());      // t1 holds X

    // The victim (younger than t1) dies, as wait-die demands.
    ASSERT_EQ(victim->Put("k", "victim").code(), StatusCode::kBusy);
    const TxId victim_age = victim->id();
    ASSERT_TRUE(victim->Abort().ok());
    ASSERT_TRUE(t1->Commit().ok());

    // A NEW competitor begins (its txid is newer than the victim's original
    // age but older than a fresh Begin would be) and takes the lock.
    auto t3 = std::move(db->Begin().value());
    ASSERT_TRUE(t3->Put("k", "t3").ok());

    // The retry inherits the victim's age: older than t3 -> it must WAIT for
    // t3's commit and then succeed. (A plain Begin() retry would be younger
    // than t3 and die with Busy - the starvation loop this fix breaks.)
    auto retry = std::move(db->Begin(victim_age).value());
    EXPECT_EQ(retry->id(), victim_age);
    std::thread committer([&t3] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ASSERT_TRUE(t3->Commit().ok());
    });
    const auto start = std::chrono::steady_clock::now();
    Status s = retry->Put("k", "retry");
    const auto waited = std::chrono::steady_clock::now() - start;
    committer.join();

    ASSERT_TRUE(s.ok()) << "retry died despite inherited age: " << s.message();
    EXPECT_GE(waited, std::chrono::milliseconds(50)) << "retry never blocked";
    ASSERT_TRUE(retry->Commit().ok());
    EXPECT_EQ(db->Get("k").value(), "retry");
}

// The 1-second backstop: an older waiter whose younger holder never finishes
// gets Busy instead of hanging forever.
TEST_F(TxnTest, LockWaitTimesOutWhenHolderNeverReleases) {
    auto db = Open(true);
    auto t1 = std::move(db->Begin().value());  // Older
    auto t2 = std::move(db->Begin().value());  // Younger
    ASSERT_TRUE(t2->Put("k", "held").ok());

    Status s = t1->Put("k", "blocked");  // Waits ~1s, then gives up
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kBusy);

    ASSERT_TRUE(t1->Abort().ok());
    ASSERT_TRUE(t2->Commit().ok());
    EXPECT_EQ(db->Get("k").value(), "held");
}

// Regression (watched fail, then pass): a transactional write that fails
// half-applied must poison the whole DB handle, exactly like the autocommit
// path does. Before the fix, TxnImpl::Poison only deactivated its own
// transaction, so every other thread kept writing to a tree that may contain
// a half-completed structural change.
TEST_F(TxnTest, HalfAppliedTxnWriteFailurePoisonsTheDbHandle) {
    Options options;
    options.create_if_missing = true;
    options.sync_on_write = false;   // No WAL flush per put
    options.max_wal_bytes = 0;       // No auto-checkpoint
    options.buffer_pool_frames = 4;  // Constant eviction -> disk writes
    auto db = std::move(DB::Open(db_path_, options).value());

    // Multi-page tree so transactional writes trigger evictions (write-backs).
    for (int i = 0; i < 300; ++i) {
        ASSERT_TRUE(db->Put("base" + std::to_string(i),
                            std::string(64, 'x')).ok());
    }
    // Make the baseline durable in the WAL before arming the fault: with
    // sync_on_write=false nothing above has been flushed (the 16-frame
    // minimum pool holds the whole baseline tree, so no eviction flushed it
    // as a side effect), and the poisoned handle will refuse the shutdown
    // checkpoint - recovery can only restore what reached wal.log.
    ASSERT_TRUE(db->Sync().ok());

    auto txn = std::move(db->Begin().value());
    storage::PosixFile::FailAfterNWrites(0);  // Next file write tears + fails

    // Keep writing until an eviction write-back fails inside tree_.Put.
    bool failed = false;
    for (int i = 0; i < 500 && !failed; ++i) {
        failed = !txn->Put("txn" + std::to_string(i), std::string(64, 'y')).ok();
    }
    storage::PosixFile::ClearWriteFailure();
    ASSERT_TRUE(failed) << "fault hook never fired - workload tuning drifted";

    // The failed transaction is spent...
    EXPECT_FALSE(txn->Put("after", "v").ok());
    // ...and the DB handle itself must be poisoned: the tree may hold a
    // half-applied change, so autocommit writes and new transactions are
    // refused until a reopen runs recovery.
    Status s = db->Put("autocommit", "v");
    EXPECT_FALSE(s.ok()) << "autocommit write accepted on a poisoned handle";
    EXPECT_EQ(s.code(), StatusCode::kInternal);
    EXPECT_FALSE(db->Begin().has_value())
        << "new transaction accepted on a poisoned handle";

    // Reopen runs recovery and restores a consistent, writable state.
    // (The poisoned handle refuses the destructor's checkpoint, so the WAL
    // recovery needs survives the close.)
    txn.reset();
    db.reset();
    auto reopened = Open(false);
    EXPECT_EQ(reopened->Get("base0").value(), std::string(64, 'x'));
    EXPECT_TRUE(reopened->Put("post-recovery", "v").ok());
}

// ==============================================================================
// LockManager unit tests (no DB, direct)
// ==============================================================================

TEST(LockManagerTest, SharedLocksCoexistAndUpgradeNeedsSoleOwnership) {
    txn::LockManager lm;
    ASSERT_TRUE(lm.Acquire(1, 1, "k", txn::LockMode::kShared).ok());
    ASSERT_TRUE(lm.Acquire(2, 2, "k", txn::LockMode::kShared).ok());  // S+S is fine

    // txid 2 (younger) upgrading to X conflicts with txid 1's S: dies.
    EXPECT_EQ(lm.Acquire(2, 2, "k", txn::LockMode::kExclusive).code(),
              StatusCode::kBusy);

    lm.ReleaseAll(1);
    // Sole holder now: upgrade succeeds.
    EXPECT_TRUE(lm.Acquire(2, 2, "k", txn::LockMode::kExclusive).ok());
    lm.ReleaseAll(2);
}

TEST(LockManagerTest, ReacquireIsIdempotentAndReleaseAllIsIdempotent) {
    txn::LockManager lm;
    ASSERT_TRUE(lm.Acquire(7, 7, "a", txn::LockMode::kExclusive).ok());
    ASSERT_TRUE(lm.Acquire(7, 7, "a", txn::LockMode::kExclusive).ok());  // Re-acquire
    ASSERT_TRUE(lm.Acquire(7, 7, "a", txn::LockMode::kShared).ok());     // X covers S
    lm.ReleaseAll(7);
    lm.ReleaseAll(7);  // No-op
    // Fully released: another txn takes X immediately.
    EXPECT_TRUE(lm.Acquire(8, 8, "a", txn::LockMode::kExclusive).ok());
    lm.ReleaseAll(8);
}

TEST(LockManagerTest, ReleaseWakesAnOlderWaiter) {
    txn::LockManager lm;
    ASSERT_TRUE(lm.Acquire(2, 2, "k", txn::LockMode::kExclusive).ok());  // Younger holds

    Status waiter_status = Status::Internal("not run");
    std::thread waiter([&] {
        // Older (txid 1) waits for the younger holder instead of dying.
        waiter_status = lm.Acquire(1, 1, "k", txn::LockMode::kExclusive);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    lm.ReleaseAll(2);
    waiter.join();
    EXPECT_TRUE(waiter_status.ok());
    lm.ReleaseAll(1);
}

// Wait-die age is the PRIORITY, not the txid: a retry running under a fresh
// (higher) txid but an inherited (lower) priority outranks a holder whose
// txid is lower - it waits instead of dying.
TEST(LockManagerTest, InheritedPriorityOutranksNewerHolder) {
    txn::LockManager lm;
    // txid 3 holds X (a first attempt: priority = txid = 3).
    ASSERT_TRUE(lm.Acquire(3, 3, "k", txn::LockMode::kExclusive).ok());

    // txid 4 is the RETRY of long-dead txid 2: priority 2 < 3 -> older ->
    // it must wait (and win on release), not die.
    Status retry_status = Status::Internal("not run");
    std::thread retry([&] {
        retry_status = lm.Acquire(4, 2, "k", txn::LockMode::kExclusive);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    lm.ReleaseAll(3);
    retry.join();
    EXPECT_TRUE(retry_status.ok()) << retry_status.message();
    lm.ReleaseAll(4);

    // Sanity: without inheritance (priority = txid = 4 vs holder 3) the same
    // request is the younger party and dies immediately.
    ASSERT_TRUE(lm.Acquire(3, 3, "k", txn::LockMode::kExclusive).ok());
    EXPECT_EQ(lm.Acquire(4, 4, "k", txn::LockMode::kExclusive).code(),
              StatusCode::kBusy);
    lm.ReleaseAll(3);
}

}  // namespace
}  // namespace kvstore
