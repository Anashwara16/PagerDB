// ==============================================================================
// kvstore/storage/buffer_pool.h - Page Cache Between B+ Tree and Pager
// ==============================================================================
//
// Phase 3: fixed number of page-sized frames; the tree fetches pages through
// RAII PageGuards; mutations are written back lazily.
// Phase 4: enforces the write-ahead rule against an attached LogWriter.
// Phase 5: thread-safe. One internal mutex guards the page table, free
// list, replacer, pins, and stats; each frame carries a shared_mutex for
// its CONTENT, acquired via FetchPageRead (shared) / FetchPageWrite
// (exclusive). Content latches are never acquired while the pool mutex is
// held EXCEPT via try_lock on pin==0 eviction victims (which cannot be
// contended, since latch holders always hold pins).
//
// Lock ordering (deadlock freedom): pool latch_ is a leaf lock below all
// page content latches; the WAL writer's internal mutex sits below the pool
// latch (the pool calls the WAL, never the reverse).
//
// Invariants:
//   - A frame's contents are replaced only when its pin_count is 0.
//   - A dirty frame is never discarded without being written back, and
//     never written while a writer holds its exclusive latch.
//   - page_table_ and resident frames are bijective.
//
// Minimum pool size: a splitting Put pins its whole root-to-leaf path plus
// every page the op rewrites (split halves, relinked sibling) until the
// op's final WAL record is appended - the pins are what keep an eviction
// from writing half a WAL op group to data.db (see EnforceWalRule). Worst
// case ~2*depth+2 frames; DbImpl clamps Options::buffer_pool_frames to 16
// (depth 7). With concurrency (Phase 5) each thread can pin a path, so
// size pools generously. kMinFrames is only this class's structural floor
// (pool-only tests); when every frame is pinned, FetchPage*/NewPage return
// Status::Busy.
//
// ==============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "kvstore/status.h"
#include "kvstore/types.h"
#include "kvstore/storage/lru_replacer.h"
#include "kvstore/storage/page_format.h"
#include "kvstore/storage/page_guard.h"
#include "kvstore/storage/pager.h"

namespace kvstore::wal {
class LogWriter;  // Phase 4: write-ahead rule (see BufferPool constructor)
}

namespace kvstore::storage {

class BufferPool {
public:
    // Cache effectiveness counters (exposed as DB::GetStats() in Phase 6).
    struct Stats {
        uint64_t hits = 0;        // FetchPage found the page resident
        uint64_t misses = 0;      // FetchPage had to read from disk
        uint64_t evictions = 0;   // A frame was repurposed for another page
    };

    // Structural floor for pool-only use (see the header comment on minimum
    // pool size: tree writers need more - DbImpl clamps to 16).
    static constexpr size_t kMinFrames = 4;

    // The pool references the pager; both must outlive the pool's guards.
    // If `wal` is given, the write-ahead rule is enforced: before any frame
    // reaches the data file (eviction, FlushPage, FlushAll), the WHOLE
    // appended log is flushed (see EnforceWalRule for why it is not bounded
    // by the frame's page_lsn). Passing nullptr (tests, pre-WAL callers)
    // disables the rule.
    explicit BufferPool(Pager& pager, size_t num_frames,
                        wal::LogWriter* wal = nullptr);

    // Frames are referenced by live guards - the pool must not move.
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    BufferPool(BufferPool&&) = delete;
    BufferPool& operator=(BufferPool&&) = delete;

    // Pin `page_id` (reading from disk on a miss) WITHOUT a content latch.
    // Only safe for single-threaded access patterns (recovery, old tests).
    [[nodiscard("FetchPage returns the guard - dropping it releases the pin")]]
    Result<PageGuard> FetchPage(PageId page_id);

    // Pin + shared content latch: concurrent readers, excludes writers.
    [[nodiscard("FetchPageRead returns the read-latched guard")]]
    Result<PageGuard> FetchPageRead(PageId page_id);

    // Pin + exclusive content latch: sole access to the page bytes.
    [[nodiscard("FetchPageWrite returns the write-latched guard")]]
    Result<PageGuard> FetchPageWrite(PageId page_id);

    // Allocate a fresh page via the Pager and pin it, zeroed, into a frame.
    // The returned guard holds the exclusive latch.
    [[nodiscard("NewPage returns the guard for the new page")]]
    Result<PageGuard> NewPage();

    // Write one resident dirty page back to disk (no-op if clean or absent).
    [[nodiscard("FlushPage returns status - check for I/O errors")]]
    Status FlushPage(PageId page_id);

    // Write every resident dirty page back to disk. Frames stay resident.
    // Takes each frame's shared latch during the copy-out, so pages are
    // captured mutation-consistent even with concurrent writers.
    [[nodiscard("FlushAll returns status - check for I/O errors")]]
    Status FlushAll();

    // The underlying pager, for meta operations (root id, num_keys, Sync).
    [[nodiscard("pager returns the underlying Pager")]]
    Pager& pager() noexcept { return pager_; }

    // Phase 5.2: the outermost tree lock, guarding pager().root_page_id().
    // Writers hold it EXCLUSIVE across a potential root change (first insert /
    // root split); readers and deletes hold it SHARED only long enough to read
    // the root id and latch the root node. Acquired before any page content
    // latch (crabbing order: root_latch -> tree path top-down).
    [[nodiscard("root_latch guards the root page id")]]
    std::shared_mutex& root_latch() noexcept { return root_latch_; }

    [[nodiscard("stats returns cache counters")]]
    Stats stats() const;

    [[nodiscard("num_frames returns the pool capacity")]]
    size_t num_frames() const noexcept { return num_frames_; }

    // Test/introspection helpers.
    [[nodiscard("PinCount returns the page's pin count (0 if absent)")]]
    uint32_t PinCount(PageId page_id) const;

    [[nodiscard("IsResident returns whether the page occupies a frame")]]
    bool IsResident(PageId page_id) const;

private:
    friend class PageGuard;  // MarkDirty / Drop touch frame state

    struct Frame {
        Page page;
        PageId page_id = kInvalidPageId;
        uint32_t pin_count = 0;             // Guarded by latch_
        std::atomic<bool> dirty{false};     // Set by guards, cleared by flush
        std::shared_mutex content_latch;    // Page-byte access (Phase 5)
    };

    // Shared fetch path; `mode` picks the content latch taken after the
    // frame is pinned (never while latch_ is held).
    [[nodiscard]] Result<PageGuard> FetchPageInternal(PageId page_id,
                                                      LatchMode mode);

    // Take a frame from the free list, or evict the LRU unpinned frame
    // (writing it back first if dirty). Busy if everything is pinned.
    // Caller must hold latch_.
    [[nodiscard]] Result<FrameId> AcquireFrameLocked();

    // The write-ahead rule (Phase 4, task 4.5): no page with
    // page_lsn > flushed WAL prefix may reach the data file.
    [[nodiscard]] Status EnforceWalRule(const Frame& frame);

    // Called by PageGuard::Drop: release `mode` latch, then unpin.
    void ReleaseGuard(FrameId frame_id, LatchMode mode);

    Pager& pager_;
    wal::LogWriter* wal_ = nullptr;

    mutable std::mutex latch_;  // Page table, free list, replacer, pins, stats
    std::shared_mutex root_latch_;  // Phase 5.2: guards pager root_page_id
    std::unique_ptr<Frame[]> frames_;
    size_t num_frames_ = 0;
    std::vector<FrameId> free_frames_;
    std::unordered_map<PageId, FrameId> page_table_;
    LruReplacer replacer_;
    Stats stats_;
};

}  // namespace kvstore::storage
