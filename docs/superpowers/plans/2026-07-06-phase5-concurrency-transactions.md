# Phase 5 — Concurrency & Transactions (5.2–5.5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the single-writer, autocommit PagerDB engine into a concurrent, transactional one — a lock-safe B+ tree (latch crabbing), an explicit `Txn` handle, before-image undo (in-session `Abort` + recovery `redo-all + undo-losers`), and a strict 2PL lock manager giving **serializable** isolation for point operations (Checkpoint C5).

**Architecture:** A new `txn/` module (`TxnManager` owning a `LockManager`, plus a `Txn` handle) sits above the existing `BPlusTree`. Transactions hold per-key S/X locks (rigorous 2PL, wait-die) with **no page latches held while waiting on a lock**; the tree beneath them is made thread-safe by exclusive/shared latch crabbing anchored on a pool-owned `root_latch`. Undo is logical (absolute put/delete) driven by before-images carried in the WAL `kPut`/`kDelete` records; recovery redoes every complete op-group then undoes the losers.

**Tech Stack:** C++23, `std::expected` error model (`Status`/`Result<T>`, no exceptions), GoogleTest, CMake presets (`asan`, `tsan`), POSIX file I/O.

## Global Constraints

- **NO git commits.** Every task ends with a **Verify & checkpoint** step that builds and runs tests — it does **not** commit. All work stays uncommitted in the working tree; the user commits manually. The `v0.5-phase5` tag is deferred (same as C1–C6). This overrides the writing-plans/executing-plans default of committing per task.
- **C++23**, single translation unit per `.cc`, headers under `include/kvstore/`, sources under `src/`. Match existing file style: banner comment header, `[[nodiscard("reason")]]`, `Status`/`Result<T>`, `std::expected`, no exceptions, `namespace kvstore::...`.
- **Error model:** return `Status`/`Result<T>`; propagate the original `Status`. Deadlock victim / lock contention returns `StatusCode::kBusy` (`Status::Busy(...)` / `ErrBusy(...)`).
- **Concurrency-correctness gates (the teeth of C5):**
  - Non-concurrent correctness under **asan**: `cmake --preset asan && cmake --build build/asan -j && ctest --preset asan`.
  - Concurrency tests under **tsan** (asan and tsan cannot be combined): `cmake --preset tsan && cmake --build build/tsan -j && ctest --preset tsan`.
  - Run a single test: `ctest --preset asan -R <name> --output-on-failure` (or `./build/asan/<name>`).
- **Deadlock-freedom invariant (§8 of the spec) — never violate:** a statement acquires its key lock **first, holding no page latches**, then runs the latched tree op. Two orthogonal mechanisms: **wait-die** for logical locks, **crabbing order** (`root_latch` → tree path top-down; leaf siblings only rightward) for physical latches.
- **No-merge/no-free invariant (decision 2.8) — relied on throughout:** tree pages are never freed or merged; splits only ADD pages. Therefore a child PageId read from a parent never dangles, deletes never modify internal nodes, and `root_page_id` changes only on first-insert or root split.
- **Scope:** implement 5.2–5.5. **5.6 MVCC is DEFERRED** (stretch goal). **Scans take no range/predicate locks** (read-committed-per-leaf) — documented, not fixed here.

---

## File Structure

**New files:**

| File | Responsibility |
|---|---|
| `include/kvstore/txn/lock_manager.h` / `src/txn/lock_manager.cc` | Per-key S/X `LockManager`: `Acquire(txid,key,mode)` (wait-die), `ReleaseAll(txid)`. |
| `include/kvstore/txn/txn.h` / `src/txn/txn.cc` | `TxnManager` (txid counter, owns `LockManager`, autocommit helpers) and `Txn` (Begin/Get/Put/Delete/Commit/Abort + undo stack). |
| `tests/btree_concurrent_test.cc` | 5.2 latch crabbing under tsan (8-thread mixed workload). |
| `tests/lock_manager_test.cc` | 5.5 lock manager unit tests (grant/conflict/upgrade/wait-die). |
| `tests/txn_test.cc` | 5.3/5.4 transaction semantics (commit/abort/autocommit). |
| `tests/txn_recovery_test.cc` | 5.4 crash recovery (redo-all + undo-losers). |
| `tests/anomaly_test.cc` | 5.5 isolation anomalies (dirty read / lost update / write-write). |

**Modified files:**

| File | Change |
|---|---|
| `include/kvstore/storage/buffer_pool.h` / `src/storage/buffer_pool.cc` | Add `std::shared_mutex root_latch_` + `root_latch()` accessor. |
| `include/kvstore/wal/log_record.h` / `src/wal/log_record.cc` | Add `kBegin/kCommit/kAbort`; add `before` field + `before_len:2` encoding; grow `kHeaderSize`. |
| `include/kvstore/btree/btree.h` / `src/btree/btree.cc` | Latch-crabbing Get/Delete/Put/Seek/SeekToFirst/FindLeaf; fix `SplitLeaf` sibling latch; thread `TxId txid` + before-image through Put/Delete + WAL helpers; drop dead `FindLeafPath`. |
| `src/btree/cursor.cc` | `FetchPage` → `FetchPageRead`. |
| `include/kvstore/db.h` | Include `txn.h`; add `virtual std::unique_ptr<Txn> Begin() = 0;`. |
| `src/db.cc` | Construct `LockManager` + `TxnManager`; route autocommit Get/Put/Delete; implement `Begin()`; rewrite `Recover` (redo-all + undo-losers); drop `needs_reopen_`. |
| `CMakeLists.txt` | Add `src/txn/*.cc` to `KVSTORE_SOURCES`; add the 5 new tests to `KVSTORE_TESTS`. |
| Docs: `docs/ARCHITECTURE.md`, `docs/DESIGN_DECISIONS.md`, `docs/FORMAT.md`, `README.md`, `phases.md` | Record the concurrency lifecycle, decisions, WAL format, status, and mark 5.2–5.5 done / C5 / 5.6 deferred. |
| `.github/workflows/ci.yml` | Ensure a tsan job runs the concurrency suite. |

---

## Task 1: Latch crabbing — make the B+ tree thread-safe (5.2)

Make every tree operation safe under concurrent readers and a serialized writer, using shared/exclusive content latches (5.1 primitives) coupled top-down, anchored on a new pool-owned `root_latch`.

**Files:**
- Modify: `include/kvstore/storage/buffer_pool.h` (add member + accessor), `src/storage/buffer_pool.cc` (nothing required beyond the header inline; the member default-constructs)
- Modify: `src/btree/btree.cc` (rewrite `Get`, `Delete`, `Put`, `Seek`, `SeekToFirst`, `FindLeaf`; fix `SplitLeaf`; remove `FindLeafPath`)
- Modify: `include/kvstore/btree/btree.h` (remove `FindLeafPath` + `PathEntry` declarations)
- Modify: `src/btree/cursor.cc` (`FetchPage` → `FetchPageRead`)
- Create: `tests/btree_concurrent_test.cc`
- Modify: `CMakeLists.txt` (register `btree_concurrent_test`)

**Interfaces:**
- Consumes: `BufferPool::FetchPageRead/FetchPageWrite/NewPage` → `Result<PageGuard>` (5.1); `PageGuard` (move-only; `page()`, `page_id()`, `MarkDirty()`, `Drop()`, move-assign drops the old latch first); `NodeView::{IsLeaf,FindChild,FindKeyIndex,CanFit,UpdateLeaf,DeleteAt,InsertLeaf,GetValue,InsertInternalAfterSplit}`; `pool_.pager().{root_page_id,SetRootPageId,num_keys,SetNumKeys}`.
- Produces:
  - `std::shared_mutex& BufferPool::root_latch()` — outermost lock guarding `root_page_id`; writers hold **exclusive**, readers/deletes hold **shared**.
  - Thread-safe `BPlusTree::{Get,Put,Delete,Contains,Seek,SeekToFirst}` (unchanged signatures this task).

- [ ] **Step 1: Write the failing concurrent test**

Create `tests/btree_concurrent_test.cc`:

```cpp
// ==============================================================================
// tests/btree_concurrent_test.cc - Latch-crabbing stress (Phase 5.2)
// ==============================================================================
// Run under tsan: cmake --preset tsan && cmake --build build/tsan -j
//                 ctest --preset tsan -R btree_concurrent_test
// ==============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kvstore/btree/btree.h"
#include "kvstore/storage/buffer_pool.h"
#include "kvstore/storage/pager.h"

namespace {

using namespace kvstore;

struct TreeFixture {
    std::string dir;
    std::optional<storage::Pager> pager;
    std::optional<storage::BufferPool> pool;
    std::optional<btree::BPlusTree> tree;

    explicit TreeFixture(const std::string& d) : dir(d) {
        std::filesystem::create_directories(dir);
        pager.emplace(storage::Pager::Open((std::filesystem::path(dir) / "data.db").string(),
                                           /*create=*/true, /*verify=*/true).value());
        pool.emplace(*pager, /*num_frames=*/128, /*wal=*/nullptr);
        tree.emplace(*pool, /*wal=*/nullptr);
    }
};

std::string Key(int thread_id, int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "t%02d-k%08d", thread_id, i);
    return buf;
}

// (a) Disjoint key ranges: each thread owns a key namespace and validates its
// own writes against a private std::map — no lost/torn writes.
TEST(BTreeConcurrent, DisjointRangesRoundTrip) {
    TreeFixture fx("/tmp/kvstore_btree_concurrent_disjoint");
    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;

    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::map<std::string, std::string> ref;
            std::mt19937 rng(static_cast<unsigned>(t + 1));
            for (int i = 0; i < kPerThread; ++i) {
                std::string k = Key(t, static_cast<int>(rng() % 500));
                std::string v = std::to_string(rng());
                if (!fx.tree->Put(k, v).ok()) { ++failures; return; }
                ref[k] = v;
            }
            for (const auto& [k, v] : ref) {
                auto got = fx.tree->Get(k);
                if (!got || *got != v) { ++failures; return; }
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(failures.load(), 0);
}

// (b) Shared keys + concurrent readers: readers never observe a torn page
// (Get either finds a well-formed value or NotFound; never crashes / garbage).
TEST(BTreeConcurrent, SharedKeysReadersStayConsistent) {
    TreeFixture fx("/tmp/kvstore_btree_concurrent_shared");
    constexpr int kKeys = 400;
    for (int i = 0; i < kKeys; ++i) {
        ASSERT_TRUE(fx.tree->Put(Key(0, i), "v0").ok());
    }

    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 6; ++r) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                for (int i = 0; i < kKeys; ++i) {
                    auto got = fx.tree->Get(Key(0, i));
                    if (got && got->substr(0, 1) != "v") ++bad;  // garbage value
                }
            }
        });
    }
    std::thread writer([&] {
        for (int round = 0; round < 50; ++round) {
            for (int i = 0; i < kKeys; ++i) {
                (void)fx.tree->Put(Key(0, i), "v" + std::to_string(round));
            }
        }
        stop.store(true);
    });
    writer.join();
    for (auto& th : readers) th.join();
    EXPECT_EQ(bad.load(), 0);
}

}  // namespace
```

- [ ] **Step 2: Register the test and run it to verify it fails to build**

Add `btree_concurrent_test` to `KVSTORE_TESTS` in `CMakeLists.txt` (after `btree_property_test`):

```cmake
        btree_property_test
        btree_concurrent_test
        db_test
```

Run: `cmake --preset tsan && cmake --build build/tsan -j`
Expected: FAIL — `root_latch` not yet referenced by the tree, but the test builds; it will data-race / assert under tsan once run because Put/Get are not yet latched. (If the build passes, run `ctest --preset tsan -R btree_concurrent_test` and expect tsan data-race reports or a crash.)

- [ ] **Step 3: Add `root_latch` to the buffer pool**

In `include/kvstore/storage/buffer_pool.h`, add the accessor in the public section (near `pager()`, around line 113):

```cpp
    // Phase 5.2: the outermost tree lock, guarding pager().root_page_id().
    // Writers hold it EXCLUSIVE across a potential root change (first insert /
    // root split); readers and deletes hold it SHARED only long enough to read
    // the root id and latch the root node. Acquired before any page content
    // latch (crabbing order: root_latch -> tree path top-down).
    [[nodiscard("root_latch guards the root page id")]]
    std::shared_mutex& root_latch() noexcept { return root_latch_; }
```

And add the member in the `private:` data section (near `latch_`, around line 159):

```cpp
    std::shared_mutex root_latch_;  // Phase 5.2: guards pager root_page_id
```

- [ ] **Step 4: Rewrite `Get` as a read-crab descent**

Replace `BPlusTree::Get` (`src/btree/btree.cc` lines 150–177) with:

```cpp
Result<std::string> BPlusTree::Get(std::string_view key) {
    // Read-crab: hold root_latch shared only to read root_id + latch the root,
    // then release it and latch-couple down with SHARED latches (a read never
    // splits, so every parent is released the instant its child is latched).
    std::shared_lock<std::shared_mutex> root_lk(pool_.root_latch());
    if (IsEmpty()) [[unlikely]] {
        return std::unexpected(Status::NotFound("Key not found (empty tree)"));
    }
    PageId current = pool_.pager().root_page_id();
    auto g = pool_.FetchPageRead(current);
    if (!g) [[unlikely]] return std::unexpected(g.error());
    root_lk.unlock();  // root latched; no root change can slip past our latch

    storage::PageGuard guard = std::move(*g);
    while (true) {
        NodeView node(guard.page());
        if (node.IsLeaf()) break;
        PageId child = node.FindChild(key);
        if (child == kInvalidPageId) [[unlikely]] {
            return std::unexpected(Status::Internal("Invalid child pointer"));
        }
        auto cg = pool_.FetchPageRead(child);
        if (!cg) [[unlikely]] return std::unexpected(cg.error());
        guard = std::move(*cg);  // move-assign drops the parent latch, adopts child
    }

    NodeView node(guard.page());
    auto value_opt = node.GetValue(key);
    if (!value_opt.has_value()) [[unlikely]] {
        return std::unexpected(Status::NotFound("Key not found"));
    }
    return std::string(value_opt.value());
}
```

Add the needed include at the top of `src/btree/btree.cc` (after the existing includes):

```cpp
#include <mutex>
#include <shared_mutex>
```

- [ ] **Step 5: Rewrite `Delete` as an exclusive-crab descent**

Replace `BPlusTree::Delete` (lines 183–217) with:

```cpp
Status BPlusTree::Delete(std::string_view key) {
    // Deletes never split and never change root_page_id (decision 2.8: no
    // merge / page freeing), so every internal node is trivially "safe": take
    // root_latch SHARED just to read root_id + latch the root, release it, then
    // exclusive-latch-couple straight down, releasing each ancestor the instant
    // its child is latched. End holding only the target leaf exclusive.
    storage::PageGuard leaf_guard;
    {
        std::shared_lock<std::shared_mutex> root_lk(pool_.root_latch());
        if (IsEmpty()) [[unlikely]] return Status::NotFound("Key not found (empty tree)");
        PageId current = pool_.pager().root_page_id();
        auto g = pool_.FetchPageWrite(current);
        if (!g) [[unlikely]] return g.error();
        root_lk.unlock();

        storage::PageGuard guard = std::move(*g);
        while (true) {
            NodeView node(guard.page());
            if (node.IsLeaf()) break;
            PageId child = node.FindChild(key);
            if (child == kInvalidPageId) [[unlikely]] {
                return Status::Internal("Invalid child pointer");
            }
            auto cg = pool_.FetchPageWrite(child);
            if (!cg) [[unlikely]] return cg.error();
            guard = std::move(*cg);  // drops the parent latch (always safe), adopts child
        }
        leaf_guard = std::move(guard);
    }

    NodeView node(leaf_guard.page());
    bool found;
    uint16_t index = node.FindKeyIndex(key, found);
    if (!found) return Status::NotFound("Key not found");
    if (!node.DeleteAt(index)) [[unlikely]] {
        return Status::Internal("DeleteAt failed for a found key");
    }
    if (Status s = LogKeyRecord(wal::LogRecordType::kDelete, leaf_guard, key, {},
                                /*op_end=*/true);
        !s.ok()) [[unlikely]] {
        return s;
    }
    return pool_.pager().SetNumKeys(pool_.pager().num_keys() - 1);
}
```

- [ ] **Step 6: Fix `SplitLeaf`'s sibling latch (thread-safety bug)**

In `SplitLeaf` (line ~705), the old-next sibling is mutated through an **unlatched** `FetchPage`. Change it to `FetchPageWrite` so the sibling's `prev` pointer is written under its exclusive latch:

```cpp
    // Update the old next's prev pointer (guard budget peaks at 3 here).
    // FetchPageWrite: the sibling is mutated, so it must be exclusively latched
    // (rightward-only sibling latching keeps crabbing order acyclic).
    if (old_next != kInvalidPageId) {
        auto old_next_guard = pool_.FetchPageWrite(old_next);
        if (!old_next_guard) [[unlikely]] {
            return std::unexpected(old_next_guard.error());
        }
        NodeView old_next_node(old_next_guard->page());
        old_next_node.SetPrevLeaf(new_page_id);
        if (Status s = LogPageImage(*old_next_guard); !s.ok()) [[unlikely]] {
            return std::unexpected(s);
        }
    }
```

- [ ] **Step 7: Rewrite `Put` as an exclusive-crab descent with held-stack split propagation**

Replace `BPlusTree::Put` (lines 231–480) with:

```cpp
Status BPlusTree::Put(std::string_view key, std::string_view value) {
    // Writers hold root_latch EXCLUSIVE up front: this write may split the root
    // and rewrite root_page_id, and root_latch is the outermost lock (acquired
    // before any page latch) so the "read root_id + latch root" window and any
    // SetRootPageId stay atomic against concurrent root changes. Released the
    // moment the root is proven not to split (pessimistic-first; a shared-latch
    // fast path for non-splitting writers is a documented future refinement).
    std::unique_lock<std::shared_mutex> root_lk(pool_.root_latch());

    // Case 1: empty tree - create the first leaf as root (a root change).
    if (IsEmpty()) [[unlikely]] {
        auto guard = pool_.NewPage();
        if (!guard) return guard.error();
        PageId root_id = guard->page_id();
        guard->page().Init(root_id, storage::PageType::kLeaf);
        NodeView node(guard->page());
        node.InitLeaf();
        if (!node.InsertLeaf(key, value)) [[unlikely]] {
            return Status::Internal("Failed to insert into empty leaf");
        }
        if (Status s = LogPageEvent(wal::LogRecordType::kAllocPage, root_id);
            !s.ok()) [[unlikely]] return s;
        if (Status s = LogPageImage(*guard); !s.ok()) [[unlikely]] return s;
        if (Status s = LogPageEvent(wal::LogRecordType::kRootChange, root_id,
                                    /*op_end=*/true); !s.ok()) [[unlikely]] return s;
        if (Status s = pool_.pager().SetRootPageId(root_id); !s.ok()) return s;
        return pool_.pager().SetNumKeys(pool_.pager().num_keys() + 1);
    }

    // Descend from the root taking EXCLUSIVE latches, keeping the whole path on
    // a guard stack. Variable-length keys make "this internal node won't split"
    // uncheckable cheaply, so we hold the path and release it wholesale once the
    // leaf is known safe (the common, no-split case).
    std::vector<storage::PageGuard> stack;
    {
        PageId current = pool_.pager().root_page_id();
        while (true) {
            auto g = pool_.FetchPageWrite(current);
            if (!g) [[unlikely]] return g.error();
            bool is_leaf = NodeView(g->page()).IsLeaf();
            PageId child = is_leaf ? kInvalidPageId : NodeView(g->page()).FindChild(key);
            stack.push_back(std::move(*g));
            if (is_leaf) break;
            if (child == kInvalidPageId) [[unlikely]] {
                return Status::Internal("Invalid child pointer");
            }
            current = child;
        }
    }

    storage::PageGuard& leaf_guard = stack.back();
    NodeView leaf_node(leaf_guard.page());
    bool found;
    uint16_t found_index = leaf_node.FindKeyIndex(key, found);
    bool replacing_grown_value = false;

    // Release every ancestor latch (keep only the leaf) and drop root_latch -
    // used on every path that does NOT split the leaf.
    auto release_ancestors_and_root = [&]() {
        for (size_t i = 0; i + 1 < stack.size(); ++i) stack[i].Drop();
        if (root_lk.owns_lock()) root_lk.unlock();
    };

    if (found) [[unlikely]] {
        if (leaf_node.UpdateLeaf(key, value)) [[likely]] {
            release_ancestors_and_root();
            return LogKeyRecord(wal::LogRecordType::kPut, leaf_guard, key, value,
                                /*op_end=*/true);
        }
        // New value no longer fits even after rebuild: remove old, fall through.
        if (!leaf_node.DeleteAt(found_index)) [[unlikely]] {
            return Status::Internal("Failed to remove record for oversized update");
        }
        leaf_guard.MarkDirty();
        replacing_grown_value = true;
    }

    if (leaf_node.CanFit(static_cast<uint16_t>(key.size()),
                         static_cast<uint16_t>(value.size()))) [[likely]] {
        // No split: the leaf is safe. Release ancestors + root_latch, then insert.
        release_ancestors_and_root();
        if (!leaf_node.InsertLeaf(key, value)) [[unlikely]] {
            return Status::Internal("CanFit disagreed with InsertLeaf");
        }
        if (Status s = LogKeyRecord(wal::LogRecordType::kPut, leaf_guard, key, value,
                                    /*op_end=*/true); !s.ok()) [[unlikely]] return s;
        if (!replacing_grown_value) {
            return pool_.pager().SetNumKeys(pool_.pager().num_keys() + 1);
        }
        return Status::Ok();
    }

    // Leaf splits - keep the ancestor stack and root_latch (the split may reach
    // the root). SplitLeaf briefly latches the right sibling (rightward order).
    PageId leaf_id = leaf_guard.page_id();
    auto split_result = SplitLeaf(leaf_guard);
    if (!split_result) [[unlikely]] return split_result.error();
    auto&& [new_leaf_id, split_key] = *split_result;

    if (key < split_key) {
        NodeView original_node(leaf_guard.page());
        if (!original_node.InsertLeaf(key, value)) [[unlikely]] {
            return Status::Internal("Failed to insert after split (left)");
        }
        if (Status s = LogKeyRecord(wal::LogRecordType::kPut, leaf_guard, key, value,
                                    /*op_end=*/false); !s.ok()) [[unlikely]] return s;
    } else {
        auto new_guard = pool_.FetchPageWrite(new_leaf_id);
        if (!new_guard) [[unlikely]] return new_guard.error();
        NodeView new_node(new_guard->page());
        if (!new_node.InsertLeaf(key, value)) [[unlikely]] {
            return Status::Internal("Failed to insert after split (right)");
        }
        if (Status s = LogKeyRecord(wal::LogRecordType::kPut, *new_guard, key, value,
                                    /*op_end=*/false); !s.ok()) [[unlikely]] return s;
    }

    if (!replacing_grown_value) {
        if (Status s = pool_.pager().SetNumKeys(pool_.pager().num_keys() + 1);
            !s.ok()) return s;
    }

    // Propagate the split up through the HELD ancestor guards (no re-fetch: we
    // still hold their exclusive latches, so the structure is stable). stack is
    // [root, ..., parent-of-leaf, leaf]; walk parents from nearest up to root.
    PageId left_child = leaf_id;
    PageId right_child = new_leaf_id;
    std::string key_to_insert = std::move(split_key);

    for (size_t depth = stack.size() - 1; depth-- > 0;) {
        storage::PageGuard& parent_guard = stack[depth];
        NodeView parent_node(parent_guard.page());
        if (parent_node.InsertInternalAfterSplit(key_to_insert, left_child,
                                                 right_child)) [[likely]] {
            // Parent absorbed the separator - no split above here; root is safe.
            return LogPageImage(parent_guard, /*op_end=*/true);
        }
        auto internal_split = SplitInternal(parent_guard);
        if (!internal_split) [[unlikely]] return internal_split.error();
        auto&& [new_internal_id, new_split_key] = *internal_split;
        PageId parent_id = parent_guard.page_id();
        if (key_to_insert < new_split_key) {
            NodeView orig_internal(parent_guard.page());
            if (!orig_internal.InsertInternalAfterSplit(key_to_insert, left_child,
                                                        right_child)) [[unlikely]] {
                return Status::Internal("insert into split internal (left)");
            }
            if (Status s = LogPageImage(parent_guard); !s.ok()) [[unlikely]] return s;
        } else {
            auto new_internal_guard = pool_.FetchPageWrite(new_internal_id);
            if (!new_internal_guard) [[unlikely]] return new_internal_guard.error();
            NodeView new_internal(new_internal_guard->page());
            if (!new_internal.InsertInternalAfterSplit(key_to_insert, left_child,
                                                       right_child)) [[unlikely]] {
                return Status::Internal("insert into split internal (right)");
            }
            if (Status s = LogPageImage(*new_internal_guard); !s.ok()) [[unlikely]] return s;
        }
        left_child = parent_id;
        right_child = new_internal_id;
        key_to_insert = std::move(new_split_key);
    }

    // Reached the root and it split - create a new root (still under root_latch).
    return CreateNewRoot(key_to_insert, left_child, right_child);
}
```

- [ ] **Step 8: Rewrite `Seek`, `SeekToFirst`, `FindLeaf` to read-crab; remove `FindLeafPath`**

Replace `FindLeaf` (lines 548–576) with:

```cpp
Result<PageId> BPlusTree::FindLeaf(std::string_view key) {
    // Read-crab down to the leaf, returning its id (latch dropped on return -
    // callers that re-fetch accept read-committed-per-leaf semantics).
    std::shared_lock<std::shared_mutex> root_lk(pool_.root_latch());
    if (IsEmpty()) [[unlikely]] return std::unexpected(Status::NotFound("Tree is empty"));
    PageId current = pool_.pager().root_page_id();
    auto g = pool_.FetchPageRead(current);
    if (!g) [[unlikely]] return std::unexpected(g.error());
    root_lk.unlock();
    storage::PageGuard guard = std::move(*g);
    while (true) {
        NodeView node(guard.page());
        if (node.IsLeaf()) [[likely]] return guard.page_id();
        PageId child = node.FindChild(key);
        if (child == kInvalidPageId) [[unlikely]] {
            return std::unexpected(Status::Internal("Invalid child pointer in internal node"));
        }
        auto cg = pool_.FetchPageRead(child);
        if (!cg) [[unlikely]] return std::unexpected(cg.error());
        guard = std::move(*cg);
    }
}
```

In `Seek` (line ~502) change the lower-bound fetch to a read latch:

```cpp
        auto guard = pool_.FetchPageRead(*leaf_result);
```

Replace `SeekToFirst` (lines 514–542) with a read-crab descent:

```cpp
Cursor BPlusTree::SeekToFirst() {
    Cursor cursor(pool_);
    std::shared_lock<std::shared_mutex> root_lk(pool_.root_latch());
    if (IsEmpty()) [[unlikely]] return cursor;
    PageId current = pool_.pager().root_page_id();
    auto g = pool_.FetchPageRead(current);
    if (!g) [[unlikely]] { cursor.status_ = g.error(); return cursor; }
    root_lk.unlock();
    storage::PageGuard guard = std::move(*g);
    while (true) {
        NodeView node(guard.page());
        if (node.IsLeaf()) break;
        PageId next = node.NumKeys() > 0 ? node.GetChild(0) : node.GetRightmostChild();
        if (next == kInvalidPageId) [[unlikely]] {
            cursor.status_ = Status::Internal("Invalid child pointer");
            return cursor;
        }
        auto cg = pool_.FetchPageRead(next);
        if (!cg) [[unlikely]] { cursor.status_ = cg.error(); return cursor; }
        guard = std::move(*cg);
    }
    PageId leaf_id = guard.page_id();
    guard.Drop();
    cursor.MoveTo(leaf_id, 0);
    return cursor;
}
```

Delete the now-dead `FindLeafPath` definition (lines 578–619) from `src/btree/btree.cc`, and remove its declaration plus the `PathEntry` struct (`include/kvstore/btree/btree.h` lines 223–231):

```cpp
    // (removed) struct PathEntry / FindLeafPath - Put now crabs with a held
    // guard stack instead of a re-fetched path.
```

- [ ] **Step 9: Change the cursor to read-latch its pages**

In `src/btree/cursor.cc`, change both `pool_->FetchPage(...)` calls (lines 21 and 45) to `pool_->FetchPageRead(...)`. The page is copied out and the guard dropped immediately, so the cursor still holds no latch between `Next()` calls (read-committed-per-leaf).

- [ ] **Step 10: Verify & checkpoint (NO commit)**

Run the non-concurrent suite under asan (nothing regressed):
`cmake --preset asan && cmake --build build/asan -j && ctest --preset asan`
Expected: PASS (all existing tests: `btree_test`, `cursor_test`, `db_test`, `recovery_test`, `crash_test`, etc.).

Run the concurrency suite under tsan:
`cmake --preset tsan && cmake --build build/tsan -j && ctest --preset tsan -R "btree_concurrent_test|buffer_pool_concurrent_test"`
Expected: PASS, tsan-clean (no data-race reports).

Do **not** commit.

---

## Task 2: WAL record format — txn markers + before-images (5.3a/5.4a)

Extend the on-disk WAL record with the three transaction markers and a before-image field, keeping the parser uniform. On-disk data-file format v3 is untouched.

**Files:**
- Modify: `include/kvstore/wal/log_record.h` (enum, `before` field, `kHeaderSize`, `SerializedSize`)
- Modify: `src/wal/log_record.cc` (`AppendTo`, `Parse`)
- Modify: `tests/wal_test.cc` (round-trip cases)

**Interfaces:**
- Consumes: `storage::Crc32`, `PutLE`/`GetLE` helpers (in `log_record.cc`).
- Produces:
  - `LogRecordType::{kBegin=8, kCommit=9, kAbort=10}`.
  - `std::optional<std::string> LogRecord::before` — prior value for `kPut`/`kDelete`; `std::nullopt` ⇒ key was absent (insert). On disk: `before_len:2` after `val_len`; `0xFFFF` sentinel ⇒ absent, else the byte length (0 allowed).
  - `LogRecord::kHeaderSize == 27` (was 25).

- [ ] **Step 1: Write the failing round-trip test**

Append to `tests/wal_test.cc` (inside the existing anonymous namespace / test file):

```cpp
TEST(LogRecord, BeforeImageRoundTrip) {
    using kvstore::wal::LogRecord;
    using kvstore::wal::LogRecordType;

    // kPut carrying a prior value (an update's before-image).
    LogRecord put;
    put.lsn = 100;
    put.txid = 7;
    put.type = LogRecordType::kPut;
    put.page_id = 3;
    put.op_end = true;
    put.key = "k";
    put.value = "new";
    put.before = std::string("old");

    std::string buf;
    put.AppendTo(buf);
    auto parsed = LogRecord::Parse(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(buf.data()), buf.size()));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->first.txid, 7u);
    EXPECT_EQ(parsed->first.type, LogRecordType::kPut);
    EXPECT_TRUE(parsed->first.op_end);
    EXPECT_EQ(parsed->first.key, "k");
    EXPECT_EQ(parsed->first.value, "new");
    ASSERT_TRUE(parsed->first.before.has_value());
    EXPECT_EQ(*parsed->first.before, "old");
    EXPECT_EQ(parsed->second, buf.size());
}

TEST(LogRecord, AbsentBeforeImageAndTxnMarkers) {
    using kvstore::wal::LogRecord;
    using kvstore::wal::LogRecordType;

    // Insert: before is nullopt (key was absent) -> 0xFFFF sentinel on disk.
    LogRecord ins;
    ins.type = LogRecordType::kPut;
    ins.key = "x";
    ins.value = "";      // empty value, still present
    ins.before = std::nullopt;
    std::string b1;
    ins.AppendTo(b1);
    auto p1 = LogRecord::Parse(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(b1.data()), b1.size()));
    ASSERT_TRUE(p1.has_value());
    EXPECT_FALSE(p1->first.before.has_value());
    EXPECT_EQ(p1->first.value, "");

    // Commit marker parses back with its type.
    LogRecord commit;
    commit.txid = 42;
    commit.type = LogRecordType::kCommit;
    commit.op_end = true;
    std::string b2;
    commit.AppendTo(b2);
    auto p2 = LogRecord::Parse(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(b2.data()), b2.size()));
    ASSERT_TRUE(p2.has_value());
    EXPECT_EQ(p2->first.type, LogRecordType::kCommit);
    EXPECT_EQ(p2->first.txid, 42u);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --preset asan && cmake --build build/asan -j 2>&1 | head -40`
Expected: FAIL to compile — `LogRecord` has no `before` member; `LogRecordType::kCommit` undeclared.

- [ ] **Step 3: Extend the record type enum and struct**

In `include/kvstore/wal/log_record.h`, add to the enum:

```cpp
enum class LogRecordType : uint8_t {
    kPut = 1,
    kDelete = 2,
    kAllocPage = 3,
    kFreePage = 4,
    kRootChange = 5,
    kCheckpoint = 6,
    kPageImage = 7,
    kBegin = 8,    // Phase 5: transaction start (lazy, first write)
    kCommit = 9,   // Phase 5: transaction commit (op_end)
    kAbort = 10,   // Phase 5: transaction abort (op_end, after logged undo)
};
```

Update the struct: grow `kHeaderSize`, add the `before` field and `<optional>` include (already included), and update `SerializedSize`:

```cpp
    // Fixed prefix: lsn + txid + type + page_id + key_len + val_len + before_len.
    static constexpr size_t kHeaderSize = 8 + 8 + 1 + 4 + 2 + 2 + 2;  // 27
    static constexpr size_t kCrcSize = 4;

    // On-disk before_len sentinel: the key had no prior value (an insert).
    static constexpr uint16_t kBeforeAbsent = 0xFFFF;
```

Add the field after `value`:

```cpp
    std::string key;
    std::string value;
    // Phase 5.4: prior value for kPut/kDelete undo. nullopt => key was absent
    // (insert). Values never reach 0xFFFF bytes (page-bounded), so the sentinel
    // is unambiguous. Empty/unused for all other record types.
    std::optional<std::string> before;
```

Update `SerializedSize`:

```cpp
    [[nodiscard("SerializedSize sizes the append")]]
    size_t SerializedSize() const noexcept {
        const size_t before_bytes = before ? before->size() : 0;
        return kHeaderSize + key.size() + value.size() + before_bytes + kCrcSize;
    }
```

- [ ] **Step 4: Update `AppendTo` and `Parse`**

In `src/wal/log_record.cc`, replace the body of `AppendTo` (after the `start` line) with:

```cpp
    const size_t start = out.size();

    PutLE(out, lsn);
    PutLE(out, txid);
    out.push_back(static_cast<char>(
        static_cast<uint8_t>(type) | (op_end ? kOpEndBit : 0)));
    PutLE(out, page_id);
    PutLE(out, static_cast<uint16_t>(key.size()));
    PutLE(out, static_cast<uint16_t>(value.size()));
    PutLE(out, before ? static_cast<uint16_t>(before->size()) : kBeforeAbsent);
    out.append(key);
    out.append(value);
    if (before) out.append(*before);

    const uint32_t crc =
        storage::Crc32(out.data() + start, out.size() - start);
    PutLE(out, crc);
```

Replace `Parse` with:

```cpp
std::optional<std::pair<LogRecord, size_t>> LogRecord::Parse(
    std::span<const uint8_t> buffer) {
    if (buffer.size() < kHeaderSize + kCrcSize) {
        return std::nullopt;  // Not even a full header: torn tail
    }

    LogRecord record;
    const uint8_t* p = buffer.data();
    record.lsn = GetLE<Lsn>(p);
    record.txid = GetLE<TxId>(p + 8);
    const uint8_t raw_type_byte = p[16];
    const uint8_t raw_type = raw_type_byte & static_cast<uint8_t>(~kOpEndBit);
    record.op_end = (raw_type_byte & kOpEndBit) != 0;
    record.page_id = GetLE<PageId>(p + 17);
    const uint16_t key_len = GetLE<uint16_t>(p + 21);
    const uint16_t val_len = GetLE<uint16_t>(p + 23);
    const uint16_t before_len = GetLE<uint16_t>(p + 25);

    if (raw_type < static_cast<uint8_t>(LogRecordType::kPut) ||
        raw_type > static_cast<uint8_t>(LogRecordType::kAbort)) {
        return std::nullopt;  // Garbage type byte
    }
    record.type = static_cast<LogRecordType>(raw_type);

    const size_t before_bytes = (before_len == kBeforeAbsent) ? 0 : before_len;
    const size_t total =
        kHeaderSize + key_len + val_len + before_bytes + kCrcSize;
    if (buffer.size() < total) {
        return std::nullopt;  // Payload cut short: torn tail
    }

    const uint32_t stored_crc = GetLE<uint32_t>(p + total - kCrcSize);
    if (storage::Crc32(p, total - kCrcSize) != stored_crc) {
        return std::nullopt;  // Bit rot or a torn write inside the record
    }

    const uint8_t* cur = p + kHeaderSize;
    record.key.assign(reinterpret_cast<const char*>(cur), key_len);
    cur += key_len;
    record.value.assign(reinterpret_cast<const char*>(cur), val_len);
    cur += val_len;
    if (before_len != kBeforeAbsent) {
        record.before = std::string(reinterpret_cast<const char*>(cur), before_len);
    }
    return std::pair{std::move(record), total};
}
```

- [ ] **Step 5: Verify & checkpoint (NO commit)**

Run: `cmake --build build/asan -j && ctest --preset asan -R wal_test`
Expected: PASS (new round-trip cases + existing WAL tests).

Then the full asan suite (recovery/crash tests still parse the WAL): `ctest --preset asan`
Expected: PASS. Do **not** commit.

---

## Task 3: `LockManager` — per-key S/X locks with wait-die (5.5 mechanism)

Build the lock manager first so transactions are lock-correct from birth. No dependencies on the txn layer.

**Files:**
- Create: `include/kvstore/txn/lock_manager.h`, `src/txn/lock_manager.cc`
- Create: `tests/lock_manager_test.cc`
- Modify: `CMakeLists.txt` (add `src/txn/lock_manager.cc` to `KVSTORE_SOURCES`; add `lock_manager_test` to `KVSTORE_TESTS`)

**Interfaces:**
- Produces:
  - `enum class LockMode : uint8_t { kShared, kExclusive };`
  - `Status LockManager::Acquire(TxId txid, std::string_view key, LockMode mode);` — grants, blocks (older txn waits), or returns `Status::Busy(...)` (younger txn dies). Re-acquiring an already-held ≥ mode is a no-op; `kShared`→`kExclusive` upgrade allowed when sole holder.
  - `void LockManager::ReleaseAll(TxId txid);` — drops every lock held by `txid` and wakes waiters.

- [ ] **Step 1: Write the failing unit test**

Create `tests/lock_manager_test.cc`:

```cpp
// ==============================================================================
// tests/lock_manager_test.cc - Strict 2PL lock manager (Phase 5.5)
// ==============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "kvstore/txn/lock_manager.h"

namespace {

using namespace kvstore;
using kvstore::txn::LockManager;
using kvstore::txn::LockMode;

TEST(LockManager, SharedSharedCompatible) {
    LockManager lm;
    EXPECT_TRUE(lm.Acquire(1, "k", LockMode::kShared).ok());
    EXPECT_TRUE(lm.Acquire(2, "k", LockMode::kShared).ok());  // shared coexists
    lm.ReleaseAll(1);
    lm.ReleaseAll(2);
}

TEST(LockManager, ExclusiveExcludesAndWaitDieDies) {
    LockManager lm;
    // Older txn (1) holds X; younger txn (2) requesting X must DIE (kBusy).
    EXPECT_TRUE(lm.Acquire(1, "k", LockMode::kExclusive).ok());
    Status s = lm.Acquire(2, "k", LockMode::kExclusive);
    EXPECT_EQ(s.code(), StatusCode::kBusy);
    lm.ReleaseAll(1);
}

TEST(LockManager, WaitDieOlderWaitsThenGrants) {
    LockManager lm;
    // Younger txn (5) holds X; older txn (2) requesting X must WAIT, then get it
    // once 5 releases (older never dies -> starvation-free).
    ASSERT_TRUE(lm.Acquire(5, "k", LockMode::kExclusive).ok());

    std::atomic<bool> granted{false};
    std::thread older([&] {
        EXPECT_TRUE(lm.Acquire(2, "k", LockMode::kExclusive).ok());
        granted.store(true);
        lm.ReleaseAll(2);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(granted.load());  // still waiting on holder 5
    lm.ReleaseAll(5);
    older.join();
    EXPECT_TRUE(granted.load());
}

TEST(LockManager, UpgradeSharedToExclusiveWhenSoleHolder) {
    LockManager lm;
    ASSERT_TRUE(lm.Acquire(1, "k", LockMode::kShared).ok());
    EXPECT_TRUE(lm.Acquire(1, "k", LockMode::kExclusive).ok());  // upgrade
    lm.ReleaseAll(1);
}

TEST(LockManager, ReAcquireIsNoOp) {
    LockManager lm;
    ASSERT_TRUE(lm.Acquire(1, "k", LockMode::kExclusive).ok());
    EXPECT_TRUE(lm.Acquire(1, "k", LockMode::kExclusive).ok());  // idempotent
    EXPECT_TRUE(lm.Acquire(1, "k", LockMode::kShared).ok());     // covered by X
    lm.ReleaseAll(1);
}

}  // namespace
```

- [ ] **Step 2: Register sources and run to verify it fails**

In `CMakeLists.txt`, add to `KVSTORE_SOURCES` (after `src/db.cc`):

```cmake
    src/txn/lock_manager.cc
```

Add to `KVSTORE_TESTS` (after `db_test`):

```cmake
        lock_manager_test
```

Run: `cmake --preset asan && cmake --build build/asan -j 2>&1 | head -20`
Expected: FAIL — `kvstore/txn/lock_manager.h` not found.

- [ ] **Step 3: Write the header**

Create `include/kvstore/txn/lock_manager.h`:

```cpp
// ==============================================================================
// kvstore/txn/lock_manager.h - Per-Key S/X Lock Manager (Phase 5.5)
// ==============================================================================
//
// Rigorous (strict) two-phase locking for point operations: transactions take
// shared (read) or exclusive (write) locks on individual keys and hold them all
// until commit/abort, yielding serializable schedules for point conflicts.
//
// Deadlock avoidance: WAIT-DIE on the transaction id (smaller id == older).
// On a conflicting request by T against holder H:
//   - T older than every conflicting holder  -> T WAITS (blocks).
//   - T younger than any conflicting holder   -> T DIES (returns kBusy; the
//     caller aborts and, for autocommit, retries reusing the SAME txid so it
//     ages into being oldest and is then guaranteed to win).
// Wait-die is provably deadlock-free for any id assignment, so it is safe to
// block in Acquire even while the caller holds other logical locks. Callers
// must hold NO page latch while calling Acquire (deadlock-freedom invariant).
//
// One mutex guards the whole table (correctness first; sharding is a perf
// follow-up, not required for C5).
//
// ==============================================================================

#pragma once

#include <condition_variable>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kvstore/status.h"
#include "kvstore/types.h"

namespace kvstore::txn {

enum class LockMode : uint8_t { kShared, kExclusive };

class LockManager {
public:
    LockManager() = default;
    LockManager(const LockManager&) = delete;
    LockManager& operator=(const LockManager&) = delete;

    // Acquire `mode` on `key` for `txid`. Grants (Ok), blocks then grants (Ok),
    // or returns Status::Busy when wait-die makes this (younger) txn the victim.
    [[nodiscard("Acquire returns Ok/Busy - a Busy caller must abort")]]
    Status Acquire(TxId txid, std::string_view key, LockMode mode);

    // Release every lock held by `txid` and wake blocked waiters. Idempotent.
    void ReleaseAll(TxId txid);

private:
    struct Request {
        TxId txid;
        LockMode mode;
        bool granted;
    };
    struct LockEntry {
        std::list<Request> queue;      // granted requests + FIFO waiters
        std::condition_variable cv;
    };

    // Compatibility + wait-die evaluation for a request already appended to the
    // entry's queue. Returns Ok (granted, request.granted set true), Busy (this
    // txn must die), or a "keep waiting" signal via `must_wait`.
    // Caller holds mu_.
    [[nodiscard]]
    Status TryGrantLocked(LockEntry& entry, std::list<Request>::iterator me,
                          bool& must_wait);

    std::mutex mu_;
    std::unordered_map<std::string, LockEntry> table_;
    std::unordered_map<TxId, std::vector<std::string>> held_;  // txid -> keys
};

}  // namespace kvstore::txn
```

- [ ] **Step 4: Write the implementation**

Create `src/txn/lock_manager.cc`:

```cpp
// ==============================================================================
// kvstore/txn/lock_manager.cc - Per-Key S/X Lock Manager (Phase 5.5)
// ==============================================================================

#include "kvstore/txn/lock_manager.h"

#include <algorithm>

namespace kvstore::txn {

namespace {
bool Conflicts(LockMode a, LockMode b) {
    // Only shared/shared is compatible.
    return a == LockMode::kExclusive || b == LockMode::kExclusive;
}
}  // namespace

Status LockManager::TryGrantLocked(LockEntry& entry,
                                   std::list<Request>::iterator me,
                                   bool& must_wait) {
    must_wait = false;
    // Scan other requests. Grant iff no OTHER GRANTED request conflicts.
    // Apply wait-die against conflicting granted holders.
    for (auto it = entry.queue.begin(); it != entry.queue.end(); ++it) {
        if (it == me) continue;
        if (!it->granted) continue;                 // waiters don't block us here
        if (it->txid == me->txid) continue;         // our own other grant
        if (!Conflicts(it->mode, me->mode)) continue;
        // Conflicting granted holder `it`.
        if (it->txid < me->txid) {
            // An OLDER txn holds a conflicting lock -> we (younger) DIE.
            return Status::Busy("wait-die: younger transaction aborts");
        }
        // We are older than this holder -> we must WAIT for it.
        must_wait = true;
    }
    if (must_wait) return Status::Ok();  // caller blocks and retries
    me->granted = true;
    return Status::Ok();
}

Status LockManager::Acquire(TxId txid, std::string_view key, LockMode mode) {
    std::unique_lock<std::mutex> lk(mu_);
    const std::string k(key);
    LockEntry& entry = table_[k];

    // Already hold something on this key? Re-acquire / upgrade handling.
    for (auto& req : entry.queue) {
        if (req.txid == txid && req.granted) {
            if (req.mode == LockMode::kExclusive || mode == LockMode::kShared) {
                return Status::Ok();  // already covered (X covers S; same mode)
            }
            // Upgrade S -> X: allowed iff no other granted holder exists.
            for (auto& other : entry.queue) {
                if (&other == &req) continue;
                if (other.granted) {
                    if (other.txid < txid) {
                        return Status::Busy("wait-die: younger upgrade aborts");
                    }
                    // Older upgrader vs younger holder: wait for it to drain.
                    // Fall through to the general waiting path by upgrading the
                    // request mode and blocking below.
                }
            }
            req.mode = LockMode::kExclusive;
            // Re-evaluate: if others still hold, block until they release.
            bool must_wait = false;
            auto me = entry.queue.begin();
            while (me != entry.queue.end() && &*me != &req) ++me;
            for (;;) {
                Status s = TryGrantLocked(entry, me, must_wait);
                if (!s.ok()) return s;
                if (!must_wait) return Status::Ok();
                entry.cv.wait(lk);
            }
        }
    }

    // Fresh request: append and evaluate under wait-die.
    entry.queue.push_back(Request{txid, mode, /*granted=*/false});
    auto me = std::prev(entry.queue.end());
    for (;;) {
        bool must_wait = false;
        Status s = TryGrantLocked(entry, me, must_wait);
        if (!s.ok()) {
            entry.queue.erase(me);      // died: withdraw the request
            entry.cv.notify_all();
            return s;
        }
        if (!must_wait) {
            held_[txid].push_back(k);   // record for ReleaseAll
            return Status::Ok();
        }
        entry.cv.wait(lk);              // older txn waits; re-check on wake
    }
}

void LockManager::ReleaseAll(TxId txid) {
    std::unique_lock<std::mutex> lk(mu_);
    auto held_it = held_.find(txid);
    if (held_it == held_.end()) return;
    for (const std::string& k : held_it->second) {
        auto entry_it = table_.find(k);
        if (entry_it == table_.end()) continue;
        LockEntry& entry = entry_it->second;
        for (auto it = entry.queue.begin(); it != entry.queue.end();) {
            if (it->txid == txid) it = entry.queue.erase(it);
            else ++it;
        }
        entry.cv.notify_all();  // let waiters re-evaluate
    }
    held_.erase(held_it);
}

}  // namespace kvstore::txn
```

- [ ] **Step 5: Run the unit tests**

Run: `cmake --build build/asan -j && ctest --preset asan -R lock_manager_test --output-on-failure`
Expected: PASS (all 5 cases, including the threaded wait-die-waits case).

- [ ] **Step 6: Verify under tsan (no data races in the CV protocol)**

Run: `cmake --preset tsan && cmake --build build/tsan -j && ctest --preset tsan -R lock_manager_test`
Expected: PASS, tsan-clean. Do **not** commit.

---

## Task 4: `Txn` / `TxnManager` integration + before-image plumbing (5.3b/5.4a)

Add the transaction layer and wire it into `DbImpl`: real txids, lazy `kBegin`, before-image capture, in-session `Abort`, autocommit routing, and the public `DB::Begin()`.

**Files:**
- Create: `include/kvstore/txn/txn.h`, `src/txn/txn.cc`
- Modify: `include/kvstore/btree/btree.h`, `src/btree/btree.cc` (thread `TxId txid` + `const std::optional<std::string>& before` through `Put`/`Delete` and the WAL helpers)
- Modify: `include/kvstore/db.h` (include `txn.h`; add `Begin()`)
- Modify: `src/db.cc` (construct `LockManager`+`TxnManager`; route autocommit; implement `Begin()`; drop `needs_reopen_`)
- Modify: `CMakeLists.txt` (add `src/txn/txn.cc`; add `txn_test`)
- Create: `tests/txn_test.cc`

**Interfaces:**
- Consumes: `LockManager` (Task 3); `BPlusTree::{Get,Put,Delete}`; `wal::LogWriter::{Append,Flush}`; `wal::LogRecordType::{kBegin,kCommit,kAbort}` + `LogRecord::before` (Task 2).
- Produces:
  - Tree signatures: `Status BPlusTree::Put(std::string_view key, std::string_view value, TxId txid = kInvalidTxId, const std::optional<std::string>& before = std::nullopt);` and `Status BPlusTree::Delete(std::string_view key, TxId txid = kInvalidTxId, const std::optional<std::string>& before = std::nullopt);`
  - WAL helper: `Status BPlusTree::LogKeyRecord(wal::LogRecordType type, storage::PageGuard& guard, std::string_view key, std::string_view value, TxId txid, const std::optional<std::string>& before, bool op_end);`
  - `class TxnManager` with `std::unique_ptr<Txn> Begin();` `Status AutocommitPut(key,value);` `Status AutocommitDelete(key);` `Result<std::string> AutocommitGet(key);` and `Status LogTxnMarker(wal::LogRecordType, TxId);`
  - `class Txn` with `Result<std::string> Get(key); Status Put(key,value); Status Delete(key); Status Commit(); Status Abort();`

- [ ] **Step 1: Write the failing transaction test**

Create `tests/txn_test.cc`:

```cpp
// ==============================================================================
// tests/txn_test.cc - Explicit transactions: commit / abort / autocommit
// ==============================================================================

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

#include "kvstore/db.h"

namespace {

using namespace kvstore;

std::unique_ptr<DB> OpenFresh(const std::string& dir) {
    std::filesystem::remove_all(dir);
    Options opts;
    opts.create_if_missing = true;
    return DB::Open(dir, opts).value();
}

TEST(Txn, CommitPersistsAllWrites) {
    auto db = OpenFresh("/tmp/kvstore_txn_commit");
    auto t = db->Begin();
    ASSERT_TRUE(t->Put("a", "1").ok());
    ASSERT_TRUE(t->Put("b", "2").ok());
    ASSERT_TRUE(t->Commit().ok());

    EXPECT_EQ(db->Get("a").value(), "1");
    EXPECT_EQ(db->Get("b").value(), "2");
}

TEST(Txn, AbortRollsBackToBeforeImages) {
    auto db = OpenFresh("/tmp/kvstore_txn_abort");
    ASSERT_TRUE(db->Put("a", "orig").ok());   // committed baseline

    auto t = db->Begin();
    ASSERT_TRUE(t->Put("a", "changed").ok());  // update over existing
    ASSERT_TRUE(t->Put("c", "new").ok());      // fresh insert
    EXPECT_EQ(t->Get("a").value(), "changed");  // own writes visible in-txn
    ASSERT_TRUE(t->Abort().ok());

    EXPECT_EQ(db->Get("a").value(), "orig");    // update undone
    EXPECT_EQ(db->Get("c").error().code(), StatusCode::kNotFound);  // insert undone
}

TEST(Txn, AutocommitStillWorks) {
    auto db = OpenFresh("/tmp/kvstore_txn_autocommit");
    ASSERT_TRUE(db->Put("k", "v").ok());       // autocommit path
    EXPECT_EQ(db->Get("k").value(), "v");
    ASSERT_TRUE(db->Delete("k").ok());
    EXPECT_EQ(db->Get("k").error().code(), StatusCode::kNotFound);
}

}  // namespace
```

- [ ] **Step 2: Register sources/tests and run to verify it fails**

In `CMakeLists.txt` add to `KVSTORE_SOURCES`:

```cmake
    src/txn/txn.cc
```

Add to `KVSTORE_TESTS`:

```cmake
        txn_test
```

Run: `cmake --preset asan && cmake --build build/asan -j 2>&1 | head -20`
Expected: FAIL — `DB` has no `Begin()`; `txn.h`/`txn.cc` absent.

- [ ] **Step 3: Thread `txid` + before-image through the tree WAL helpers**

In `include/kvstore/btree/btree.h`, change `Put`/`Delete` and `LogKeyRecord` declarations:

```cpp
    [[nodiscard("Put returns status - check for errors")]]
    Status Put(std::string_view key, std::string_view value,
               TxId txid = kInvalidTxId,
               const std::optional<std::string>& before = std::nullopt);

    [[nodiscard("Delete returns status - check for errors")]]
    Status Delete(std::string_view key, TxId txid = kInvalidTxId,
                  const std::optional<std::string>& before = std::nullopt);
```

```cpp
    [[nodiscard]]
    Status LogKeyRecord(wal::LogRecordType type, storage::PageGuard& guard,
                        std::string_view key, std::string_view value,
                        TxId txid, const std::optional<std::string>& before,
                        bool op_end);
```

In `src/btree/btree.cc`, update `LogKeyRecord`'s definition to stamp both fields:

```cpp
Status BPlusTree::LogKeyRecord(wal::LogRecordType type,
                               storage::PageGuard& guard,
                               std::string_view key, std::string_view value,
                               TxId txid, const std::optional<std::string>& before,
                               bool op_end) {
    guard.MarkDirty();
    if (wal_ == nullptr) {
        return Status::Ok();
    }
    if (wal_->NeedsImage(guard.page_id())) {
        return LogPageImage(guard, op_end);
    }
    wal::LogRecord record;
    record.type = type;
    record.txid = txid;
    record.page_id = guard.page_id();
    record.op_end = op_end;
    record.key = key;
    record.value = value;
    record.before = before;
    auto lsn = wal_->Append(record);
    if (!lsn) [[unlikely]] {
        return lsn.error();
    }
    guard.page().header()->page_lsn = *lsn;
    return Status::Ok();
}
```

> Note: when `NeedsImage` forces a `kPageImage` (first post-checkpoint mutation of the page), the before-image is not attached to that image record. That is correct: the loser-undo pass (Task 5) re-derives the inverse from the `kPut`/`kDelete` records it *does* find, and a page that only ever received an image from this txn contributes no logical key record to undo. Structural redo via images is physical and idempotent.

Update `Put`'s signature line and its four `LogKeyRecord` call sites to pass `txid, before`:

```cpp
Status BPlusTree::Put(std::string_view key, std::string_view value, TxId txid,
                      const std::optional<std::string>& before) {
```

- The update-in-place call: `LogKeyRecord(wal::LogRecordType::kPut, leaf_guard, key, value, txid, before, /*op_end=*/true)`
- The no-split insert call: same, `op_end=true`.
- The two post-split insert calls (`leaf_guard` and `*new_guard`): pass `txid, before, /*op_end=*/false`.

Update `Delete`'s signature line and its `LogKeyRecord` call:

```cpp
Status BPlusTree::Delete(std::string_view key, TxId txid,
                         const std::optional<std::string>& before) {
```

- `LogKeyRecord(wal::LogRecordType::kDelete, leaf_guard, key, {}, txid, before, /*op_end=*/true)`

Add `#include <optional>` to `btree.h` if not already present (it is, via `<optional>` at line 48).

- [ ] **Step 4: Write the txn header**

Create `include/kvstore/txn/txn.h`:

```cpp
// ==============================================================================
// kvstore/txn/txn.h - Transactions: TxnManager + explicit Txn handle (Phase 5)
// ==============================================================================
//
// TxnManager hands out transaction ids and owns the LockManager; it is also the
// entry point autocommit (db->Get/Put/Delete) routes through. A Txn holds its
// key locks (rigorous 2PL) and an in-memory undo stack of before-images for
// rollback. Locks are always taken BEFORE the latched tree op, with no page
// latch held while waiting (deadlock-freedom invariant).
//
// kBegin is written lazily on the first WRITE of a transaction; read-only
// transactions log nothing and are invisible to recovery (nothing to redo/undo).
//
// ==============================================================================

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kvstore/status.h"
#include "kvstore/types.h"
#include "kvstore/txn/lock_manager.h"

namespace kvstore {
struct Options;
}
namespace kvstore::btree { class BPlusTree; }
namespace kvstore::wal { class LogWriter; enum class LogRecordType : uint8_t; }

namespace kvstore::txn {

class Txn;

class TxnManager {
public:
    // References must outlive the manager (all owned by DbImpl).
    TxnManager(btree::BPlusTree& tree, wal::LogWriter& wal, const Options& options)
        : tree_(tree), wal_(wal), options_(options) {}

    TxnManager(const TxnManager&) = delete;
    TxnManager& operator=(const TxnManager&) = delete;

    // Start an explicit transaction.
    [[nodiscard("Begin returns the transaction handle")]]
    std::unique_ptr<Txn> Begin();

    // Autocommit single-op paths (db->Put/Delete/Get). Each takes the key lock,
    // performs the op, logs a commit, flushes per Options, and releases locks.
    // On lock contention they retry internally reusing a stable txid (wait-die
    // starvation-freedom).
    [[nodiscard]] Status AutocommitPut(std::string_view key, std::string_view value);
    [[nodiscard]] Status AutocommitDelete(std::string_view key);
    [[nodiscard]] Result<std::string> AutocommitGet(std::string_view key);

    [[nodiscard]] LockManager& locks() noexcept { return locks_; }
    [[nodiscard]] btree::BPlusTree& tree() noexcept { return tree_; }
    [[nodiscard]] wal::LogWriter& wal() noexcept { return wal_; }
    [[nodiscard]] const Options& options() const noexcept { return options_; }

    // Allocate the next transaction id (monotone, starts at 1).
    [[nodiscard]] TxId NextTxId() noexcept;

    // Append a payload-free txn marker (kBegin/kCommit/kAbort) for `txid`.
    [[nodiscard]] Status LogTxnMarker(wal::LogRecordType type, TxId txid);

private:
    btree::BPlusTree& tree_;
    wal::LogWriter& wal_;
    const Options& options_;
    LockManager locks_;
    std::atomic<TxId> next_txid_{1};
};

class Txn {
public:
    Txn(TxnManager& mgr, TxId txid) : mgr_(mgr), txid_(txid) {}
    ~Txn();  // aborts if still open (RAII safety)

    Txn(const Txn&) = delete;
    Txn& operator=(const Txn&) = delete;

    [[nodiscard("Get returns the value")]]
    Result<std::string> Get(std::string_view key);
    [[nodiscard("Put returns status")]]
    Status Put(std::string_view key, std::string_view value);
    [[nodiscard("Delete returns status")]]
    Status Delete(std::string_view key);
    [[nodiscard("Commit returns status")]]
    Status Commit();
    [[nodiscard("Abort returns status")]]
    Status Abort();

    [[nodiscard]] TxId id() const noexcept { return txid_; }

private:
    // One recorded write, for rollback: `before` == nullopt means the key was
    // absent (inverse = delete); else the inverse is put(before).
    struct UndoEntry {
        std::string key;
        std::optional<std::string> before;
    };

    // Ensure kBegin has been logged (lazy: first write only).
    [[nodiscard]] Status EnsureBegun();

    TxnManager& mgr_;
    TxId txid_;
    std::vector<UndoEntry> undo_;
    bool began_logged_ = false;
    bool finished_ = false;  // Commit or Abort ran
};

}  // namespace kvstore::txn
```

Add `#include <atomic>` to the header's include list (used by `next_txid_`).

- [ ] **Step 5: Write the txn implementation**

Create `src/txn/txn.cc`:

```cpp
// ==============================================================================
// kvstore/txn/txn.cc - Transactions: TxnManager + Txn (Phase 5)
// ==============================================================================

#include "kvstore/txn/txn.h"

#include <thread>

#include "kvstore/db.h"                 // Options
#include "kvstore/btree/btree.h"
#include "kvstore/wal/log_record.h"
#include "kvstore/wal/log_writer.h"

namespace kvstore::txn {

TxId TxnManager::NextTxId() noexcept {
    return next_txid_.fetch_add(1, std::memory_order_relaxed);
}

Status TxnManager::LogTxnMarker(wal::LogRecordType type, TxId txid) {
    wal::LogRecord rec;
    rec.type = type;
    rec.txid = txid;
    rec.op_end = true;   // a marker is its own (empty) op group
    auto lsn = wal_.Append(rec);
    if (!lsn) [[unlikely]] return lsn.error();
    return Status::Ok();
}

std::unique_ptr<Txn> TxnManager::Begin() {
    return std::make_unique<Txn>(*this, NextTxId());
}

// ------------------------------------------------------------------ Autocommit

Status TxnManager::AutocommitPut(std::string_view key, std::string_view value) {
    const TxId txid = NextTxId();   // stable across retries -> ages to oldest
    for (;;) {
        Status s = locks_.Acquire(txid, key, LockMode::kExclusive);
        if (s.code() == StatusCode::kBusy) {
            // Died under wait-die. Reusing the SAME txid ages us toward oldest
            // (guaranteed to eventually WAIT rather than die); yield to avoid a
            // hot spin while an older holder finishes.
            locks_.ReleaseAll(txid);
            std::this_thread::yield();
            continue;
        }
        if (!s.ok()) { locks_.ReleaseAll(txid); return s; }

        // before-image under the held X-lock (value is stable).
        std::optional<std::string> before;
        if (auto got = tree_.Get(key); got) before = *got;

        if (Status w = LogTxnMarker(wal::LogRecordType::kBegin, txid); !w.ok()) {
            locks_.ReleaseAll(txid); return w;
        }
        if (Status w = tree_.Put(key, value, txid, before); !w.ok()) {
            locks_.ReleaseAll(txid); return w;
        }
        if (Status w = LogTxnMarker(wal::LogRecordType::kCommit, txid); !w.ok()) {
            locks_.ReleaseAll(txid); return w;
        }
        if (options_.sync_on_write) {
            if (Status w = wal_.Flush(); !w.ok()) { locks_.ReleaseAll(txid); return w; }
        }
        locks_.ReleaseAll(txid);
        return Status::Ok();
    }
}

Status TxnManager::AutocommitDelete(std::string_view key) {
    const TxId txid = NextTxId();
    for (;;) {
        Status s = locks_.Acquire(txid, key, LockMode::kExclusive);
        if (s.code() == StatusCode::kBusy) {
            // Died under wait-die. Reusing the SAME txid ages us toward oldest
            // (guaranteed to eventually WAIT rather than die); yield to avoid a
            // hot spin while an older holder finishes.
            locks_.ReleaseAll(txid);
            std::this_thread::yield();
            continue;
        }
        if (!s.ok()) { locks_.ReleaseAll(txid); return s; }

        std::optional<std::string> before;
        if (auto got = tree_.Get(key); got) before = *got;
        else { locks_.ReleaseAll(txid); return Status::NotFound("Key not found"); }

        if (Status w = LogTxnMarker(wal::LogRecordType::kBegin, txid); !w.ok()) {
            locks_.ReleaseAll(txid); return w;
        }
        if (Status w = tree_.Delete(key, txid, before); !w.ok()) {
            locks_.ReleaseAll(txid); return w;
        }
        if (Status w = LogTxnMarker(wal::LogRecordType::kCommit, txid); !w.ok()) {
            locks_.ReleaseAll(txid); return w;
        }
        if (options_.sync_on_write) {
            if (Status w = wal_.Flush(); !w.ok()) { locks_.ReleaseAll(txid); return w; }
        }
        locks_.ReleaseAll(txid);
        return Status::Ok();
    }
}

Result<std::string> TxnManager::AutocommitGet(std::string_view key) {
    const TxId txid = NextTxId();
    for (;;) {
        Status s = locks_.Acquire(txid, key, LockMode::kShared);
        if (s.code() == StatusCode::kBusy) {
            // Died under wait-die. Reusing the SAME txid ages us toward oldest
            // (guaranteed to eventually WAIT rather than die); yield to avoid a
            // hot spin while an older holder finishes.
            locks_.ReleaseAll(txid);
            std::this_thread::yield();
            continue;
        }
        if (!s.ok()) { locks_.ReleaseAll(txid); return std::unexpected(s); }
        auto got = tree_.Get(key);
        locks_.ReleaseAll(txid);   // read-only: no markers logged
        return got;
    }
}

// ------------------------------------------------------------------------- Txn

Txn::~Txn() {
    if (!finished_) {
        (void)Abort();  // RAII safety: never leak locks / partial state
    }
}

Status Txn::EnsureBegun() {
    if (began_logged_) return Status::Ok();
    if (Status s = mgr_.LogTxnMarker(wal::LogRecordType::kBegin, txid_); !s.ok()) {
        return s;
    }
    began_logged_ = true;
    return Status::Ok();
}

Result<std::string> Txn::Get(std::string_view key) {
    if (finished_) return std::unexpected(Status::Internal("Txn already finished"));
    Status s = mgr_.locks().Acquire(txid_, key, LockMode::kShared);
    if (!s.ok()) return std::unexpected(s);  // kBusy -> caller aborts
    return mgr_.tree().Get(key);
}

Status Txn::Put(std::string_view key, std::string_view value) {
    if (finished_) return Status::Internal("Txn already finished");
    Status s = mgr_.locks().Acquire(txid_, key, LockMode::kExclusive);
    if (!s.ok()) return s;                    // kBusy -> caller aborts
    if (Status b = EnsureBegun(); !b.ok()) return b;

    std::optional<std::string> before;
    if (auto got = mgr_.tree().Get(key); got) before = *got;  // under X-lock

    if (Status w = mgr_.tree().Put(key, value, txid_, before); !w.ok()) return w;
    undo_.push_back({std::string(key), before});
    return Status::Ok();
}

Status Txn::Delete(std::string_view key) {
    if (finished_) return Status::Internal("Txn already finished");
    Status s = mgr_.locks().Acquire(txid_, key, LockMode::kExclusive);
    if (!s.ok()) return s;
    if (Status b = EnsureBegun(); !b.ok()) return b;

    std::optional<std::string> before;
    if (auto got = mgr_.tree().Get(key); got) before = *got;
    else return Status::NotFound("Key not found");  // nothing to delete

    if (Status w = mgr_.tree().Delete(key, txid_, before); !w.ok()) return w;
    undo_.push_back({std::string(key), before});
    return Status::Ok();
}

Status Txn::Commit() {
    if (finished_) return Status::Internal("Txn already finished");
    finished_ = true;
    if (began_logged_) {  // read-only txns logged nothing -> nothing to commit
        if (Status s = mgr_.LogTxnMarker(wal::LogRecordType::kCommit, txid_); !s.ok()) {
            return s;
        }
        if (mgr_.options().sync_on_write) {
            if (Status s = mgr_.wal().Flush(); !s.ok()) return s;
        }
    }
    mgr_.locks().ReleaseAll(txid_);
    return Status::Ok();
}

Status Txn::Abort() {
    if (finished_) return Status::Ok();
    finished_ = true;
    // Replay inverses newest->oldest as normal logged records under this txid.
    // Absolute put/delete => idempotent; a later kAbort marks us a "winner" so
    // recovery redoes these inverses instead of undoing again.
    Status result = Status::Ok();
    for (auto it = undo_.rbegin(); it != undo_.rend() && result.ok(); ++it) {
        if (it->before.has_value()) {
            result = mgr_.tree().Put(it->key, *it->before, txid_, std::nullopt);
        } else {
            Status d = mgr_.tree().Delete(it->key, txid_, std::nullopt);
            if (!d.ok() && d.code() != StatusCode::kNotFound) result = d;
        }
    }
    if (began_logged_) {
        if (Status s = mgr_.LogTxnMarker(wal::LogRecordType::kAbort, txid_);
            !s.ok() && result.ok()) {
            result = s;
        }
        if (mgr_.options().sync_on_write) {
            if (Status s = mgr_.wal().Flush(); !s.ok() && result.ok()) result = s;
        }
    }
    mgr_.locks().ReleaseAll(txid_);
    return result;
}

}  // namespace kvstore::txn
```

- [ ] **Step 6: Add `DB::Begin()` to the public API**

In `include/kvstore/db.h`, add the include near the top (after `#include "kvstore/status.h"`):

```cpp
#include "kvstore/txn/txn.h"
```

Add the pure-virtual in the Core Operations section (after `Contains`):

```cpp
    // -------------------------------------------------------------------------
    // Transactions (Phase 5)
    // -------------------------------------------------------------------------

    // Begin an explicit transaction. The returned handle supports
    // Get/Put/Delete/Commit/Abort with serializable isolation for point ops
    // (rigorous 2PL). db->Get/Put/Delete remain autocommit single-op txns.
    [[nodiscard("Begin returns the transaction handle")]]
    virtual std::unique_ptr<txn::Txn> Begin() = 0;
```

- [ ] **Step 7: Wire `DbImpl` — members, autocommit routing, `Begin()`, drop `needs_reopen_`**

In `src/db.cc`, add the include:

```cpp
#include "kvstore/txn/txn.h"
```

Add members after `tree_` (order matters: `TxnManager` references `tree_`, `wal_writer_`, `options_`):

```cpp
    txn::TxnManager txn_mgr_;    // References tree_ + wal_writer_ + options_
```

Initialize it in the constructor initializer list (after `tree_(pool_, &wal_writer_)`):

```cpp
        , tree_(pool_, &wal_writer_)
        , txn_mgr_(tree_, wal_writer_, options_)
```

Remove the `bool needs_reopen_ = false;` member and both `if (needs_reopen_) ...` guards + the `needs_reopen_ = true;` assignments.

Replace `Get`, `Put`, `Delete` bodies with autocommit routing:

```cpp
    [[nodiscard]]
    Result<std::string> Get(std::string_view key) override {
        return txn_mgr_.AutocommitGet(key);
    }

    [[nodiscard]]
    Status Put(std::string_view key, std::string_view value) override {
        if (Status s = txn_mgr_.AutocommitPut(key, value); !s.ok()) return s;
        return MaybeAutoCheckpoint();
    }

    [[nodiscard]]
    Status Delete(std::string_view key) override {
        if (Status s = txn_mgr_.AutocommitDelete(key); !s.ok()) return s;
        return MaybeAutoCheckpoint();
    }
```

Add `Begin()`:

```cpp
    [[nodiscard]]
    std::unique_ptr<txn::Txn> Begin() override {
        return txn_mgr_.Begin();
    }
```

> `Contains` currently calls `tree_.Contains` (which calls `Get`); leave it routing straight to the tree (a `Contains` need not take a lock for these tests) OR route via `AutocommitGet` for consistency. Keep `return tree_.Contains(key);` — unchanged.

- [ ] **Step 8: Run the transaction tests**

Run: `cmake --build build/asan -j && ctest --preset asan -R txn_test --output-on-failure`
Expected: PASS (commit persists, abort rolls back updates+inserts, autocommit works).

- [ ] **Step 9: Verify & checkpoint (NO commit)**

Run the full asan suite (existing db/btree/recovery tests must stay green with the new autocommit path):
`ctest --preset asan`
Expected: PASS. Then tsan on the concurrency tests: `cmake --build build/tsan -j && ctest --preset tsan -R "btree_concurrent_test|lock_manager_test"`
Expected: PASS. Do **not** commit.

---

## Task 5: Recovery rewrite — redo-all + undo-losers (5.4b)

Replace redo-only recovery with ARIES-lite: redo every complete op-group (including uncommitted), then undo the losers (txns with data records but no `kCommit`/`kAbort`) via logical before-image inverses.

**Files:**
- Modify: `src/db.cc` (`Recover` + a new `UndoLosers` helper)
- Create: `tests/txn_recovery_test.cc`
- Modify: `CMakeLists.txt` (add `txn_recovery_test`)

**Interfaces:**
- Consumes: `LogRecord::{txid,type,before,key,page_id,lsn}`; `tree_.Put/Delete(key,txid,before)`; `tree_.SeekToFirst`; `pager_.SetNumKeys`.
- Produces: `Status DbImpl::Recover(const std::vector<wal::LogRecord>& records)` (redo-all + undo-losers); the `needs_reopen_` hack is gone (mid-op failures now roll back).

- [ ] **Step 1: Write the failing recovery test**

Create `tests/txn_recovery_test.cc`:

```cpp
// ==============================================================================
// tests/txn_recovery_test.cc - redo-all + undo-losers recovery (Phase 5.4)
// ==============================================================================
// A child process performs transactional writes then _exit()s WITHOUT a clean
// shutdown (no destructor -> no checkpoint) - the fork+_exit pattern from
// crash_test, so LeakSanitizer never sees the abandoned handles. The parent
// reopens (running recovery) and checks: committed txns survive; loser txns
// (writes flushed to the WAL, no kCommit/kAbort) are fully undone.
// ==============================================================================

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <system_error>

#include "kvstore/db.h"

namespace kvstore {
namespace {

namespace fs = std::filesystem;

Options CrashSafe() {
    Options o;
    o.create_if_missing = true;
    o.sync_on_write = true;
    o.max_wal_bytes = 0;   // never auto-checkpoint: keep the WAL for replay
    return o;
}

// Child: commit a transaction, then _exit(0) with no clean shutdown. The WAL
// holds kBegin/kPut/kPut/kCommit (flushed by Commit under sync_on_write); the
// data file is un-checkpointed, so recovery must REDO the committed writes.
[[noreturn]] void ChildCommit(const fs::path& dir) {
    auto db = DB::Open(dir.string(), CrashSafe()).value();
    auto t = db->Begin();
    if (!t->Put("a", "1").ok()) _exit(4);
    if (!t->Put("b", "2").ok()) _exit(4);
    if (!t->Commit().ok()) _exit(4);
    _exit(0);  // "crash": no destructor, no checkpoint
}

// Child: write a committed baseline (autocommit), then a second transaction
// that writes but never commits; Sync() flushes the loser's records to the WAL,
// then _exit(0) mid-transaction. Recovery must REDO then UNDO the loser.
[[noreturn]] void ChildLoser(const fs::path& dir) {
    auto db = DB::Open(dir.string(), CrashSafe()).value();
    if (!db->Put("a", "orig").ok()) _exit(4);   // committed baseline
    auto t = db->Begin();
    if (!t->Put("a", "changed").ok()) _exit(4);
    if (!t->Put("c", "new").ok()) _exit(4);
    if (!db->Sync().ok()) _exit(4);   // flush the loser's uncommitted records
    _exit(0);  // "crash" mid-txn: no kCommit, no kAbort, no checkpoint
}

int RunChild(void (*body)(const fs::path&), const fs::path& dir) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) body(dir);   // never returns
    int wstatus = 0;
    if (waitpid(pid, &wstatus, 0) != pid) return -2;
    return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -3;
}

TEST(TxnRecovery, CommittedSurvives) {
    const fs::path dir = fs::path(::testing::TempDir()) / "kvstore_txn_recovery_commit";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_EQ(RunChild(ChildCommit, dir), 0);

    Options o;
    o.create_if_missing = false;
    auto db = DB::Open(dir.string(), o).value();
    EXPECT_EQ(db->Get("a").value(), "1");
    EXPECT_EQ(db->Get("b").value(), "2");
    db.reset();
    fs::remove_all(dir, ec);
}

TEST(TxnRecovery, UncommittedLoserUndone) {
    const fs::path dir = fs::path(::testing::TempDir()) / "kvstore_txn_recovery_loser";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_EQ(RunChild(ChildLoser, dir), 0);

    Options o;
    o.create_if_missing = false;
    auto db = DB::Open(dir.string(), o).value();
    EXPECT_EQ(db->Get("a").value(), "orig");                        // loser undone
    EXPECT_EQ(db->Get("c").error().code(), StatusCode::kNotFound);  // loser undone
    db.reset();
    fs::remove_all(dir, ec);
}

}  // namespace
}  // namespace kvstore
```

- [ ] **Step 2: Register and run to verify it fails**

Add `txn_recovery_test` to `KVSTORE_TESTS`. Run:
`cmake --build build/asan -j && ctest --preset asan -R txn_recovery_test --output-on-failure`
Expected: FAIL — `UncommittedLoserUndone` shows `a == "changed"` and `c == "new"` (current redo-only recovery keeps uncommitted writes).

- [ ] **Step 3: Rewrite `Recover` to tally winners and undo losers**

In `src/db.cc`, replace the tail of `Recover` (the `if (applied_any) { ... recount ... }` block) so that it first undoes losers, then recounts. Add loser-tracking to the op-group loop and a helper. Full replacement for the method body from the group loop onward:

```cpp
        // Apply records in op groups, all-or-nothing (unchanged), while tallying
        // per-txid which transactions committed or aborted (their records are
        // "winners"; everyone else with data records is a "loser" to undo).
        std::unordered_map<TxId, bool> settled;   // txid -> saw kCommit or kAbort
        std::vector<const wal::LogRecord*> undo_candidates;  // kPut/kDelete, in order

        std::vector<const wal::LogRecord*> group;
        for (const auto& record : records) {
            if (record.lsn < checkpoint) continue;  // already in checkpointed pages
            if (record.type == wal::LogRecordType::kCommit ||
                record.type == wal::LogRecordType::kAbort) {
                settled[record.txid] = true;
            }
            if ((record.type == wal::LogRecordType::kPut ||
                 record.type == wal::LogRecordType::kDelete) &&
                record.txid != kInvalidTxId) {
                undo_candidates.push_back(&record);
            }
            group.push_back(&record);
            if (!record.op_end) continue;
            for (const wal::LogRecord* rec_ptr : group) {
                if (Status s = ApplyRecord(*rec_ptr, ensure_page_exists, applied_any);
                    !s.ok()) [[unlikely]] {
                    return s;
                }
            }
            group.clear();
        }
        // Any leftover group is an operation that never completed: dropped.

        // Undo losers: newest -> oldest, apply the logical inverse of each
        // kPut/kDelete belonging to a txn that never committed or aborted.
        // Absolute put/delete inverses are idempotent, and Open() checkpoints
        // immediately after recovery, so a crash mid-recovery re-runs this from
        // the same checkpoint with the identical result (no CLRs needed).
        for (auto it = undo_candidates.rbegin(); it != undo_candidates.rend(); ++it) {
            const wal::LogRecord& rec = **it;
            if (settled.contains(rec.txid)) continue;     // winner: redo-only
            applied_any = true;
            if (rec.before.has_value()) {
                // Undo of insert-with-prior / update / delete => restore prior.
                if (Status s = tree_.Put(rec.key, *rec.before, kInvalidTxId, std::nullopt);
                    !s.ok()) [[unlikely]] return s;
            } else {
                // Undo of an insert of a previously-absent key => delete it.
                if (Status s = tree_.Delete(rec.key, kInvalidTxId, std::nullopt);
                    !s.ok() && s.code() != StatusCode::kNotFound) [[unlikely]] {
                    return s;
                }
            }
        }

        if (applied_any) {
            uint64_t count = 0;
            auto cursor = tree_.SeekToFirst();
            for (; cursor.Valid(); cursor.Next()) ++count;
            if (!cursor.status().ok()) [[unlikely]] return cursor.status();
            if (Status s = pager_.SetNumKeys(count); !s.ok()) [[unlikely]] return s;
        }
        return Status::Ok();
    }
```

Add `#include <unordered_map>` to `src/db.cc` if not already present.

Also handle the new record types in `ApplyRecord`'s `switch (record.type)` (they carry no page effect — the markers are consumed by the tally loop above, not by redo). Extend the final `case wal::LogRecordType::kFreePage: case wal::LogRecordType::kCheckpoint: break;` line to:

```cpp
                case wal::LogRecordType::kFreePage:   // Nothing emits this yet
                case wal::LogRecordType::kCheckpoint:
                case wal::LogRecordType::kBegin:      // Txn markers: no page effect
                case wal::LogRecordType::kCommit:
                case wal::LogRecordType::kAbort:
                    break;
```

(Without this the switch is non-exhaustive and `-Wswitch` warns; `-Werror` is off, so it would only warn, but keep it clean.)

> The undo path calls `tree_.Put/Delete` with `txid = kInvalidTxId`: recovery is single-threaded, so the tree's `root_latch`/page latches are uncontended, and passing the invalid txid keeps the compensating writes out of the winner/loser tally on a subsequent recovery (they are logged, redone idempotently, and re-undone identically — safe).

- [ ] **Step 4: Run the recovery tests**

Run: `cmake --build build/asan -j && ctest --preset asan -R "txn_recovery_test|recovery_test|crash_test" --output-on-failure`
Expected: PASS — committed survives, loser undone, and the pre-existing Phase-4 recovery/crash tests still pass (redo-all is a superset of the old redo behavior; winners with no explicit txid, e.g. legacy records, are treated as settled-by-absence... see Step 5).

- [ ] **Step 5: Handle legacy/txid-less records (defensive)**

Records written before this phase (or structural records) carry `txid == kInvalidTxId (0)`. They are **not** undo candidates (guarded by `record.txid != kInvalidTxId`), so they are always redo-only — correct: a pre-txn autocommit write had no rollback and must persist. Confirm the guard is present in Step 3's loop. Re-run `ctest --preset asan` (full suite).
Expected: PASS.

- [ ] **Step 6: Verify & checkpoint (NO commit)**

Full asan: `ctest --preset asan` → PASS.
tsan concurrency subset: `cmake --build build/tsan -j && ctest --preset tsan -R "btree_concurrent_test|lock_manager_test"` → PASS.
Do **not** commit.

---

## Task 6: Anomaly tests — demonstrate the isolation level (5.5)

Prove serializable point-isolation with barrier-forced interleavings: dirty read, lost update, write-write conflict.

**Files:**
- Create: `tests/anomaly_test.cc`
- Modify: `CMakeLists.txt` (add `anomaly_test`)

**Interfaces:**
- Consumes: `DB::Begin()` → `Txn`; `Txn::{Get,Put,Commit,Abort}`; `StatusCode::kBusy`.

- [ ] **Step 1: Write the anomaly tests**

Create `tests/anomaly_test.cc`:

```cpp
// ==============================================================================
// tests/anomaly_test.cc - Isolation anomalies under rigorous 2PL (Phase 5.5)
// ==============================================================================
// Run under tsan too: ctest --preset tsan -R anomaly_test
// ==============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <filesystem>
#include <thread>

#include "kvstore/db.h"

namespace {

using namespace kvstore;

std::unique_ptr<DB> OpenFresh(const std::string& dir) {
    std::filesystem::remove_all(dir);
    Options o; o.create_if_missing = true;
    return DB::Open(dir, o).value();
}

// Write-write conflict: T1 holds X on k; T2's X on k must WAIT or DIE (kBusy),
// never silently proceed. Younger T2 dies under wait-die.
TEST(Anomaly, WriteWriteConflictSerializes) {
    auto db = OpenFresh("/tmp/kvstore_anomaly_ww");
    auto t1 = db->Begin();               // older
    auto t2 = db->Begin();               // younger
    ASSERT_TRUE(t1->Put("k", "t1").ok());

    Status s = t2->Put("k", "t2");       // conflict: younger -> dies
    EXPECT_EQ(s.code(), StatusCode::kBusy);
    ASSERT_TRUE(t2->Abort().ok());
    ASSERT_TRUE(t1->Commit().ok());
    EXPECT_EQ(db->Get("k").value(), "t1");
}

// Dirty read prevented: T1 writes k (uncommitted); T2's read of k must not see
// "dirty" (younger T2 requesting S conflicts with T1's X -> dies), and after
// T1 commits, T2 (fresh) reads the committed value.
TEST(Anomaly, NoDirtyRead) {
    auto db = OpenFresh("/tmp/kvstore_anomaly_dirty");
    ASSERT_TRUE(db->Put("k", "clean").ok());

    auto t1 = db->Begin();               // older
    auto t2 = db->Begin();               // younger
    ASSERT_TRUE(t1->Put("k", "dirty").ok());

    auto r = t2->Get("k");               // younger reader conflicts -> dies
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), StatusCode::kBusy);
    ASSERT_TRUE(t2->Abort().ok());
    ASSERT_TRUE(t1->Commit().ok());

    EXPECT_EQ(db->Get("k").value(), "dirty");  // committed value now visible
}

// Lost update prevented: two threads each run T: read k, write k+1, commit;
// wait-die makes the loser retry (autocommit-style loop here), so the final
// value reflects BOTH increments (no lost update).
TEST(Anomaly, NoLostUpdate) {
    auto db = OpenFresh("/tmp/kvstore_anomaly_lost");
    ASSERT_TRUE(db->Put("n", "0").ok());

    auto rmw_once = [&] {
        for (;;) {
            auto t = db->Begin();
            auto cur = t->Get("n");
            if (!cur) { (void)t->Abort(); continue; }   // kBusy: retry
            int v = std::stoi(*cur) + 1;
            if (Status s = t->Put("n", std::to_string(v)); !s.ok()) {
                (void)t->Abort(); continue;
            }
            if (t->Commit().ok()) return;
        }
    };
    std::thread a(rmw_once), b(rmw_once);
    a.join(); b.join();
    EXPECT_EQ(db->Get("n").value(), "2");   // both increments applied
}

}  // namespace
```

- [ ] **Step 2: Register and run to verify (defines behavior)**

Add `anomaly_test` to `KVSTORE_TESTS`. Run:
`cmake --build build/asan -j && ctest --preset asan -R anomaly_test --output-on-failure`
Expected: PASS. If `WriteWriteConflictSerializes` or `NoDirtyRead` fails with the conflicting op returning `Ok` instead of `kBusy`, the lock is not being taken before the tree op — revisit Task 4 `Txn::Put/Get`.

> These tests assert wait-die's *younger dies* outcome directly (deterministic because `Begin()` order fixes the txid order). No sleeps; `NoLostUpdate` uses a retry loop, the others use fixed older/younger ordering.

- [ ] **Step 3: Verify under tsan**

Run: `cmake --preset tsan && cmake --build build/tsan -j && ctest --preset tsan -R anomaly_test`
Expected: PASS, tsan-clean.

- [ ] **Step 4: Verify & checkpoint (NO commit)**

Full asan suite (all correctness, including the fork-based recovery/crash harnesses):
`ctest --preset asan` → PASS.

C5 stress gate — the genuinely concurrent tests under tsan (fork-based crash tests are excluded here; they run under asan):
`cmake --build build/tsan -j && ctest --preset tsan -R "btree_concurrent_test|buffer_pool_concurrent_test|lock_manager_test|txn_test|anomaly_test"`
→ PASS, tsan-clean (concurrent tree, lock manager, anomalies, buffer pool).
Do **not** commit.

---

## Task 7: Documentation, CI, and Checkpoint C5

Record the concurrency lifecycle and decisions, update status, and make the tsan suite a CI gate. No production code changes.

**Files:**
- Modify: `docs/ARCHITECTURE.md`, `docs/DESIGN_DECISIONS.md`, `docs/FORMAT.md`, `README.md`, `phases.md`, `.github/workflows/ci.yml`

**Interfaces:** none (docs/CI only).

- [ ] **Step 1: Document the WAL format change in `docs/FORMAT.md`**

Add a Phase-5 subsection to the WAL record section:

```markdown
### WAL record — Phase 5 additions

Record layout gains a `before_len:2` field after `val_len`:

    [lsn:8][txid:8][type:1][page_id:4][key_len:2][val_len:2][before_len:2]
           [key][value][before][crc:4]

- `before_len == 0xFFFF` — the key had no prior value (an insert); no `before`
  bytes follow. Any other value is the prior value's byte length (0 allowed).
- `kHeaderSize` grows from 25 to 27 bytes.
- New record types: `kBegin=8`, `kCommit=9`, `kAbort=10` (payload-free markers,
  `op_end` set). `txid` is now a real, monotone transaction id (was always 0).
- The v3 **data-file** format is unchanged. Old WAL files are not forward-
  compatible (pre-1.0 learning project; recovery of a pre-Phase-5 WAL is not
  supported).
```

- [ ] **Step 2: Record decisions in `docs/DESIGN_DECISIONS.md`**

Append the Phase-5 decision table (copy the spec §9 table): tree concurrency = exclusive-crab with release-when-safe at the leaf; root protection = pool-owned `root_latch`; isolation = rigorous 2PL → serializable (point ops); lock deadlock = wait-die; latch deadlock = crabbing order + "no lock-wait under a latch"; undo = logical before-images; recovery = redo-all + undo-losers, no CLRs; txn API = explicit `Begin()` + autocommit; scan isolation = read-committed-per-leaf (no range locks). Note the pessimistic root_latch (writers exclusive up front) and the deferred shared-writer/optimistic-restart refinement.

- [ ] **Step 3: Describe the concurrency lifecycle in `docs/ARCHITECTURE.md`**

Add a "Concurrency & transactions (Phase 5)" section: the `DB::Begin() → Txn → TxnManager/LockManager → BPlusTree (crabbing) → BufferPool (+root_latch) → WAL` stack; the deadlock-freedom invariant (lock before latch, never wait on a lock under a latch); wait-die; redo-all + undo-losers with the restart-safety argument (immediate post-recovery checkpoint + idempotent logical undo ⇒ no CLRs).

- [ ] **Step 4: Update `README.md` status and `phases.md`**

`README.md`: mark the engine as concurrent + transactional (serializable point ops); note scans are read-committed-per-leaf and MVCC (5.6) is deferred.

`phases.md`: mark 5.2, 5.3, 5.4, 5.5 done; record **Checkpoint C5** reached (tsan-clean stress + anomaly suite; crash harness green with concurrent writers); note 5.6 MVCC explicitly deferred with rationale; note the `v0.5-phase5` tag is deferred (no commits — standing rule).

- [ ] **Step 5: Make tsan a CI gate in `.github/workflows/ci.yml`**

Ensure a job configures/builds/tests the `tsan` preset and runs at least the concurrency suite:

```yaml
  tsan:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Configure + build (tsan)
        run: cmake --preset tsan && cmake --build build/tsan -j
      - name: Concurrency + anomaly + txn suite (tsan)
        run: ctest --preset tsan -R "btree_concurrent_test|buffer_pool_concurrent_test|lock_manager_test|txn_test|anomaly_test" --output-on-failure
```

> `txn_recovery_test` and `crash_test` are fork-based and run under the **asan**
> job (add them there if the asan job uses an explicit `-R` filter); keep them
> out of the tsan filter.

(Adapt to the existing CI structure — matrix entry or standalone job — matching how the `asan` job is defined.)

- [ ] **Step 6: Verify & checkpoint (NO commit)**

Sanity re-run both gates one final time:
`ctest --preset asan` → PASS (full suite, incl. fork-based recovery/crash).
`cmake --build build/tsan -j && ctest --preset tsan -R "btree_concurrent_test|buffer_pool_concurrent_test|lock_manager_test|txn_test|anomaly_test"` → PASS, tsan-clean.
Confirm the working tree holds all Phase-5 changes **uncommitted**. Do **not** commit or tag — the user commits manually.

---

## Notes for the executor

- **Standing rule:** never `git commit`, `git add`, or tag. Every "Verify & checkpoint" step is a build+test gate only.
- **asan vs tsan are mutually exclusive** sanitizers — build each preset separately; run concurrency tests under tsan and everything under asan.
- If a tsan run reports a data race, the culprit is almost always a page accessed without the right latch (audit for a lingering `FetchPage` where `FetchPageRead`/`FetchPageWrite` is needed) or a tree field read outside `root_latch`. Do **not** paper over it by widening a mutex; fix the latch.
- Task order is dependency-driven: 1 (crabbing) → 2 (WAL format) → 3 (LockManager) → 4 (Txn integration, depends on 2+3) → 5 (recovery, depends on 2+4) → 6 (anomalies, depends on 4) → 7 (docs/CI).
```
