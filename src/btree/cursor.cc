// ==============================================================================
// kvstore/btree/cursor.cc - Leaf-Chain Cursor Implementation (C++23)
// ==============================================================================
//
// The cursor walks the leaf sibling chain (next_page links). All positioning
// logic funnels through SkipToLiveEntry, which owns the two invariants:
//   - a valid cursor always points at an existing entry (index_ < NumKeys())
//   - empty leaves in the chain are never observable from the outside
//
// ==============================================================================

#include "kvstore/btree/cursor.h"

#include <string>

#include "kvstore/btree/node.h"

namespace kvstore::btree {

void Cursor::MoveTo(PageId page_id, uint16_t index) {
    auto guard = pool_->FetchPageRead(page_id);
    if (!guard) [[unlikely]] {
        valid_ = false;
        status_ = guard.error();
        return;
    }
    page_ = guard->page();  // Copy out; the pin is released immediately
    index_ = index;
    valid_ = true;
    SkipToLiveEntry();
}

void Cursor::SkipToLiveEntry() {
    for (;;) {
        NodeView node(page_);
        if (index_ < node.NumKeys()) {
            return;  // Positioned on a live entry
        }
        // Past the end of this leaf (or the leaf is empty): hop forward.
        PageId next = node.GetNextLeaf();
        if (next == kInvalidPageId) {
            valid_ = false;  // Clean end of scan; status_ stays Ok
            return;
        }
        auto guard = pool_->FetchPageRead(next);
        if (!guard) [[unlikely]] {
            valid_ = false;
            status_ = guard.error();
            return;
        }
        page_ = guard->page();
        // A chain link pointing at a non-leaf page means on-disk damage
        // (e.g. a half-applied split); fail loudly instead of walking junk.
        if (NodeView(page_).IsLeaf() == false) [[unlikely]] {
            valid_ = false;
            status_ = Status::Corruption(
                "Leaf chain links to non-leaf page " + std::to_string(next));
            return;
        }
        index_ = 0;
    }
}

void Cursor::Next() {
    if (!valid_) [[unlikely]] {
        return;
    }
    ++index_;
    SkipToLiveEntry();
}

std::string_view Cursor::key() const {
    if (!valid_) [[unlikely]] {
        return {};
    }
    // NodeView needs a mutable Page& but GetKey only reads; the cast keeps
    // key()/value() const for callers.
    NodeView node(const_cast<storage::Page&>(page_));
    return node.GetKey(index_);
}

std::string_view Cursor::value() const {
    if (!valid_) [[unlikely]] {
        return {};
    }
    NodeView node(const_cast<storage::Page&>(page_));
    return node.GetValueAt(index_);
}

}  // namespace kvstore::btree
