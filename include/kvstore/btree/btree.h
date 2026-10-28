// ==============================================================================
// kvstore/btree/btree.h - B+ Tree Implementation (C++23)
// ==============================================================================
//
// This file implements a B+ tree for the key-value store. The B+ tree provides:
//   - O(log n) search, insert, and delete
//   - Sorted key order (enables range scans)
//   - All values stored in leaf nodes (efficient for range queries)
//
// B+ Tree Structure:
// ------------------
//
//                    [Internal Node]
//                    keys: [M]
//                   /          \
//          [Leaf A]            [Leaf B]
//        keys: [A,B,C]       keys: [M,N,O]
//        values: [...]       values: [...]
//              |                   |
//              +--->---next--->----+  (leaf linked list)
//
// Key Properties:
//   1. All values are in leaf nodes
//   2. Internal nodes only store keys and child pointers
//   3. Leaf nodes are linked for efficient range scans
//   4. Keys are sorted within each node
//   5. For internal node key K: left child has keys < K, right child has keys >= K
//
// MVP Simplifications:
// --------------------
//   - No delete operation (keys can only be added)
//   - No concurrent access (single-threaded)
//   - No transactions (operations are immediate)
//
// C++23 Features Used:
// --------------------
//   - [[nodiscard("reason")]] with explanatory messages
//   - std::unreachable() for impossible code paths
//   - Deducing this for cleaner accessor methods
//   - std::ranges algorithms
//
// ==============================================================================

#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <utility>

#include "kvstore/types.h"
#include "kvstore/status.h"
#include "kvstore/storage/buffer_pool.h"
#include "kvstore/storage/pager.h"
#include "kvstore/btree/cursor.h"
#include "kvstore/btree/node.h"
#include "kvstore/wal/log_record.h"
#include "kvstore/wal/log_writer.h"

namespace kvstore::btree {

// ==============================================================================
// BPlusTree Class
// ==============================================================================
//
// Usage:
//   Pager pager = Pager::Open("db.dat", true).value();
//   BPlusTree tree(pager);
//
//   // Insert
//   tree.Put("name", "Alice");
//   tree.Put("age", "30");
//
//   // Lookup
//   auto result = tree.Get("name");
//   if (result.ok()) {
//       std::cout << result.value() << std::endl;  // "Alice"
//   }

class BPlusTree {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------
    
    // Create a B+ tree on top of the given buffer pool (Phase 3). All page
    // access goes through the pool; meta operations (root id, num_keys) go
    // through pool.pager().
    //
    // Phase 4: if `wal` is given, every mutation emits physiological log
    // records (single-page kPut/kDelete; kPageImage + kAllocPage +
    // kRootChange for structural changes) and stamps the affected page's
    // page_lsn. Passing nullptr disables logging (tree-level tests).
    explicit BPlusTree(storage::BufferPool& pool,
                       wal::LogWriter* wal = nullptr);
    
    // -------------------------------------------------------------------------
    // Core Operations
    // -------------------------------------------------------------------------
    
    // Get the value for a key
    //
    // Returns:
    //   On success: The value associated with the key
    //   On not found: Status::NotFound
    //   On error: Status with error details
    [[nodiscard("Get returns the value - ignoring it wastes the lookup")]]
    Result<std::string> Get(std::string_view key);
    
    // Insert or update a key-value pair
    //
    // If key exists: value is updated
    // If key doesn't exist: key-value pair is inserted
    //
    // Returns:
    //   Status::Ok() on success
    //   Status::IoError() on disk error
    //
    // Phase 5: `txid` stamps every WAL record this op emits (recovery groups
    // by it). When `before_out` is non-null the prior value is captured - both
    // into the kPut record's before-image (for recovery undo) and back out to
    // the caller (for a transaction's in-memory undo list): it is set to the
    // old value on an update, or nullopt on a fresh insert. Autocommit callers
    // pass a fresh txid and a null before_out (single-op txns never roll back).
    [[nodiscard("Put returns status - check for errors")]]
    Status Put(std::string_view key, std::string_view value,
               TxId txid = kInvalidTxId,
               std::optional<std::string>* before_out = nullptr);
    
    // Delete a key-value pair (Phase 2, task 2.2)
    //
    // Returns:
    //   Status::Ok() if the key was found and removed
    //   Status::NotFound() if the key is absent (tree unchanged)
    //   Status with error details on I/O failure
    //
    // The leaf keeps its position in the sibling chain even if it becomes
    // empty (decision 2.8: no page freeing / underflow handling yet).
    //
    // Phase 5: `txid`/`before_out` work as in Put. On a successful delete with
    // a non-null before_out, *before_out is set to the removed value (never
    // nullopt - the key existed); it feeds both the kDelete record's undo
    // before-image and the transaction's undo list.
    [[nodiscard("Delete returns status - check for errors")]]
    Status Delete(std::string_view key, TxId txid = kInvalidTxId,
                  std::optional<std::string>* before_out = nullptr);

    // Check if a key exists
    //
    // Returns:
    //   true if key exists
    //   false if key doesn't exist
    [[nodiscard("Contains result should be used")]]
    bool Contains(std::string_view key);
    
    // -------------------------------------------------------------------------
    // Cursors / Range Scans (Phase 2, task 2.10)
    // -------------------------------------------------------------------------

    // Position a cursor on the first key >= `key` (lower bound).
    // The cursor is invalid if no such key exists; check cursor.status()
    // to distinguish exhaustion from an I/O error.
    [[nodiscard("Seek returns the positioned cursor")]]
    Cursor Seek(std::string_view key);

    // Position a cursor on the smallest key in the tree.
    [[nodiscard("SeekToFirst returns the positioned cursor")]]
    Cursor SeekToFirst();

    // -------------------------------------------------------------------------
    // Tree Information (using deducing this for const-correctness)
    // -------------------------------------------------------------------------
    
    // Get the root page ID
    [[nodiscard("root_page_id returns a value")]]
    constexpr auto root_page_id(this auto&& self) {
        return self.pool_.pager().root_page_id();
    }
    
    // Check if the tree is empty
    [[nodiscard("IsEmpty returns a boolean condition")]]
    constexpr auto IsEmpty(this auto&& self) {
        return self.pool_.pager().root_page_id() == kInvalidPageId;
    }

private:
    storage::BufferPool& pool_;
    wal::LogWriter* wal_ = nullptr;
    
    // -------------------------------------------------------------------------
    // Internal Operations
    // -------------------------------------------------------------------------
    
    // Find the leaf page that should contain a key
    // Returns the PageId of the leaf, or kInvalidPageId if tree is empty
    [[nodiscard("FindLeaf returns the target page")]]
    Result<PageId> FindLeaf(std::string_view key);

    // The splitting slow path of Put (Phase 5.2): exclusive root_latch, full
    // exclusive path descent, splits propagated through the held guards.
    // Public Put tries the optimistic shared-latch fast path first and calls
    // this only when the target leaf cannot absorb the write.
    [[nodiscard]]
    Status PutPessimistic(std::string_view key, std::string_view value,
                          TxId txid, std::optional<std::string>* before_out);
    
    // Insert into a leaf node, handling splits if necessary
    // Returns the new root page ID if root changed, otherwise kInvalidPageId
    struct InsertResult {
        bool split_occurred;
        std::string split_key;      // Key to insert into parent
        PageId new_page_id;         // New page created by split
    };
    
    [[nodiscard]]
    Result<InsertResult> InsertIntoLeaf(PageId leaf_id, 
                                         std::string_view key, 
                                         std::string_view value);
    
    // Insert into an internal node after a child split
    [[nodiscard]]
    Result<InsertResult> InsertIntoInternal(PageId internal_id,
                                             std::string_view key,
                                             PageId left_child,
                                             PageId right_child);
    
    // Split a leaf node (page held by `guard`, mutated in place).
    // Returns: (guard of the new right page, split_key); the split_key should
    // be inserted into the parent, the new page gets the upper half of keys.
    //
    // WAL op-group atomicity: the split logs page images WITHOUT op_end, so
    // every page it rewrites must stay pinned until the caller appends the
    // group's op-end record - an unpinned dirty page could be evicted (and
    // written to data.db) mid-group, while recovery drops incomplete groups
    // assuming none of their pages reached disk. The new page's guard is
    // returned; the old-next sibling's guard is parked in `op_guards`. The
    // caller keeps all of them alive through its final (op-end) Log* call.
    // `txid` stamps the structural WAL records the split emits.
    [[nodiscard("Split returns the new page's guard + split key")]]
    Result<std::pair<storage::PageGuard, std::string>> SplitLeaf(
        storage::PageGuard& guard, TxId txid,
        std::vector<storage::PageGuard>& op_guards);

    // Split an internal node. Same op-group pinning contract as SplitLeaf:
    // the new page's guard is returned and must live until op-end.
    [[nodiscard("Split returns the new page's guard + split key")]]
    Result<std::pair<storage::PageGuard, std::string>> SplitInternal(
        storage::PageGuard& guard, TxId txid);

    // Create a new root after the old root splits
    [[nodiscard("CreateNewRoot returns status")]]
    Status CreateNewRoot(std::string_view split_key,
                         PageId left_child,
                         PageId right_child,
                         TxId txid);

    // (removed) struct PathEntry / FindLeafPath - Put now crabs with a held
    // guard stack instead of a re-fetched path.

    // -------------------------------------------------------------------------
    // WAL emission (Phase 4) - no-ops when wal_ is null
    // -------------------------------------------------------------------------

    // Append a single-page logical record (kPut/kDelete) for the page held
    // by `guard`, stamp its page_lsn with the record's LSN, and mark dirty.
    // `op_end` marks the record that completes the logical operation
    // (recovery applies op groups all-or-nothing). `txid` groups the record;
    // `before` is the undo before-image (nullopt for a fresh insert; ignored
    // for kDelete's own record shape but carried for recovery undo).
    // `needs_undo` (transactional ops): guarantee a delta record carrying the
    // before-image so recovery can undo a loser, even on a page's first touch.
    // A first-touch image then becomes redo-only (op_end moves to the delta).
    // Autocommit ops (needs_undo=false) keep the lean image-or-delta shape.
    [[nodiscard]]
    Status LogKeyRecord(wal::LogRecordType type, TxId txid,
                        storage::PageGuard& guard, std::string_view key,
                        std::string_view value,
                        const std::optional<std::string>& before, bool op_end,
                        bool needs_undo);

    // Append a full after-image of the page held by `guard` (structural
    // changes: split halves, sibling relinks, parent rebuilds, new roots).
    // The image embeds the record's own LSN as page_lsn. `txid` groups it.
    [[nodiscard]]
    Status LogPageImage(storage::PageGuard& guard, TxId txid,
                        bool op_end = false);

    // Append a payload-free structural record (kAllocPage / kRootChange).
    [[nodiscard]]
    Status LogPageEvent(wal::LogRecordType type, TxId txid, PageId page_id,
                        bool op_end = false);
};

}  // namespace kvstore::btree
