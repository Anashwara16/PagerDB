// ==============================================================================
// kvstore/storage/page_guard.h - RAII Pin (+ Latch) on a Buffer Pool Frame
// ==============================================================================
//
// A PageGuard represents one pin on one buffer pool frame, and - since
// Phase 5 - optionally a content latch on that frame:
//
//   FetchPageRead   → pin + shared latch    (concurrent readers)
//   FetchPageWrite  → pin + exclusive latch (sole mutator)
//   NewPage         → pin + exclusive latch (fresh page)
//   FetchPage       → pin only, NO latch    (single-threaded callers, e.g.
//                                            recovery and old tests)
//
// While any guard on a page is alive, the pool will not evict or reuse the
// frame. The destructor releases the latch (if held) and the pin.
//
// Move-only: copying would double-count the pin and the latch.
//
// ==============================================================================

#pragma once

#include <utility>

#include "kvstore/types.h"

namespace kvstore::storage {

class BufferPool;
class Page;

// Which content latch a guard holds on its frame.
enum class LatchMode : uint8_t {
    kNone,       // Pin only (single-threaded access patterns)
    kShared,     // Reader: excludes writers, shares with readers
    kExclusive,  // Writer: excludes everyone
};

class PageGuard {
public:
    // Invalid guard (no pin). valid() is false.
    PageGuard() = default;

    PageGuard(PageGuard&& other) noexcept { *this = std::move(other); }

    PageGuard& operator=(PageGuard&& other) noexcept {
        if (this != &other) {
            Drop();  // Release our current pin/latch, if any
            pool_ = std::exchange(other.pool_, nullptr);
            page_ = std::exchange(other.page_, nullptr);
            frame_ = other.frame_;
            page_id_ = std::exchange(other.page_id_, kInvalidPageId);
            mode_ = std::exchange(other.mode_, LatchMode::kNone);
        }
        return *this;
    }

    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;

    ~PageGuard() { Drop(); }

    // True while this guard holds a pin.
    [[nodiscard("valid gates every other accessor")]]
    bool valid() const noexcept { return pool_ != nullptr; }

    // The pinned page. Only call while valid(). Mutating through a guard
    // that does not hold the exclusive latch is a race under concurrency.
    [[nodiscard("page returns the pinned frame's page")]]
    Page& page() const noexcept { return *page_; }

    [[nodiscard("page_id identifies the pinned page")]]
    PageId page_id() const noexcept { return page_id_; }

    [[nodiscard("mode reports the held content latch")]]
    LatchMode mode() const noexcept { return mode_; }

    // Record that the page was mutated; the pool writes it back on eviction
    // or FlushPage/FlushAll. Replaces Pager::WritePage on the hot path.
    void MarkDirty();

    // Release the latch and pin early (idempotent; the destructor calls
    // this too).
    void Drop();

private:
    friend class BufferPool;  // Only the pool creates live guards

    PageGuard(BufferPool* pool, FrameId frame, Page* page, PageId page_id,
              LatchMode mode) noexcept
        : pool_(pool), page_(page), frame_(frame), page_id_(page_id),
          mode_(mode) {}

    BufferPool* pool_ = nullptr;
    Page* page_ = nullptr;
    FrameId frame_ = 0;
    PageId page_id_ = kInvalidPageId;
    LatchMode mode_ = LatchMode::kNone;
};

}  // namespace kvstore::storage
