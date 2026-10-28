// ==============================================================================
// kvstore/storage/buffer_pool.cc - Buffer Pool Implementation
// ==============================================================================
//
// Phase 5 concurrency notes:
//   - latch_ guards the page table, free list, replacer, pin counts, and
//     stats. It is held across eviction I/O (coarse but simple - Phase 6
//     can shard it if benchmarks care).
//   - A frame's content latch is only ever BLOCKING-acquired after the
//     frame is pinned and latch_ released; eviction try_locks the latch of
//     pin==0 victims, which cannot be contended (holders keep pins).
//   - FlushAll pins its snapshot first, then latches each frame shared, so
//     concurrent writers can't be captured mid-mutation and frames can't
//     be evicted under the flush.
//
// ==============================================================================

#include "kvstore/storage/buffer_pool.h"

#include <cassert>
#include <cstring>

#include "kvstore/wal/log_writer.h"

namespace kvstore::storage {

// ==============================================================================
// PageGuard members that need the pool definition
// ==============================================================================

void PageGuard::MarkDirty() {
    if (pool_ != nullptr) [[likely]] {
        pool_->frames_[frame_].dirty.store(true, std::memory_order_release);
    }
}

void PageGuard::Drop() {
    if (pool_ != nullptr) {
        pool_->ReleaseGuard(frame_, mode_);
        pool_ = nullptr;
        page_ = nullptr;
        page_id_ = kInvalidPageId;
        mode_ = LatchMode::kNone;
    }
}

// ==============================================================================
// Construction
// ==============================================================================

BufferPool::BufferPool(Pager& pager, size_t num_frames, wal::LogWriter* wal)
    : pager_(pager), wal_(wal) {
    assert(num_frames >= kMinFrames && "pool too small for the split path");
    num_frames_ = num_frames < kMinFrames ? kMinFrames : num_frames;
    frames_ = std::make_unique<Frame[]>(num_frames_);
    free_frames_.reserve(num_frames_);
    // Hand out low frame indices first (cosmetic, aids debugging).
    for (FrameId i = static_cast<FrameId>(num_frames_); i-- > 0;) {
        free_frames_.push_back(i);
    }
}

// ==============================================================================
// Frame acquisition (free list, then LRU eviction) - latch_ held by caller
// ==============================================================================

Result<FrameId> BufferPool::AcquireFrameLocked() {
    if (!free_frames_.empty()) {
        FrameId frame_id = free_frames_.back();
        free_frames_.pop_back();
        return frame_id;
    }

    auto victim = replacer_.Evict();
    if (!victim) [[unlikely]] {
        return std::unexpected(
            Status::Busy("All buffer pool frames are pinned"));
    }

    Frame& frame = frames_[*victim];
    assert(frame.pin_count == 0 && "evicting a pinned frame");

    if (frame.dirty.load(std::memory_order_acquire)) {
        // Restores the victim to the replacer on any failure below; losing
        // the frame would leak it and discarding the dirty page loses data.
        auto put_back = [&](const Status& s) {
            replacer_.RecordAccess(*victim);
            replacer_.SetEvictable(*victim, true);
            return std::unexpected(s);
        };
        // pin_count == 0 means no guard exists, so the content latch cannot
        // be held; try_lock documents (and asserts) that assumption without
        // risking a lock-order inversion against latch_.
        if (!frame.content_latch.try_lock()) [[unlikely]] {
            assert(false && "content latch held on a pin-0 frame");
            return put_back(Status::Internal("Latched frame with zero pins"));
        }
        std::lock_guard<std::shared_mutex> content(frame.content_latch,
                                                   std::adopt_lock);
        // Write-ahead rule: the log must cover this page before it lands.
        if (Status s = EnforceWalRule(frame); !s.ok()) [[unlikely]] {
            return put_back(s);
        }
        if (Status s = pager_.WritePage(frame.page_id, frame.page); !s.ok()) {
            return put_back(s);
        }
        frame.dirty.store(false, std::memory_order_release);
    }

    page_table_.erase(frame.page_id);
    frame.page_id = kInvalidPageId;
    ++stats_.evictions;
    return *victim;
}

// ==============================================================================
// FetchPage variants / NewPage
// ==============================================================================

Result<PageGuard> BufferPool::FetchPageInternal(PageId page_id, LatchMode mode) {
    Frame* frame = nullptr;
    FrameId frame_id = 0;
    {
        std::lock_guard<std::mutex> pool_lock(latch_);
        if (auto it = page_table_.find(page_id); it != page_table_.end()) {
            frame_id = it->second;
            frame = &frames_[frame_id];
            ++frame->pin_count;
            replacer_.RecordAccess(frame_id);
            replacer_.SetEvictable(frame_id, false);
            ++stats_.hits;
        } else {
            ++stats_.misses;
            auto acquired = AcquireFrameLocked();
            if (!acquired) [[unlikely]] {
                return std::unexpected(acquired.error());
            }
            frame_id = *acquired;
            frame = &frames_[frame_id];
            if (Status s = pager_.ReadPage(page_id, frame->page);
                !s.ok()) [[unlikely]] {
                free_frames_.push_back(frame_id);  // Frame stays clean/unused
                return std::unexpected(s);
            }
            frame->page_id = page_id;
            frame->pin_count = 1;
            frame->dirty.store(false, std::memory_order_release);
            page_table_.emplace(page_id, frame_id);
            replacer_.RecordAccess(frame_id);
            replacer_.SetEvictable(frame_id, false);
        }
    }

    // Content latch OUTSIDE latch_: it may block on a concurrent holder,
    // and the pin already protects the frame from eviction.
    switch (mode) {
        case LatchMode::kShared:    frame->content_latch.lock_shared(); break;
        case LatchMode::kExclusive: frame->content_latch.lock(); break;
        case LatchMode::kNone:      break;
    }
    return PageGuard(this, frame_id, &frame->page, page_id, mode);
}

Result<PageGuard> BufferPool::FetchPage(PageId page_id) {
    return FetchPageInternal(page_id, LatchMode::kNone);
}

Result<PageGuard> BufferPool::FetchPageRead(PageId page_id) {
    return FetchPageInternal(page_id, LatchMode::kShared);
}

Result<PageGuard> BufferPool::FetchPageWrite(PageId page_id) {
    return FetchPageInternal(page_id, LatchMode::kExclusive);
}

Result<PageGuard> BufferPool::NewPage() {
    Frame* frame = nullptr;
    FrameId frame_id = 0;
    PageId page_id = kInvalidPageId;
    {
        std::lock_guard<std::mutex> pool_lock(latch_);
        // Reserve the frame first: if the pool is exhausted we must fail
        // before the file grows.
        auto acquired = AcquireFrameLocked();
        if (!acquired) [[unlikely]] {
            return std::unexpected(acquired.error());
        }
        frame_id = *acquired;
        frame = &frames_[frame_id];

        auto allocated = pager_.AllocatePage();
        if (!allocated) [[unlikely]] {
            free_frames_.push_back(frame_id);
            return std::unexpected(allocated.error());
        }
        page_id = *allocated;

        std::memset(frame->page.RawData(), 0, kPageSize);  // On-disk state
        frame->page_id = page_id;
        frame->pin_count = 1;
        frame->dirty.store(false, std::memory_order_release);
        page_table_.emplace(page_id, frame_id);
        replacer_.RecordAccess(frame_id);
        replacer_.SetEvictable(frame_id, false);
    }

    // Fresh page: no other thread can know its id yet, but take the latch
    // through the normal path for uniform guard semantics.
    frame->content_latch.lock();
    return PageGuard(this, frame_id, &frame->page, page_id,
                     LatchMode::kExclusive);
}

// ==============================================================================
// Guard release
// ==============================================================================

void BufferPool::ReleaseGuard(FrameId frame_id, LatchMode mode) {
    Frame& frame = frames_[frame_id];
    // Latch first (eviction try_locks the latch of pin-0 frames), then pin.
    switch (mode) {
        case LatchMode::kShared:    frame.content_latch.unlock_shared(); break;
        case LatchMode::kExclusive: frame.content_latch.unlock(); break;
        case LatchMode::kNone:      break;
    }
    std::lock_guard<std::mutex> pool_lock(latch_);
    assert(frame.pin_count > 0 && "unpinning an unpinned frame");
    if (--frame.pin_count == 0) {
        replacer_.SetEvictable(frame_id, true);
    }
}

// ==============================================================================
// Flushing
// ==============================================================================

Status BufferPool::FlushPage(PageId page_id) {
    Frame* frame = nullptr;
    FrameId frame_id = 0;
    {
        std::lock_guard<std::mutex> pool_lock(latch_);
        auto it = page_table_.find(page_id);
        if (it == page_table_.end()) {
            return Status::Ok();  // Not resident: nothing buffered to flush
        }
        frame_id = it->second;
        frame = &frames_[frame_id];
        ++frame->pin_count;  // Keep the frame ours while we work unlatched
        replacer_.SetEvictable(frame_id, false);
    }

    Status result = Status::Ok();
    if (frame->dirty.load(std::memory_order_acquire)) {
        std::shared_lock<std::shared_mutex> content(frame->content_latch);
        result = EnforceWalRule(*frame);
        if (result.ok()) {
            result = pager_.WritePage(frame->page_id, frame->page);
        }
        if (result.ok()) {
            frame->dirty.store(false, std::memory_order_release);
        }
    }
    ReleaseGuard(frame_id, LatchMode::kNone);
    return result;
}

Status BufferPool::FlushAll() {
    // Snapshot + pin the resident set, then flush each frame under its
    // shared latch without holding latch_ (avoids latch-order inversion
    // with writers who hold content latches while fetching other pages).
    std::vector<FrameId> pinned;
    {
        std::lock_guard<std::mutex> pool_lock(latch_);
        pinned.reserve(page_table_.size());
        for (auto& [page_id, frame_id] : page_table_) {
            ++frames_[frame_id].pin_count;
            replacer_.SetEvictable(frame_id, false);
            pinned.push_back(frame_id);
        }
    }

    Status result = Status::Ok();
    for (FrameId frame_id : pinned) {
        Frame& frame = frames_[frame_id];
        if (result.ok() && frame.dirty.load(std::memory_order_acquire)) {
            std::shared_lock<std::shared_mutex> content(frame.content_latch);
            result = EnforceWalRule(frame);
            if (result.ok()) {
                result = pager_.WritePage(frame.page_id, frame.page);
            }
            if (result.ok()) {
                frame.dirty.store(false, std::memory_order_release);
            }
        }
        ReleaseGuard(frame_id, LatchMode::kNone);
    }
    return result;
}

// ==============================================================================
// Write-ahead rule (Phase 4, task 4.5)
// ==============================================================================

Status BufferPool::EnforceWalRule(const Frame& frame) {
    if (wal_ == nullptr) {
        return Status::Ok();  // No WAL attached (tests / pre-WAL callers)
    }
    if (frame.page.page_lsn() == kInvalidLsn) {
        return Status::Ok();  // Page was never part of a logged mutation
    }
    // Flush EVERYTHING appended so far, not just through this page's
    // page_lsn. Recovery applies record groups all-or-nothing (a group whose
    // op-end record is not durable is dropped), so the durable log must
    // contain the COMPLETE group of this page's last mutation before the
    // page reaches data.db. The tree keeps every page of an in-flight op
    // pinned until the group's op-end record is appended, so by the time a
    // frame is evictable its group is fully buffered - and a whole-buffer
    // flush is what makes it durable. A page_lsn-bounded flush is not
    // enough: a concurrent commit can flush the log mid-group, covering
    // this page's LSN but not the later op-end, turning the bounded flush
    // into a no-op (regression: WalRuleTest.
    // DirtyEvictionFlushesTheWholeAppendedLog).
    return wal_->Flush();
}

// ==============================================================================
// Introspection
// ==============================================================================

BufferPool::Stats BufferPool::stats() const {
    std::lock_guard<std::mutex> pool_lock(latch_);
    return stats_;
}

uint32_t BufferPool::PinCount(PageId page_id) const {
    std::lock_guard<std::mutex> pool_lock(latch_);
    auto it = page_table_.find(page_id);
    return it == page_table_.end() ? 0 : frames_[it->second].pin_count;
}

bool BufferPool::IsResident(PageId page_id) const {
    std::lock_guard<std::mutex> pool_lock(latch_);
    return page_table_.contains(page_id);
}

}  // namespace kvstore::storage
