// ==============================================================================
// kvstore/btree/btree.cc - B+ Tree Implementation (C++23)
// ==============================================================================
//
// This file implements the core B+ tree operations: Get, Put, Delete, Seek.
//
// Phase 3: all page access goes through the BufferPool. Reads fetch a pinned
// frame (PageGuard) and mutations happen in place followed by MarkDirty();
// nothing on this path calls Pager::WritePage directly anymore - dirty
// frames reach disk on eviction or FlushAll. Meta operations (root page id,
// num_keys) still go straight through the Pager.
//
// Guard budget (documented in buffer_pool.h): reads hold one guard at a
// time. A splitting Put holds its whole root-to-leaf path (pessimistic
// descent) PLUS every page the operation rewrites - split halves and the
// relinked sibling stay pinned until the op's final (op-end) WAL record is
// appended, so an eviction can never write half an op group to data.db
// (recovery drops groups without a durable op-end). Worst case, a full
// root cascade pins ~2*depth+2 frames; DbImpl clamps the pool to 16 frames
// minimum (covers depth 7).
//
// Key Algorithms:
// ---------------
//
// GET (Search):
//   1. Start at root
//   2. At each internal node, find child to descend into
//   3. At leaf, binary search for key
//   4. Return value if found
//
// PUT (Insert):
//   1. Find the leaf that should contain the key
//   2. If key exists, update value (or delete + reinsert if it outgrew the leaf)
//   3. If key doesn't exist and leaf has space, insert
//   4. If leaf is full, split:
//      a. Create new leaf with upper half of keys
//      b. Insert new key into appropriate leaf
//      c. Push split key up to parent
//      d. If parent is full, recursively split parent
//      e. If root splits, create new root
//
// Split Logic:
// ------------
//
// Leaf Split:
//   Before: [A B C D E] (full)
//   After:  [A B C] -> [D E]  (push D up to parent)
//
// Internal Split:
//   Before: [K1 K2 K3 K4 K5] with children [C0 C1 C2 C3 C4 C5]
//   After:  [K1 K2] [C0 C1 C2]  and  [K4 K5] [C3 C4 C5]
//           Push K3 up to parent
//
// ==============================================================================

#include "kvstore/btree/btree.h"

#include <cassert>
#include <cstring>
#include <mutex>
#include <ranges>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace kvstore::btree {

// ==============================================================================
// Constructor
// ==============================================================================

BPlusTree::BPlusTree(storage::BufferPool& pool, wal::LogWriter* wal)
    : pool_(pool)
    , wal_(wal)
{}

// ==============================================================================
// WAL emission helpers (Phase 4) - all no-ops when wal_ is null
// ==============================================================================

Status BPlusTree::LogKeyRecord(wal::LogRecordType type, TxId txid,
                               storage::PageGuard& guard,
                               std::string_view key, std::string_view value,
                               const std::optional<std::string>& before,
                               bool op_end, bool needs_undo) {
    guard.MarkDirty();
    if (wal_ == nullptr) {
        return Status::Ok();
    }
    // Torn-page protection: the first record for this page since the last
    // checkpoint must be a full image (the mutation is already applied to the
    // page, so the image subsumes it for redo). For an autocommit op that is
    // the whole story. A transactional op still needs its before-image on a
    // separate delta record for recovery undo, so the image becomes redo-only
    // (op_end deferred to the delta) and we fall through.
    if (wal_->NeedsImage(guard.page_id())) {
        if (Status s = LogPageImage(guard, txid, /*op_end=*/op_end && !needs_undo);
            !s.ok()) [[unlikely]] {
            return s;
        }
        if (!needs_undo) {
            return Status::Ok();  // Image subsumes the mutation
        }
    }
    // Delta record: redo (idempotent when an image preceded it) plus the undo
    // before-image (nullopt marks a fresh insert; undo of that is a delete).
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

Status BPlusTree::LogPageImage(storage::PageGuard& guard, TxId txid,
                               bool op_end) {
    guard.MarkDirty();
    if (wal_ == nullptr) {
        return Status::Ok();
    }
    // AppendPageImage stamps page_lsn and serializes the page atomically under
    // the writer's lock, so a concurrent appender can't steal the LSN the
    // image embeds (Phase 5). The caller holds this page's exclusive latch.
    auto lsn = wal_->AppendPageImage(guard.page_id(), guard.page(), txid, op_end);
    if (!lsn) [[unlikely]] {
        return lsn.error();
    }
    return Status::Ok();
}

Status BPlusTree::LogPageEvent(wal::LogRecordType type, TxId txid,
                               PageId page_id, bool op_end) {
    if (wal_ == nullptr) {
        return Status::Ok();
    }
    wal::LogRecord record;
    record.type = type;
    record.txid = txid;
    record.page_id = page_id;
    record.op_end = op_end;
    auto lsn = wal_->Append(record);
    if (!lsn) [[unlikely]] {
        return lsn.error();
    }
    return Status::Ok();
}

// ==============================================================================
// Get
// ==============================================================================

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

// ==============================================================================
// Delete
// ==============================================================================

Status BPlusTree::Delete(std::string_view key, TxId txid,
                         std::optional<std::string>* before_out) {
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
    // Capture the value being removed BEFORE it is gone (the view dies with
    // DeleteAt): feeds the caller's undo list and the WAL undo before-image.
    // Only transactional deletes need it (autocommit is never rolled back).
    std::optional<std::string> before;
    if (before_out != nullptr) {
        before = std::string(node.GetValueAt(index));
        *before_out = before;
    }
    if (!node.DeleteAt(index)) [[unlikely]] {
        return Status::Internal("DeleteAt failed for a found key");
    }
    if (Status s = LogKeyRecord(wal::LogRecordType::kDelete, txid, leaf_guard,
                                key, {}, before, /*op_end=*/true,
                                /*needs_undo=*/before_out != nullptr);
        !s.ok()) [[unlikely]] {
        return s;
    }
    // The leaf stays in the sibling chain even when empty (decision 2.8:
    // no underflow handling / page freeing in this phase).
    return pool_.pager().AddToNumKeys(-1);
}

// ==============================================================================
// Contains
// ==============================================================================

bool BPlusTree::Contains(std::string_view key) {
    return Get(key).has_value();
}

// ==============================================================================
// Put
// ==============================================================================

Status BPlusTree::Put(std::string_view key, std::string_view value, TxId txid,
                      std::optional<std::string>* before_out) {
    // No overflow pages (documented limitation, Phase 7 candidate): a record
    // must fit inside one page. Reject impossible records up front, BEFORE
    // any mutation - without this check an oversized Put failed deep inside
    // the split path with Internal, which also poisons the DB handle. This
    // bound is necessary, not sufficient: large records (roughly > 1/4 page)
    // can still overflow a count-based split when the leaf packs unluckily;
    // that failure is caught by the split code and the byte-aware split
    // point that would eliminate it is Phase 7 work alongside overflow
    // pages. Practical guidance in db.h: keep key+value under ~1 KB.
    constexpr size_t kMaxLeafRecordBytes =
        storage::kPageSize - sizeof(storage::PageHeader) -
        sizeof(storage::SlotEntry);  // 4048: empty leaf, one slot
    if (4 + key.size() + value.size() > kMaxLeafRecordBytes) [[unlikely]] {
        return Status::InvalidArgument(
            "key+value too large for a page (no overflow pages yet)");
    }

    // Optimistic fast path (Phase 5.2, "release when safe"): most Puts do not
    // split. Every structural change in this tree (leaf/internal split, root
    // change) runs entirely under an EXCLUSIVE root_latch - so a writer that
    // holds root_latch SHARED sees a frozen tree shape and can read-crab down
    // exactly like Get, then take just the leaf's exclusive latch. Concurrent
    // non-splitting writers on different leaves proceed in parallel; only a
    // writer that actually needs to split falls back to the pessimistic
    // full-path descent below.
    {
        std::shared_lock<std::shared_mutex> root_lk(pool_.root_latch());
        if (!IsEmpty()) [[likely]] {
            PageId current = pool_.pager().root_page_id();
            auto g = pool_.FetchPageRead(current);
            if (!g) [[unlikely]] return g.error();
            storage::PageGuard guard = std::move(*g);
            while (!NodeView(guard.page()).IsLeaf()) {
                PageId child = NodeView(guard.page()).FindChild(key);
                if (child == kInvalidPageId) [[unlikely]] {
                    return Status::Internal("Invalid child pointer");
                }
                auto cg = pool_.FetchPageRead(child);
                if (!cg) [[unlikely]] return cg.error();
                guard = std::move(*cg);
                current = child;
            }
            // Swap the leaf's shared latch for the exclusive one. The drop-
            // then-refetch window is safe: root_latch (held shared) blocks
            // every split, so `current` stays the correct target leaf and
            // cannot be repurposed (the tree never frees pages, decision 2.8).
            // Its records may change - re-searched below under the X latch.
            guard.Drop();
            auto lg = pool_.FetchPageWrite(current);
            if (!lg) [[unlikely]] return lg.error();
            storage::PageGuard leaf_guard = std::move(*lg);
            // The leaf is exclusively ours: no split can move our key range
            // away, so the shape freeze is no longer needed.
            root_lk.unlock();

            NodeView leaf_node(leaf_guard.page());
            bool found;
            uint16_t found_index = leaf_node.FindKeyIndex(key, found);
            std::optional<std::string> before;
            if (before_out != nullptr) {
                if (found) before = std::string(leaf_node.GetValueAt(found_index));
                *before_out = before;
            }
            if (found) {
                if (leaf_node.UpdateLeaf(key, value)) [[likely]] {
                    return LogKeyRecord(wal::LogRecordType::kPut, txid,
                                        leaf_guard, key, value, before,
                                        /*op_end=*/true,
                                        /*needs_undo=*/before_out != nullptr);
                }
                // Oversized update (delete + reinsert, possibly splitting):
                // NOT attempted here - mutating before knowing the reinsert
                // fits would strand the undo/num_keys bookkeeping. Restart
                // pessimistically with the page untouched.
            } else if (leaf_node.InsertLeaf(key, value)) [[likely]] {
                if (Status s = LogKeyRecord(wal::LogRecordType::kPut, txid,
                                            leaf_guard, key, value, before,
                                            /*op_end=*/true,
                                            /*needs_undo=*/before_out != nullptr);
                    !s.ok()) [[unlikely]] {
                    return s;
                }
                return pool_.pager().AddToNumKeys(1);
            }
            // Leaf is genuinely full (or the update outgrew it): this write
            // splits. Drop every latch and restart with the pessimistic
            // descent - the world may change in between, so it re-evaluates
            // everything from scratch.
        }
    }
    return PutPessimistic(key, value, txid, before_out);
}

Status BPlusTree::PutPessimistic(std::string_view key, std::string_view value,
                                 TxId txid,
                                 std::optional<std::string>* before_out) {
    // Writers hold root_latch EXCLUSIVE up front: this write may split the root
    // and rewrite root_page_id, and root_latch is the outermost lock (acquired
    // before any page latch) so the "read root_id + latch root" window and any
    // SetRootPageId stay atomic against concurrent root changes. Released the
    // moment the root is proven not to split. (This exclusive hold is also
    // what makes the optimistic fast path's shared hold a structure freeze.)
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
        if (before_out != nullptr) *before_out = std::nullopt;  // Fresh insert
        if (Status s = LogPageEvent(wal::LogRecordType::kAllocPage, txid, root_id);
            !s.ok()) [[unlikely]] return s;
        if (Status s = LogPageImage(*guard, txid); !s.ok()) [[unlikely]] return s;
        if (Status s = LogPageEvent(wal::LogRecordType::kRootChange, txid, root_id,
                                    /*op_end=*/true); !s.ok()) [[unlikely]] return s;
        if (Status s = pool_.pager().SetRootPageId(root_id); !s.ok()) return s;
        return pool_.pager().AddToNumKeys(1);
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

    // Undo before-image (transactional callers only): the prior value on an
    // update, or nullopt on a fresh insert. Captured before any mutation
    // invalidates the page view; feeds the caller's undo list and the WAL.
    std::optional<std::string> before;
    if (before_out != nullptr) {
        if (found) before = std::string(leaf_node.GetValueAt(found_index));
        *before_out = before;
    }

    // Release every ancestor latch (keep only the leaf) and drop root_latch -
    // used on every path that does NOT split the leaf.
    auto release_ancestors_and_root = [&]() {
        for (size_t i = 0; i + 1 < stack.size(); ++i) stack[i].Drop();
        if (root_lk.owns_lock()) root_lk.unlock();
    };

    if (found) [[unlikely]] {
        if (leaf_node.UpdateLeaf(key, value)) [[likely]] {
            release_ancestors_and_root();
            return LogKeyRecord(wal::LogRecordType::kPut, txid, leaf_guard, key,
                                value, before, /*op_end=*/true,
                                /*needs_undo=*/before_out != nullptr);
        }
        // New value no longer fits even after rebuild: remove old, fall through.
        if (!leaf_node.DeleteAt(found_index)) [[unlikely]] {
            return Status::Internal("Failed to remove record for oversized update");
        }
        leaf_guard.MarkDirty();
        replacing_grown_value = true;
    }

    // Try to insert. InsertLeaf compacts reclaimable dead space (EnsureFit) if
    // needed and only fails when the leaf is genuinely full, so it is the
    // precise "no split" oracle. (CanFit ignores dead_bytes, so gating the
    // split on it splits prematurely under Put/Delete churn and grows the file
    // unboundedly - see BTreeTest.FileSizeStopsGrowingUnderPutDeleteChurn.)
    // Success => no split => release ancestors + root_latch.
    if (leaf_node.InsertLeaf(key, value)) [[likely]] {
        release_ancestors_and_root();
        if (Status s = LogKeyRecord(wal::LogRecordType::kPut, txid, leaf_guard,
                                    key, value, before, /*op_end=*/true,
                                    /*needs_undo=*/before_out != nullptr);
            !s.ok()) [[unlikely]] return s;
        if (!replacing_grown_value) {
            return pool_.pager().AddToNumKeys(1);
        }
        return Status::Ok();
    }

    // Leaf splits - keep the ancestor stack and root_latch (the split may reach
    // the root). SplitLeaf latches the right sibling (rightward order).
    //
    // op_guards (WAL op-group atomicity, see BufferPool::EnforceWalRule):
    // every page this operation rewrites stays PINNED here until the group's
    // op-end record has been appended (which happens before this function
    // returns - the guards die after the final Log* call). Recovery drops a
    // record group whose op-end is not durable, assuming none of the group's
    // pages reached disk; an unpinned dirty page could be evicted - and
    // written - mid-group, breaking that assumption and leaving a
    // half-applied split on disk after a crash. Cost: up to 2*depth+2 pins
    // on a full root cascade (see buffer_pool.h minimum pool sizing).
    PageId leaf_id = leaf_guard.page_id();
    std::vector<storage::PageGuard> op_guards;
    auto split_result = SplitLeaf(leaf_guard, txid, op_guards);
    if (!split_result) [[unlikely]] return split_result.error();
    auto&& [new_leaf_guard, split_key] = *split_result;
    const PageId new_leaf_id = new_leaf_guard.page_id();

    if (key < split_key) {
        NodeView original_node(leaf_guard.page());
        if (!original_node.InsertLeaf(key, value)) [[unlikely]] {
            return Status::Internal("Failed to insert after split (left)");
        }
        if (Status s = LogKeyRecord(wal::LogRecordType::kPut, txid, leaf_guard,
                                    key, value, before, /*op_end=*/false,
                                    /*needs_undo=*/before_out != nullptr);
            !s.ok()) [[unlikely]] return s;
    } else {
        // The new right leaf is already exclusively latched (NewPage inside
        // SplitLeaf) - use the held guard, never re-fetch a page we hold.
        NodeView new_node(new_leaf_guard.page());
        if (!new_node.InsertLeaf(key, value)) [[unlikely]] {
            return Status::Internal("Failed to insert after split (right)");
        }
        if (Status s = LogKeyRecord(wal::LogRecordType::kPut, txid,
                                    new_leaf_guard, key, value, before,
                                    /*op_end=*/false,
                                    /*needs_undo=*/before_out != nullptr);
            !s.ok()) [[unlikely]] return s;
    }
    op_guards.push_back(std::move(new_leaf_guard));

    if (!replacing_grown_value) {
        if (Status s = pool_.pager().AddToNumKeys(1); !s.ok()) return s;
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
            return LogPageImage(parent_guard, txid, /*op_end=*/true);
        }
        auto internal_split = SplitInternal(parent_guard, txid);
        if (!internal_split) [[unlikely]] return internal_split.error();
        auto&& [new_internal_guard, new_split_key] = *internal_split;
        const PageId new_internal_id = new_internal_guard.page_id();
        PageId parent_id = parent_guard.page_id();
        if (key_to_insert < new_split_key) {
            NodeView orig_internal(parent_guard.page());
            if (!orig_internal.InsertInternalAfterSplit(key_to_insert, left_child,
                                                        right_child)) [[unlikely]] {
                return Status::Internal("insert into split internal (left)");
            }
            if (Status s = LogPageImage(parent_guard, txid); !s.ok()) [[unlikely]] return s;
        } else {
            // Held exclusively since NewPage inside SplitInternal.
            NodeView new_internal(new_internal_guard.page());
            if (!new_internal.InsertInternalAfterSplit(key_to_insert, left_child,
                                                       right_child)) [[unlikely]] {
                return Status::Internal("insert into split internal (right)");
            }
            if (Status s = LogPageImage(new_internal_guard, txid); !s.ok()) [[unlikely]] return s;
        }
        // Pinned until the op-end record is appended (see op_guards above).
        op_guards.push_back(std::move(new_internal_guard));
        left_child = parent_id;
        right_child = new_internal_id;
        key_to_insert = std::move(new_split_key);
    }

    // Reached the root and it split - create a new root (still under root_latch).
    return CreateNewRoot(key_to_insert, left_child, right_child, txid);
}

// ==============================================================================
// Cursors
// ==============================================================================

Cursor BPlusTree::Seek(std::string_view key) {
    Cursor cursor(pool_);
    // No unlatched IsEmpty() short-circuit here: root_page_id is written by
    // concurrent root changes under an exclusive root_latch, so it may only
    // be read under the latch - FindLeaf does the emptiness check there and
    // reports it as NotFound. (An unlatched check was both a data race and
    // a spurious "empty" result for a scan racing the first insert.)
    auto leaf_result = FindLeaf(key);
    if (!leaf_result) [[unlikely]] {
        if (leaf_result.error().code() != StatusCode::kNotFound) {
            cursor.status_ = leaf_result.error();
        }
        return cursor;  // Empty tree: invalid cursor, Ok status
    }

    // Position at the lower bound within the leaf; MoveTo hops forward over
    // leaf boundaries (and empty leaves) if the key sorts past this leaf.
    uint16_t lower_bound;
    {
        auto guard = pool_.FetchPageRead(*leaf_result);
        if (!guard) [[unlikely]] {
            cursor.status_ = guard.error();
            return cursor;
        }
        NodeView node(guard->page());
        lower_bound = node.LowerBound(key);
    }
    cursor.MoveTo(*leaf_result, lower_bound);
    return cursor;
}

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

// ==============================================================================
// FindLeaf
// ==============================================================================

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

// ==============================================================================
// SplitLeaf
// ==============================================================================

Result<std::pair<storage::PageGuard, std::string>> BPlusTree::SplitLeaf(
    storage::PageGuard& guard, TxId txid,
    std::vector<storage::PageGuard>& op_guards) {
    NodeView node(guard.page());
    uint16_t num_keys = node.NumKeys();
    uint16_t split_index = num_keys / 2;

    // Pin a fresh page for the right half (guard budget: 2 so far)
    auto new_guard_result = pool_.NewPage();
    if (!new_guard_result) [[unlikely]] {
        return std::unexpected(new_guard_result.error());
    }
    storage::PageGuard new_guard = std::move(*new_guard_result);
    PageId new_page_id = new_guard.page_id();

    // Log the allocation first: redo must extend the file before any image
    // record targets the new page.
    if (Status s = LogPageEvent(wal::LogRecordType::kAllocPage, txid, new_page_id);
        !s.ok()) [[unlikely]] {
        return std::unexpected(s);
    }

    // Collect entries for the new page (upper half)
    std::vector<std::pair<std::string, std::string>> right_entries;
    right_entries.reserve(num_keys - split_index);

    for (auto i : std::views::iota(split_index, num_keys)) {
        right_entries.emplace_back(std::string(node.GetKey(i)),
                                   std::string(node.GetValueAt(i)));
    }

    // Get the split key (first key of right page)
    std::string split_key(node.GetKey(split_index));

    // Collect entries for the left page (lower half)
    std::vector<std::pair<std::string, std::string>> left_entries;
    left_entries.reserve(split_index);

    for (auto i : std::views::iota(uint16_t{0}, split_index)) {
        left_entries.emplace_back(std::string(node.GetKey(i)),
                                  std::string(node.GetValueAt(i)));
    }

    // Setup leaf linking
    PageId old_next = node.GetNextLeaf();
    PageId old_prev = node.GetPrevLeaf();
    PageId original_page_id = guard.page_id();

    // Reinitialize original page with left entries
    guard.page().Init(original_page_id, storage::PageType::kLeaf);
    NodeView left_node(guard.page());
    left_node.InitLeaf();
    left_node.SetPrevLeaf(old_prev);
    left_node.SetNextLeaf(new_page_id);

    for (const auto& [k, v] : left_entries) {
        if (!left_node.InsertLeaf(k, v)) [[unlikely]] {
            return std::unexpected(Status::Internal("Leaf split rebuild (left) overflow"));
        }
    }
    if (Status s = LogPageImage(guard, txid); !s.ok()) [[unlikely]] {
        return std::unexpected(s);
    }

    // Initialize new page with right entries
    new_guard.page().Init(new_page_id, storage::PageType::kLeaf);
    NodeView right_node(new_guard.page());
    right_node.InitLeaf();
    right_node.SetPrevLeaf(original_page_id);
    right_node.SetNextLeaf(old_next);

    for (const auto& [k, v] : right_entries) {
        if (!right_node.InsertLeaf(k, v)) [[unlikely]] {
            return std::unexpected(Status::Internal("Leaf split rebuild (right) overflow"));
        }
    }
    if (Status s = LogPageImage(new_guard, txid); !s.ok()) [[unlikely]] {
        return std::unexpected(s);
    }

    // Update the old next's prev pointer.
    // FetchPageWrite: the sibling is mutated, so it must be exclusively latched
    // (rightward-only sibling latching keeps crabbing order acyclic). The
    // guard is parked in op_guards, NOT dropped here: its image was logged
    // without op_end, so the page must stay pinned (unevictable) until the
    // caller appends the group's op-end record - otherwise an eviction could
    // put this half of the split on disk while recovery would drop the
    // incomplete group (WAL op-group atomicity).
    if (old_next != kInvalidPageId) {
        auto old_next_guard = pool_.FetchPageWrite(old_next);
        if (!old_next_guard) [[unlikely]] {
            return std::unexpected(old_next_guard.error());
        }
        NodeView old_next_node(old_next_guard->page());
        old_next_node.SetPrevLeaf(new_page_id);
        if (Status s = LogPageImage(*old_next_guard, txid); !s.ok()) [[unlikely]] {
            return std::unexpected(s);
        }
        op_guards.push_back(std::move(*old_next_guard));
    }

    // The new right leaf's guard moves to the caller: it still needs it (the
    // key may land there) and it too must stay pinned until op-end.
    return std::pair{std::move(new_guard), std::move(split_key)};
}

// ==============================================================================
// SplitInternal
// ==============================================================================

Result<std::pair<storage::PageGuard, std::string>> BPlusTree::SplitInternal(
    storage::PageGuard& guard, TxId txid) {
    NodeView node(guard.page());
    uint16_t num_keys = node.NumKeys();
    uint16_t split_index = num_keys / 2;

    // Pin a fresh page for the right half
    auto new_guard_result = pool_.NewPage();
    if (!new_guard_result) [[unlikely]] {
        return std::unexpected(new_guard_result.error());
    }
    storage::PageGuard new_guard = std::move(*new_guard_result);
    PageId new_page_id = new_guard.page_id();

    if (Status s = LogPageEvent(wal::LogRecordType::kAllocPage, txid, new_page_id);
        !s.ok()) [[unlikely]] {
        return std::unexpected(s);
    }

    // The middle key goes up to the parent
    std::string split_key(node.GetKey(split_index));

    // Collect entries for the right page (keys after split_index)
    // Internal node has keys K0, K1, K2, K3, K4 and children C0, C1, C2, C3, C4, C5
    // After split at index 2 (K2):
    // Left:  K0, K1 with C0, C1, C2
    // Right: K3, K4 with C3, C4, C5
    // K2 goes up to parent

    std::vector<std::pair<std::string, PageId>> right_entries;
    right_entries.reserve(num_keys - split_index - 1);

    for (auto i : std::views::iota(static_cast<uint16_t>(split_index + 1), num_keys)) {
        right_entries.emplace_back(std::string(node.GetKey(i)), node.GetChild(i));
    }

    // Collect left entries
    std::vector<std::pair<std::string, PageId>> left_entries;
    left_entries.reserve(split_index);

    for (auto i : std::views::iota(uint16_t{0}, split_index)) {
        left_entries.emplace_back(std::string(node.GetKey(i)), node.GetChild(i));
    }

    // The "rightmost" child of left node is the child at split_index
    PageId left_rightmost = node.GetChild(split_index);

    // The rightmost child of right node is the old rightmost
    PageId right_rightmost = node.GetRightmostChild();

    PageId original_page_id = guard.page_id();

    // Reinitialize original page with left entries
    guard.page().Init(original_page_id, storage::PageType::kInternal);
    NodeView left_node(guard.page());
    left_node.InitInternal();
    left_node.SetRightmostChild(left_rightmost);

    for (const auto& [k, child] : left_entries) {
        if (!left_node.InsertInternal(k, child)) [[unlikely]] {
            return std::unexpected(Status::Internal("Internal split rebuild (left) overflow"));
        }
    }
    if (Status s = LogPageImage(guard, txid); !s.ok()) [[unlikely]] {
        return std::unexpected(s);
    }

    // Initialize new page with right entries
    new_guard.page().Init(new_page_id, storage::PageType::kInternal);
    NodeView right_node(new_guard.page());
    right_node.InitInternal();
    right_node.SetRightmostChild(right_rightmost);

    for (const auto& [k, child] : right_entries) {
        if (!right_node.InsertInternal(k, child)) [[unlikely]] {
            return std::unexpected(Status::Internal("Internal split rebuild (right) overflow"));
        }
    }
    if (Status s = LogPageImage(new_guard, txid); !s.ok()) [[unlikely]] {
        return std::unexpected(s);
    }

    // The guard moves to the caller: the separator may land in the new half,
    // and the page must stay pinned until the op-end record is appended.
    return std::pair{std::move(new_guard), std::move(split_key)};
}

// ==============================================================================
// CreateNewRoot
// ==============================================================================

Status BPlusTree::CreateNewRoot(std::string_view split_key,
                                 PageId left_child,
                                 PageId right_child,
                                 TxId txid) {
    auto guard = pool_.NewPage();
    if (!guard) [[unlikely]] {
        return guard.error();
    }

    PageId new_root_id = guard->page_id();
    if (Status s = LogPageEvent(wal::LogRecordType::kAllocPage, txid, new_root_id);
        !s.ok()) [[unlikely]] {
        return s;
    }
    guard->page().Init(new_root_id, storage::PageType::kInternal);

    NodeView root_node(guard->page());
    root_node.InitInternal();

    // Insert the split key with left_child
    if (!root_node.InsertInternal(split_key, left_child)) [[unlikely]] {
        return Status::Internal("Failed to insert split key into new root");
    }

    // Set right_child as the rightmost child
    root_node.SetRightmostChild(right_child);
    if (Status s = LogPageImage(*guard, txid); !s.ok()) [[unlikely]] {
        return s;
    }
    if (Status s = LogPageEvent(wal::LogRecordType::kRootChange, txid, new_root_id,
                                /*op_end=*/true);
        !s.ok()) [[unlikely]] {
        return s;
    }

    // Update the root page ID in the pager
    return pool_.pager().SetRootPageId(new_root_id);
}

}  // namespace kvstore::btree
