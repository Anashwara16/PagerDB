// ==============================================================================
// kvstore/btree/cursor.h - Leaf-Chain Cursor for Range Scans (C++23)
// ==============================================================================
//
// A Cursor is a forward iterator over the key-value pairs of the B+ tree in
// sorted key order. It holds a private copy of the current leaf page and an
// index into it, and follows the next_page sibling links at leaf boundaries.
// Pages are read through the buffer pool (Phase 3) so scans observe dirty,
// not-yet-flushed frames; the copy is taken while the guard is held, so the
// cursor itself never keeps a frame pinned between calls.
// Leaves emptied by deletes stay in the chain (decision 2.8) and are skipped
// transparently.
//
// Usage:
//   for (Cursor c = tree.SeekToFirst(); c.Valid(); c.Next()) {
//       use(c.key(), c.value());
//   }
//   if (!c.status().ok()) { /* the scan stopped on an I/O error */ }
//
// Lifetime rules:
//   - key() and value() are views into the cursor's page buffer; they are
//     invalidated by Next(). Copy them out to keep them.
//   - The cursor reads pages as it goes; mutating the tree while a cursor is
//     open gives unspecified (but memory-safe) results. Snapshot semantics
//     arrive with transactions in a later phase.
//
// ==============================================================================

#pragma once

#include <string_view>

#include "kvstore/status.h"
#include "kvstore/types.h"
#include "kvstore/storage/page_format.h"
#include "kvstore/storage/buffer_pool.h"

namespace kvstore::btree {

class Cursor {
public:
    // Default-constructed cursors are invalid and OK-status (empty range).
    Cursor() = default;

    // A cursor is exhausted when Valid() is false. Distinguish "end of data"
    // from "scan failed" via status().
    [[nodiscard("Valid gates every key()/value()/Next() call")]]
    bool Valid() const noexcept { return valid_; }

    // Ok while positioned or cleanly exhausted; an error if a page read
    // failed mid-scan (the cursor becomes invalid at that point).
    [[nodiscard("status distinguishes end-of-scan from I/O failure")]]
    const Status& status() const noexcept { return status_; }

    // Advance to the next key in sorted order, following the leaf chain and
    // skipping empty leaves. No-op on an invalid cursor.
    void Next();

    // Current entry. Views into the cursor's page buffer - invalidated by
    // Next(). Only call while Valid().
    [[nodiscard("key returns the current key")]]
    std::string_view key() const;

    [[nodiscard("value returns the current value")]]
    std::string_view value() const;

private:
    friend class BPlusTree;  // Only Seek/SeekToFirst create live cursors

    explicit Cursor(storage::BufferPool& pool) noexcept : pool_(&pool) {}

    // Load `page_id` and position at `index`, then advance across the leaf
    // chain until a live entry is found (or the chain ends / a read fails).
    void MoveTo(PageId page_id, uint16_t index);

    // Forward from the current position to the first index that is inside
    // its leaf, hopping to next leaves as needed.
    void SkipToLiveEntry();

    storage::BufferPool* pool_ = nullptr;
    storage::Page page_;      // Private copy of the current leaf
    uint16_t index_ = 0;      // Slot index within page_
    bool valid_ = false;
    Status status_ = Status::Ok();
};

}  // namespace kvstore::btree
