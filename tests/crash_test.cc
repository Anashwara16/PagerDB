// ==============================================================================
// tests/crash_test.cc - Fork-based crash/recovery harness (Phase 4, task 4.10)
// ==============================================================================
//
// Each run forks a child that executes a seeded, deterministic stream of
// Puts/Deletes (sync_on_write=true: every acknowledged op is committed) with
// the torn-write fault hook armed at a seeded countdown. When the fault
// fires - possibly mid-split, mid-checkpoint, or mid-WAL-flush - the child
// _exit()s immediately: no destructor, no shutdown checkpoint, torn bytes
// left exactly where they landed.
//
// The parent regenerates the identical op stream from the seed, reads how
// many ops the child confirmed (a plain-stdio side file, unaffected by the
// hook), reopens the database (running recovery), and requires:
//   - every confirmed op's effect is present (committed == durable),
//   - the observable state is exactly the confirmed prefix, or that prefix
//     plus the single in-flight op (which may have committed its WAL record
//     just before the fault),
//   - the scan is strictly ascending (structural sanity), and
//   - the on-disk tree passes full structural verification (see
//     VerifyStructuralIntegrity below): backward sibling links, chain <->
//     tree agreement, no orphaned pages - the failure shapes of a half-
//     flushed multi-page op group.
//
// Run count: KVSTORE_CRASH_RUNS (default 25 for ctest; the C4 gate runs 500).
//
// ==============================================================================

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kvstore/btree/node.h"
#include "kvstore/db.h"
#include "kvstore/storage/file.h"
#include "kvstore/storage/page_format.h"
#include "kvstore/storage/pager.h"

namespace kvstore {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Structural integrity verifier (post-recovery, on the closed database).
//
// The sorted-scan check in the tests below only exercises the FORWARD leaf
// chain through reachable pages; the WAL op-group bug class (half a split on
// disk because a page was flushed before its group's op-end record was
// durable) shows up as a broken BACKWARD chain, a leaf reachable from the
// chain but not the root (or vice versa), or a non-zeroed page no structure
// references. Verify all of it directly against the page format:
//   - every tree edge points at a valid in-range page, visited exactly once,
//   - keys are strictly ascending within and across leaves,
//   - the sibling chain (walked forward from the leftmost leaf) visits
//     exactly the tree's leaves in order, with prev mirroring next,
//   - every data page is reachable from the root, on the free list, or
//     still zeroed (allocated, never written).
// Call with the DB closed (the pager needs the file).
// ---------------------------------------------------------------------------
::testing::AssertionResult VerifyStructuralIntegrity(const fs::path& dir) {
    auto pager_result = storage::Pager::Open((dir / "data.db").string(),
                                             /*create_if_missing=*/false);
    if (!pager_result) {
        return ::testing::AssertionFailure()
               << "pager open failed: " << pager_result.error().message();
    }
    auto& pager = *pager_result;
    const PageId num_pages = pager.num_pages();
    const PageId root = pager.root_page_id();

    std::vector<char> reachable(num_pages, 0);
    std::vector<PageId> leaves_in_order;

    // Depth-first, children in key order, so leaves come out in tree order.
    // Iterative: (page, next child index) stack.
    if (root != kInvalidPageId) {
        if (root < storage::Pager::kFirstDataPage || root >= num_pages) {
            return ::testing::AssertionFailure()
                   << "root " << root << " out of range (num_pages "
                   << num_pages << ")";
        }
        std::vector<PageId> stack{root};
        while (!stack.empty()) {
            const PageId id = stack.back();
            stack.pop_back();
            if (id < storage::Pager::kFirstDataPage || id >= num_pages) {
                return ::testing::AssertionFailure()
                       << "edge to out-of-range page " << id;
            }
            if (reachable[id]) {
                return ::testing::AssertionFailure()
                       << "page " << id << " referenced twice (cycle or "
                          "shared child)";
            }
            reachable[id] = 1;

            storage::Page page;
            if (Status s = pager.ReadPage(id, page); !s.ok()) {
                return ::testing::AssertionFailure()
                       << "read page " << id << ": " << s.message();
            }
            btree::NodeView node(page);
            if (node.IsLeaf()) {
                leaves_in_order.push_back(id);
                continue;
            }
            // Push children in REVERSE key order so they pop in key order.
            const PageId rightmost = node.GetRightmostChild();
            if (rightmost == kInvalidPageId) {
                return ::testing::AssertionFailure()
                       << "internal page " << id << " has no rightmost child";
            }
            stack.push_back(rightmost);
            for (uint16_t i = node.NumKeys(); i-- > 0;) {
                stack.push_back(node.GetChild(i));
            }
        }
    }

    // Key ORDER is verified along the sibling chain below; here only the
    // leaf SET matters (chain and tree must agree on membership).
    std::vector<char> is_tree_leaf(num_pages, 0);
    for (PageId id : leaves_in_order) {
        is_tree_leaf[id] = 1;
    }

    // Walk the sibling chain forward from the leftmost leaf (descend via
    // child 0 / rightmost when keyless).
    size_t chain_len = 0;
    if (root != kInvalidPageId) {
        PageId current = root;
        storage::Page page;
        while (true) {
            if (Status s = pager.ReadPage(current, page); !s.ok()) {
                return ::testing::AssertionFailure()
                       << "descend to leftmost: page " << current << ": "
                       << s.message();
            }
            btree::NodeView node(page);
            if (node.IsLeaf()) {
                break;
            }
            current = node.NumKeys() > 0 ? node.GetChild(0)
                                         : node.GetRightmostChild();
        }

        PageId prev_expected = kInvalidPageId;
        std::string last_key;
        bool have_last_key = false;
        while (current != kInvalidPageId) {
            if (current < storage::Pager::kFirstDataPage ||
                current >= num_pages) {
                return ::testing::AssertionFailure()
                       << "chain points at out-of-range page " << current;
            }
            if (++chain_len > leaves_in_order.size()) {
                return ::testing::AssertionFailure()
                       << "chain longer than the tree's leaf count "
                       << leaves_in_order.size() << " (cycle, or a leaf "
                          "unreachable from the root - half a split?)";
            }
            if (!is_tree_leaf[current]) {
                return ::testing::AssertionFailure()
                       << "chain visits page " << current
                       << " which is not a leaf reachable from the root";
            }
            if (Status s = pager.ReadPage(current, page); !s.ok()) {
                return ::testing::AssertionFailure()
                       << "chain read " << current << ": " << s.message();
            }
            btree::NodeView node(page);
            if (node.GetPrevLeaf() != prev_expected) {
                return ::testing::AssertionFailure()
                       << "leaf " << current << " prev=" << node.GetPrevLeaf()
                       << ", expected " << prev_expected
                       << " (backward chain broken - half a split?)";
            }
            for (uint16_t i = 0; i < node.NumKeys(); ++i) {
                std::string key(node.GetKey(i));
                if (have_last_key && key <= last_key) {
                    return ::testing::AssertionFailure()
                           << "keys not strictly ascending at leaf " << current
                           << " (" << key << " after " << last_key << ")";
                }
                last_key = std::move(key);
                have_last_key = true;
            }
            prev_expected = current;
            current = node.GetNextLeaf();
        }
    }
    if (chain_len != leaves_in_order.size()) {
        return ::testing::AssertionFailure()
               << "chain visits " << chain_len << " leaves but the tree has "
               << leaves_in_order.size()
               << " (leaf stranded outside the chain)";
    }

    // Orphan check: every data page must be tree-reachable, free-listed, or
    // still zeroed. A non-zeroed unreachable page is exactly what the
    // op-group/WAL-rule bug leaves behind.
    for (PageId id = storage::Pager::kFirstDataPage; id < num_pages; ++id) {
        if (reachable[id]) {
            continue;
        }
        storage::Page page;
        if (Status s = pager.ReadPage(id, page); !s.ok()) {
            return ::testing::AssertionFailure()
                   << "read page " << id << ": " << s.message();
        }
        if (page.IsZeroed() || page.type() == storage::PageType::kFree) {
            continue;
        }
        return ::testing::AssertionFailure()
               << "page " << id << " is written but unreachable from the "
                  "root (orphaned half of a structural change?)";
    }
    return ::testing::AssertionSuccess();
}

struct Op {
    bool is_put;
    std::string key;
    std::string value;  // Empty for deletes
};

// Deterministic op stream: the parent and child derive the same sequence.
std::vector<Op> GenerateOps(uint32_t seed, int count) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> key_pick(0, 599);
    std::uniform_int_distribution<int> value_len(20, 400);
    std::uniform_int_distribution<int> op_roll(0, 9);

    std::vector<Op> ops;
    ops.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        Op op;
        op.key = std::format("key{:04d}", key_pick(rng));
        op.is_put = op_roll(rng) < 7;  // 70% puts, 30% deletes
        if (op.is_put) {
            op.value = std::format("v{:08x}-", rng()) +
                       std::string(static_cast<size_t>(value_len(rng)), 'x');
        }
        ops.push_back(std::move(op));
    }
    return ops;
}

void ApplyToModel(std::map<std::string, std::string>& model, const Op& op) {
    if (op.is_put) {
        model[op.key] = op.value;
    } else {
        model.erase(op.key);  // Deleting an absent key is a NotFound no-op
    }
}

// Child body: never returns. Exit codes: 0 = finished all ops, 3 = crashed
// at the armed fault (the expected case), 4 = setup failure.
[[noreturn]] void RunChild(const fs::path& dir, uint32_t seed, int op_count) {
    Options options;
    options.create_if_missing = true;
    options.sync_on_write = true;    // Acked == committed
    options.buffer_pool_frames = 8;  // Constant eviction pressure
    options.max_wal_bytes = 32 * 1024;  // Auto-checkpoints in the blast zone

    auto open_result = DB::Open(dir.string(), options);
    if (!open_result) {
        _exit(4);
    }
    auto db = std::move(*open_result);

    std::FILE* confirm = std::fopen((dir / "confirmed.txt").c_str(), "w");
    if (confirm == nullptr) {
        _exit(4);
    }

    const auto ops = GenerateOps(seed, op_count);

    // Seeded fault: fires inside a Put/Delete/checkpoint at any depth.
    std::mt19937 fault_rng(seed ^ 0x5EEDF00Du);
    storage::PosixFile::FailAfterNWrites(
        std::uniform_int_distribution<int64_t>(1, 300)(fault_rng));

    for (const auto& op : ops) {
        Status s = op.is_put ? db->Put(op.key, op.value) : db->Delete(op.key);
        if (!s.ok() && s.code() != StatusCode::kNotFound) {
            _exit(3);  // "Crash": no cleanup, no destructor checkpoint
        }
        std::fputs("ok\n", confirm);
        std::fflush(confirm);
    }
    std::fclose(confirm);
    _exit(0);  // Survived every op (fault countdown outlived the workload)
}

int CountConfirmedOps(const fs::path& dir) {
    std::FILE* confirm = std::fopen((dir / "confirmed.txt").c_str(), "r");
    if (confirm == nullptr) {
        return 0;
    }
    int lines = 0;
    int ch;
    while ((ch = std::fgetc(confirm)) != EOF) {
        if (ch == '\n') {
            ++lines;
        }
    }
    std::fclose(confirm);
    return lines;
}

TEST(CrashTest, SeededCrashRunsRecoverEveryCommittedOp) {
    int runs = 25;
    if (const char* env = std::getenv("KVSTORE_CRASH_RUNS");
        env != nullptr && std::atoi(env) > 0) {
        runs = std::atoi(env);
    }
    constexpr int kOpsPerRun = 400;

    const fs::path base =
        fs::path(::testing::TempDir()) / "kvstore_crash_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);

    int crashed_runs = 0;
    for (int run = 0; run < runs; ++run) {
        const uint32_t seed = 20260705u + static_cast<uint32_t>(run);
        const fs::path dir = base / std::format("run{:04d}", run);

        pid_t pid = fork();
        ASSERT_GE(pid, 0) << "fork failed";
        if (pid == 0) {
            RunChild(dir, seed, kOpsPerRun);  // Never returns
        }

        int wstatus = 0;
        ASSERT_EQ(waitpid(pid, &wstatus, 0), pid);
        ASSERT_TRUE(WIFEXITED(wstatus)) << "run " << run << ": child killed?";
        const int exit_code = WEXITSTATUS(wstatus);
        ASSERT_TRUE(exit_code == 0 || exit_code == 3)
            << "run " << run << ": child setup failed (" << exit_code << ")";
        if (exit_code == 3) {
            ++crashed_runs;
        }

        // Rebuild the committed prefix from the confirmation count.
        const auto ops = GenerateOps(seed, kOpsPerRun);
        const int confirmed = CountConfirmedOps(dir);
        ASSERT_LE(confirmed, kOpsPerRun) << "run " << run;

        std::map<std::string, std::string> committed;
        for (int i = 0; i < confirmed; ++i) {
            ApplyToModel(committed, ops[static_cast<size_t>(i)]);
        }
        // The op in flight at the fault may or may not have committed.
        std::map<std::string, std::string> with_in_flight = committed;
        if (confirmed < kOpsPerRun) {
            ApplyToModel(with_in_flight, ops[static_cast<size_t>(confirmed)]);
        }

        // Reopen: recovery runs here.
        Options options;
        options.create_if_missing = false;
        auto reopened = DB::Open(dir.string(), options);
        ASSERT_TRUE(reopened.has_value())
            << "run " << run << " (seed " << seed << ", confirmed "
            << confirmed << "): " << reopened.error().message();
        auto db = std::move(*reopened);

        std::map<std::string, std::string> state;
        std::string last;
        ASSERT_TRUE(db->Scan("", "",
                             [&](std::string_view key, std::string_view value) {
                                 EXPECT_LT(last, std::string(key))
                                     << "run " << run << ": unsorted scan";
                                 last = std::string(key);
                                 state.emplace(key, value);
                                 return true;
                             })
                        .ok())
            << "run " << run;

        const bool matches =
            (state == committed) || (state == with_in_flight);
        ASSERT_TRUE(matches)
            << "run " << run << " (seed " << seed << ", confirmed "
            << confirmed << "): state has " << state.size()
            << " keys, committed model has " << committed.size();

        // Point reads agree with the scan for the committed prefix.
        for (const auto& [key, value] : committed) {
            if (state.contains(key)) {
                auto result = db->Get(key);
                ASSERT_TRUE(result.has_value()) << "run " << run << " " << key;
            }
        }

        db.reset();
        EXPECT_TRUE(VerifyStructuralIntegrity(dir))
            << "run " << run << " (seed " << seed << ")";
        fs::remove_all(dir, ec);
    }

    // The harness is only meaningful if faults actually fired.
    EXPECT_GT(crashed_runs, runs / 2)
        << "fault hook fired too rarely - workload/fault tuning drifted";
}

// ==============================================================================
// Phase 5.4: transaction losers must be undone by recovery
// ==============================================================================

// Deterministic: commit a baseline transaction, then run a big in-flight
// transaction that writes (and, with an 8-frame pool, evicts its dirty pages
// to disk under the write-ahead rule) but never commits. Sync makes its WAL
// records durable, then _exit skips the destructor's checkpoint - a clean
// stand-in for a crash mid-transaction. Recovery must keep the committed keys
// and erase every loser key.
//
// `checkpoint_mid_txn` fires an explicit Checkpoint() while the loser is in
// flight: the fuzzy-checkpoint rule says it must flush pages but KEEP the log
// (truncating would discard the very records the undo below depends on) - if
// that rule ever regresses, the loser keys survive recovery and this fails.
[[noreturn]] void RunTxnLoserChild(const fs::path& dir, int committed_keys,
                                   int loser_keys, bool checkpoint_mid_txn) {
    Options options;
    options.create_if_missing = true;
    options.sync_on_write = true;
    options.buffer_pool_frames = 8;    // Force steal: loser pages reach disk
    options.max_wal_bytes = 0;         // No auto-checkpoint; keep loser records

    auto open_result = DB::Open(dir.string(), options);
    if (!open_result) _exit(4);
    auto db = std::move(*open_result);

    {
        auto begin = db->Begin();
        if (!begin) _exit(4);
        auto txn = std::move(*begin);
        for (int i = 0; i < committed_keys; ++i) {
            if (!txn->Put(std::format("c{:06d}", i), "committed").ok()) _exit(4);
        }
        if (!txn->Commit().ok()) _exit(4);
    }

    auto begin = db->Begin();
    if (!begin) _exit(4);
    auto loser = std::move(*begin);  // Held past _exit: never committed/aborted
    for (int i = 0; i < loser_keys; ++i) {
        if (!loser->Put(std::format("L{:06d}", i), "loser").ok()) _exit(4);
    }
    if (checkpoint_mid_txn && !db->Checkpoint().ok()) _exit(4);
    if (!db->Sync().ok()) _exit(4);   // Loser's kBegin+kPut records now durable
    _exit(0);                          // No kCommit, no shutdown checkpoint
}

TEST(CrashTest, InFlightTransactionIsUndoneAfterCrash) {
    const fs::path base = fs::path(::testing::TempDir()) / "kvstore_crash_txn";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);

    struct Sizes { int committed; int loser; bool ckpt_mid_txn; };
    for (const Sizes sz : {Sizes{50, 400, false}, Sizes{200, 900, false},
                           Sizes{5, 5, false}, Sizes{50, 400, true}}) {
        const fs::path dir = base / std::format("c{}_l{}_k{:d}", sz.committed,
                                                sz.loser, sz.ckpt_mid_txn);

        pid_t pid = fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            RunTxnLoserChild(dir, sz.committed, sz.loser,
                             sz.ckpt_mid_txn);  // never returns
        }
        int wstatus = 0;
        ASSERT_EQ(waitpid(pid, &wstatus, 0), pid);
        ASSERT_TRUE(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0)
            << "child exit " << WEXITSTATUS(wstatus);

        Options options;
        options.create_if_missing = false;
        auto reopened = DB::Open(dir.string(), options);
        ASSERT_TRUE(reopened.has_value()) << reopened.error().message();
        auto db = std::move(*reopened);

        for (int i = 0; i < sz.committed; ++i) {
            EXPECT_TRUE(db->Get(std::format("c{:06d}", i)).has_value())
                << "committed key " << i << " lost";
        }
        for (int i = 0; i < sz.loser; ++i) {
            EXPECT_FALSE(db->Get(std::format("L{:06d}", i)).has_value())
                << "loser key " << i << " survived";
        }
        EXPECT_EQ(db->ApproximateKeyCount(),
                  static_cast<uint64_t>(sz.committed));
        db.reset();
        EXPECT_TRUE(VerifyStructuralIntegrity(dir))
            << "c" << sz.committed << "_l" << sz.loser;
        fs::remove_all(dir, ec);
    }
}

// Randomized: a stream of transactions, each committing atomically, with the
// torn-write fault armed at a seeded countdown. The transaction in flight when
// the fault fires is a loser - its keys must be absent after recovery, leaving
// exactly the committed prefix (or that prefix plus the single txn whose commit
// record may have flushed just before the fault).
struct TxnSpec {
    std::vector<std::pair<std::string, std::string>> writes;
};

std::vector<TxnSpec> GenerateTxns(uint32_t seed, int count) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> key_pick(0, 199);
    std::uniform_int_distribution<int> per_txn(1, 6);
    std::uniform_int_distribution<int> value_len(10, 200);

    std::vector<TxnSpec> txns;
    txns.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        TxnSpec t;
        const int n = per_txn(rng);
        for (int k = 0; k < n; ++k) {
            t.writes.emplace_back(
                std::format("k{:04d}", key_pick(rng)),
                std::format("v{:08x}-", rng()) +
                    std::string(static_cast<size_t>(value_len(rng)), 'y'));
        }
        txns.push_back(std::move(t));
    }
    return txns;
}

[[noreturn]] void RunTxnStreamChild(const fs::path& dir, uint32_t seed,
                                    int txn_count) {
    Options options;
    options.create_if_missing = true;
    options.sync_on_write = true;
    options.buffer_pool_frames = 8;
    options.max_wal_bytes = 32 * 1024;

    auto open_result = DB::Open(dir.string(), options);
    if (!open_result) _exit(4);
    auto db = std::move(*open_result);

    std::FILE* confirm = std::fopen((dir / "confirmed.txt").c_str(), "w");
    if (confirm == nullptr) _exit(4);

    const auto txns = GenerateTxns(seed, txn_count);
    std::mt19937 fault_rng(seed ^ 0x7A11FEEDu);
    storage::PosixFile::FailAfterNWrites(
        std::uniform_int_distribution<int64_t>(1, 350)(fault_rng));

    for (const auto& t : txns) {
        auto begin = db->Begin();
        if (!begin) _exit(3);
        auto txn = std::move(*begin);
        for (const auto& [key, value] : t.writes) {
            if (!txn->Put(key, value).ok()) _exit(3);
        }
        if (!txn->Commit().ok()) _exit(3);
        std::fputs("ok\n", confirm);  // This transaction committed
        std::fflush(confirm);
    }
    std::fclose(confirm);
    _exit(0);
}

TEST(CrashTest, SeededTransactionCrashesUndoLosers) {
    int runs = 25;
    if (const char* env = std::getenv("KVSTORE_CRASH_RUNS");
        env != nullptr && std::atoi(env) > 0) {
        runs = std::atoi(env);
    }
    constexpr int kTxnsPerRun = 300;

    const fs::path base =
        fs::path(::testing::TempDir()) / "kvstore_crash_txn_stream";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);

    int crashed_runs = 0;
    for (int run = 0; run < runs; ++run) {
        const uint32_t seed = 20260707u + static_cast<uint32_t>(run);
        const fs::path dir = base / std::format("run{:04d}", run);

        pid_t pid = fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            RunTxnStreamChild(dir, seed, kTxnsPerRun);  // never returns
        }
        int wstatus = 0;
        ASSERT_EQ(waitpid(pid, &wstatus, 0), pid);
        ASSERT_TRUE(WIFEXITED(wstatus));
        const int exit_code = WEXITSTATUS(wstatus);
        ASSERT_TRUE(exit_code == 0 || exit_code == 3)
            << "run " << run << ": setup failed (" << exit_code << ")";
        if (exit_code == 3) ++crashed_runs;

        const auto txns = GenerateTxns(seed, kTxnsPerRun);
        const int confirmed = CountConfirmedOps(dir);  // committed txn count
        ASSERT_LE(confirmed, kTxnsPerRun);

        auto apply_txn = [](std::map<std::string, std::string>& m,
                            const TxnSpec& t) {
            for (const auto& [key, value] : t.writes) m[key] = value;
        };
        std::map<std::string, std::string> committed;
        for (int i = 0; i < confirmed; ++i) {
            apply_txn(committed, txns[static_cast<size_t>(i)]);
        }
        std::map<std::string, std::string> with_in_flight = committed;
        if (confirmed < kTxnsPerRun) {
            apply_txn(with_in_flight, txns[static_cast<size_t>(confirmed)]);
        }

        Options options;
        options.create_if_missing = false;
        auto reopened = DB::Open(dir.string(), options);
        ASSERT_TRUE(reopened.has_value())
            << "run " << run << " (seed " << seed << ", confirmed " << confirmed
            << "): " << reopened.error().message();
        auto db = std::move(*reopened);

        std::map<std::string, std::string> state;
        std::string last;
        ASSERT_TRUE(db->Scan("", "",
                             [&](std::string_view key, std::string_view value) {
                                 EXPECT_LT(last, std::string(key))
                                     << "run " << run << ": unsorted";
                                 last = std::string(key);
                                 state.emplace(key, value);
                                 return true;
                             })
                        .ok());

        const bool matches =
            (state == committed) || (state == with_in_flight);
        ASSERT_TRUE(matches)
            << "run " << run << " (seed " << seed << ", confirmed " << confirmed
            << "): state has " << state.size() << " keys, committed model has "
            << committed.size();
        db.reset();
        EXPECT_TRUE(VerifyStructuralIntegrity(dir))
            << "run " << run << " (seed " << seed << ")";
        fs::remove_all(dir, ec);
    }
    EXPECT_GT(crashed_runs, runs / 2)
        << "fault hook fired too rarely - workload/fault tuning drifted";
}

// ==============================================================================
// Phase 5 / C5 gate: crash with CONCURRENT writers
// ==============================================================================
//
// The child runs several writer threads (autocommit Puts through the full
// DbImpl path: latch crabbing, shared WAL, auto-checkpoints) with the
// torn-write fault armed. Each thread confirms every acknowledged op to its
// own side file, and every (thread, i) op targets a UNIQUE key, so the parent
// can verify per thread:
//   - every confirmed key is present with its exact value (committed ==
//     durable, even when the WAL interleaves 4 writers' records), and
//   - any unconfirmed key that survives is that thread's single in-flight op
//     (acknowledged-or-not is genuinely ambiguous only for op[confirmed]).

constexpr int kMtThreads = 4;

std::string MtKey(int thread_id, int i) {
    return std::format("mt{}-{:06d}", thread_id, i);
}
std::string MtValue(int thread_id, int i) {
    return std::format("v{}-{}", thread_id, i * 7919);
}

[[noreturn]] void RunConcurrentChild(const fs::path& dir, uint32_t seed,
                                     int ops_per_thread) {
    Options options;
    options.create_if_missing = true;
    options.sync_on_write = true;       // Acked == committed
    // Eviction pressure across 4 writers, with headroom for the op-group
    // pins: a splitting writer holds path + rewritten pages (up to
    // 2*depth+2) until its op-end record is appended, so 4 concurrent
    // splitters need ~28 frames before Busy.
    options.buffer_pool_frames = 32;
    options.max_wal_bytes = 64 * 1024;  // Auto-checkpoints in the blast zone

    auto open_result = DB::Open(dir.string(), options);
    if (!open_result) _exit(4);
    auto db = std::move(*open_result);

    std::mt19937 fault_rng(seed ^ 0xC0FFEE42u);
    storage::PosixFile::FailAfterNWrites(
        std::uniform_int_distribution<int64_t>(50, 1200)(fault_rng));

    std::vector<std::thread> threads;
    for (int t = 0; t < kMtThreads; ++t) {
        threads.emplace_back([&db, &dir, t, ops_per_thread] {
            std::FILE* confirm = std::fopen(
                (dir / std::format("confirmed{}.txt", t)).c_str(), "w");
            if (confirm == nullptr) _exit(4);
            for (int i = 0; i < ops_per_thread; ++i) {
                if (!db->Put(MtKey(t, i), MtValue(t, i)).ok()) {
                    std::fclose(confirm);
                    return;  // Fault fired (here or in a sibling thread)
                }
                std::fputs("ok\n", confirm);
                std::fflush(confirm);
            }
            std::fclose(confirm);
        });
    }
    for (auto& th : threads) th.join();
    // Exit code only distinguishes setup failure; the parent decides
    // crashed-vs-finished from the confirmation counts.
    _exit(0);
}

TEST(CrashTest, ConcurrentWritersRecoverEveryCommittedOp) {
    int runs = 15;
    if (const char* env = std::getenv("KVSTORE_CRASH_RUNS");
        env != nullptr && std::atoi(env) > 0) {
        runs = std::max(1, std::atoi(env) / 2);  // Each run is ~4x the work
    }
    constexpr int kOpsPerThread = 400;

    const fs::path base =
        fs::path(::testing::TempDir()) / "kvstore_crash_concurrent";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);

    int crashed_runs = 0;
    for (int run = 0; run < runs; ++run) {
        const uint32_t seed = 20260708u + static_cast<uint32_t>(run);
        const fs::path dir = base / std::format("run{:04d}", run);
        fs::create_directories(dir);  // Threads write confirm files here

        pid_t pid = fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            RunConcurrentChild(dir, seed, kOpsPerThread);  // never returns
        }
        int wstatus = 0;
        ASSERT_EQ(waitpid(pid, &wstatus, 0), pid);
        ASSERT_TRUE(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0)
            << "run " << run << ": child setup failed";

        std::array<int, kMtThreads> confirmed{};
        bool all_finished = true;
        for (int t = 0; t < kMtThreads; ++t) {
            std::FILE* f = std::fopen(
                (dir / std::format("confirmed{}.txt", t)).c_str(), "r");
            int lines = 0;
            if (f != nullptr) {
                int ch;
                while ((ch = std::fgetc(f)) != EOF) {
                    if (ch == '\n') ++lines;
                }
                std::fclose(f);
            }
            confirmed[static_cast<size_t>(t)] = lines;
            if (lines < kOpsPerThread) all_finished = false;
        }
        if (!all_finished) ++crashed_runs;

        Options options;
        options.create_if_missing = false;
        auto reopened = DB::Open(dir.string(), options);
        ASSERT_TRUE(reopened.has_value())
            << "run " << run << " (seed " << seed
            << "): " << reopened.error().message();
        auto db = std::move(*reopened);

        // Committed == durable, per thread.
        for (int t = 0; t < kMtThreads; ++t) {
            for (int i = 0; i < confirmed[static_cast<size_t>(t)]; ++i) {
                auto got = db->Get(MtKey(t, i));
                ASSERT_TRUE(got.has_value())
                    << "run " << run << ": confirmed key " << MtKey(t, i)
                    << " lost";
                EXPECT_EQ(*got, MtValue(t, i));
            }
        }

        // No phantom writes: anything beyond the confirmed prefix is at most
        // the one op each thread had in flight, with the correct value.
        std::string last;
        uint64_t key_count = 0;
        ASSERT_TRUE(
            db->Scan("", "",
                     [&](std::string_view key, std::string_view value) {
                         EXPECT_LT(last, std::string(key))
                             << "run " << run << ": unsorted scan";
                         last = std::string(key);
                         ++key_count;
                         int t = -1, i = -1;
                         if (std::sscanf(std::string(key).c_str(), "mt%d-%d",
                                         &t, &i) != 2 ||
                             t < 0 || t >= kMtThreads || i < 0) {
                             ADD_FAILURE() << "run " << run
                                           << ": foreign key " << key;
                             return true;
                         }
                         EXPECT_LE(i, confirmed[static_cast<size_t>(t)])
                             << "run " << run << ": key " << key
                             << " beyond thread " << t << "'s in-flight window";
                         EXPECT_EQ(value, MtValue(t, i)) << "run " << run;
                         return true;
                     })
                .ok())
            << "run " << run;
        EXPECT_EQ(db->ApproximateKeyCount(), key_count) << "run " << run;

        db.reset();
        EXPECT_TRUE(VerifyStructuralIntegrity(dir))
            << "run " << run << " (seed " << seed << ")";
        fs::remove_all(dir, ec);
    }
    EXPECT_GT(crashed_runs, runs / 2)
        << "fault hook fired too rarely - workload/fault tuning drifted";
}

}  // namespace
}  // namespace kvstore
