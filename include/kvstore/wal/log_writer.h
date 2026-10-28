// ==============================================================================
// kvstore/wal/log_writer.h - Buffered Append-Only WAL Writer (Phase 4)
// ==============================================================================
//
// Appends records to wal.log with byte-offset LSNs: a record's LSN is
// base_lsn + its byte offset in the (logical) log, so LSNs stay globally
// monotonic across checkpoint truncations - after a truncation the new base
// is the old end.
//
// Appends are buffered in memory; FlushTo(lsn) writes and fsyncs when the
// requested LSN is not yet durable. The buffer pool calls Flush() - the
// WHOLE appended log - before writing any frame (the write-ahead rule;
// bounding the flush by the frame's page_lsn would let a page reach disk
// while its op group's later op-end record is still buffered, see
// BufferPool::EnforceWalRule); DbImpl also calls Flush() on commit when
// sync_on_write is set.
//
// Phase 5: thread-safe. One internal mutex serializes every append, flush,
// truncation, image-tracking update, and accessor, so concurrent transaction
// writers share a single log. The mutex is a leaf lock: the pool holds a
// frame content latch when it calls FlushTo, and the tree holds page latches
// when it calls Append/AppendPageImage - the writer never reaches back for a
// page or pool latch, so no inversion is possible. LSN assignment and
// page_lsn stamping happen together under the lock (see AppendPageImage), so
// a concurrently-appending thread can never steal the LSN an image embeds.
//
// ==============================================================================

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

#include "kvstore/status.h"
#include "kvstore/types.h"
#include "kvstore/storage/file.h"
#include "kvstore/wal/log_record.h"

namespace kvstore::storage {
class Page;  // AppendPageImage serializes a full page under the writer's lock
}

namespace kvstore::wal {

class LogWriter {
public:
    // Open (creating if needed) the log at `path`. `base_lsn` is the LSN of
    // the file's first byte: meta.checkpoint_lsn for an empty log, or the
    // first record's stored LSN when reopening a non-empty one (recovery
    // derives it). Existing bytes are treated as already flushed.
    [[nodiscard("Open returns the writer")]]
    static Result<LogWriter> Open(const std::string& path, Lsn base_lsn);

    LogWriter(LogWriter&&) noexcept = default;
    LogWriter& operator=(LogWriter&&) noexcept = default;
    LogWriter(const LogWriter&) = delete;
    LogWriter& operator=(const LogWriter&) = delete;

    // Assign the next LSN to `record`, buffer its bytes, and return the LSN.
    // Durability requires a later FlushTo/Flush. Thread-safe.
    [[nodiscard("Append returns the record's LSN")]]
    Result<Lsn> Append(LogRecord& record);

    // Atomically (under the writer's lock): assign the next LSN, stamp it into
    // `page`'s header as page_lsn, serialize the full page as a kPageImage
    // record, append it, and mark the page imaged for this checkpoint epoch.
    // The caller must hold `page`'s exclusive content latch. Returns the LSN.
    //
    // This exists because an image embeds its own LSN (page_lsn == record.lsn);
    // peeking the next LSN and appending in two steps would race with a
    // concurrent appender.
    [[nodiscard("AppendPageImage returns the image record's LSN")]]
    Result<Lsn> AppendPageImage(PageId page_id, storage::Page& page,
                                TxId txid, bool op_end);

    // Make everything appended so far durable (write + fsync). Thread-safe.
    [[nodiscard("Flush returns status - check for I/O errors")]]
    Status Flush();

    // Make at least everything up to and including `lsn` durable.
    // No-op (and no fsync) if already flushed past it. Thread-safe.
    [[nodiscard("FlushTo returns status - check for I/O errors")]]
    Status FlushTo(Lsn lsn);

    // Drop the entire log and restart at `new_base` (checkpoint epilogue:
    // everything before new_base is now reflected in flushed data pages).
    [[nodiscard("Truncate returns status - check for I/O errors")]]
    Status Truncate(Lsn new_base);

    [[nodiscard("base_lsn returns the LSN of the file's first byte")]]
    Lsn base_lsn() const {
        std::lock_guard<std::mutex> lg(*mu_);
        return base_lsn_;
    }

    [[nodiscard("end_lsn returns the LSN one past the last record")]]
    Lsn end_lsn() const {
        std::lock_guard<std::mutex> lg(*mu_);
        return end_lsn_;
    }

    [[nodiscard("flushed_lsn returns the durable prefix bound")]]
    Lsn flushed_lsn() const {
        std::lock_guard<std::mutex> lg(*mu_);
        return flushed_lsn_;
    }

    // Log growth since the last truncation (drives Options::max_wal_bytes).
    [[nodiscard("SizeBytes returns the current log size")]]
    uint64_t SizeBytes() const {
        std::lock_guard<std::mutex> lg(*mu_);
        return end_lsn_ - base_lsn_;
    }

    // Full-page-write tracking (torn-page protection): the FIRST mutation of
    // a page after a checkpoint must log a full after-image, because a torn
    // data-page write destroys the base state that delta records would need.
    // Truncate() (the checkpoint epilogue) resets the tracking. Thread-safe;
    // a page is only ever imaged/mutated under its exclusive content latch, so
    // the check and the subsequent AppendPageImage for a given page cannot
    // race with each other.
    [[nodiscard("NeedsImage gates delta vs full-image logging")]]
    bool NeedsImage(PageId page_id) const {
        std::lock_guard<std::mutex> lg(*mu_);
        return !imaged_since_truncate_.contains(page_id);
    }

private:
    LogWriter(storage::PosixFile file, Lsn base_lsn, uint64_t file_bytes)
        : file_(std::move(file)),
          base_lsn_(base_lsn),
          end_lsn_(base_lsn + file_bytes),
          flushed_lsn_(base_lsn + file_bytes),
          file_bytes_(file_bytes),
          mu_(std::make_unique<std::mutex>()) {}

    // Serialize + buffer a record that already has its lsn assigned; advances
    // end_lsn_. Caller holds mu_.
    void BufferRecordLocked(const LogRecord& record);

    // Write the buffer and fsync. Caller holds mu_.
    [[nodiscard]] Status FlushLocked();

    storage::PosixFile file_;
    std::string buffer_;      // Appended but not yet written bytes
    Lsn base_lsn_ = kFirstLsn;
    Lsn end_lsn_ = kFirstLsn;
    Lsn flushed_lsn_ = kFirstLsn;
    uint64_t file_bytes_ = 0;  // Bytes already written to the file
    std::unordered_set<PageId> imaged_since_truncate_;
    // Held for every mutation and accessor; unique_ptr keeps LogWriter movable
    // (std::mutex is not). Never null on an Open()ed writer.
    std::unique_ptr<std::mutex> mu_;
};

}  // namespace kvstore::wal
