// ==============================================================================
// kvstore/db.cc - Database Implementation (C++23)
// ==============================================================================
//
// This file implements the DB interface using:
//   - Pager for page-level I/O
//   - BPlusTree for key-value storage
//
// File Layout:
// ------------
//   <path>/data.db   - Pages (dual meta + B+ tree), see docs/FORMAT.md
//   <path>/wal.log   - Write-ahead log (Phase 4)
//   <path>/LOCK      - flock()ed for the process's lifetime: at most one
//                      process opens a database directory at a time (two
//                      would recover the same WAL, checkpoint concurrently,
//                      and truncate each other's log). Kernel-released on
//                      crash - never stale.
//
// C++23 Features Used:
// --------------------
//   - std::expected (via Result type pattern)
//   - [[likely]] / [[unlikely]] attributes
//   - Improved designated initializers
//   - std::unreachable() for impossible code paths
//
// ==============================================================================

#include "kvstore/db.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kvstore/storage/buffer_pool.h"
#include "kvstore/storage/file.h"
#include "kvstore/storage/pager.h"
#include "kvstore/btree/btree.h"
#include "kvstore/btree/node.h"
#include "kvstore/txn/lock_manager.h"
#include "kvstore/txn/txn_manager.h"
#include "kvstore/wal/log_reader.h"
#include "kvstore/wal/log_record.h"
#include "kvstore/wal/log_writer.h"

namespace kvstore {

class TxnImpl;  // Explicit-transaction handle; defined after DbImpl

// ==============================================================================
// DbImpl Class
// ==============================================================================
//
// The concrete implementation of the DB interface.

class DbImpl final : public DB {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------
    
    DbImpl(std::string path, Options options, storage::PosixFile lock_file,
           storage::Pager pager, wal::LogWriter wal_writer, size_t pool_frames)
        : path_(std::move(path))
        , options_(options)
        , lock_file_(std::move(lock_file))
        , pager_(std::move(pager))
        , wal_writer_(std::move(wal_writer))
        , pool_(pager_, pool_frames, &wal_writer_)
        , tree_(pool_, &wal_writer_)
    {}

    ~DbImpl() override {
        // Best-effort clean shutdown: flush everything and truncate the WAL
        // so the next open needs no replay. On failure the WAL still holds
        // every acknowledged operation and recovery will finish the job.
        if (Status s = Checkpoint(); !s.ok()) {
            // Nothing sensible to do in a destructor; recovery covers us.
        }
    }
    
    // -------------------------------------------------------------------------
    // DB Interface Implementation
    // -------------------------------------------------------------------------
    
    [[nodiscard]]
    Result<std::string> Get(std::string_view key) override {
        return tree_.Get(key);
    }
    
    [[nodiscard]]
    Status Put(std::string_view key, std::string_view value) override {
        if (needs_reopen_.load(std::memory_order_acquire)) [[unlikely]] {
            return Status::Internal(
                "A prior write failed mid-operation; reopen the database "
                "(recovery restores the last consistent state)");
        }
        {
            // Writers share the checkpoint latch; a checkpoint takes it
            // exclusively, so it never runs while a write is mid-flight
            // (Phase 5: many writers, one checkpointer).
            auto ck = AcquireWriterLatch();
            // Autocommit: a fresh single-op transaction. No kBegin/kCommit -
            // a complete op-group is committed by definition (Phase 4 model).
            if (Status s = tree_.Put(key, value, txn_manager_.Begin());
                !s.ok()) [[unlikely]] {
                // InvalidArgument = rejected up front (oversized record),
                // nothing mutated - no need to poison the handle.
                if (s.code() != StatusCode::kInvalidArgument) {
                    needs_reopen_.store(true, std::memory_order_release);
                }
                return s;
            }

            // Phase 4: commit = WAL flushed. Data pages reach disk lazily via
            // the buffer pool (eviction / checkpoint), always behind the log.
            if (options_.sync_on_write) [[likely]] {
                if (Status s = wal_writer_.Flush(); !s.ok()) [[unlikely]] {
                    return s;
                }
            }
        }

        return MaybeAutoCheckpoint();
    }
    
    [[nodiscard]]
    Status Delete(std::string_view key) override {
        if (needs_reopen_.load(std::memory_order_acquire)) [[unlikely]] {
            return Status::Internal(
                "A prior write failed mid-operation; reopen the database "
                "(recovery restores the last consistent state)");
        }
        {
            auto ck = AcquireWriterLatch();
            if (Status s = tree_.Delete(key, txn_manager_.Begin());
                !s.ok()) [[unlikely]] {
                if (s.code() != StatusCode::kNotFound) {
                    needs_reopen_.store(true, std::memory_order_release);
                }
                return s;  // NotFound (nothing changed) or I/O error
            }

            if (options_.sync_on_write) [[likely]] {
                if (Status s = wal_writer_.Flush(); !s.ok()) [[unlikely]] {
                    return s;
                }
            }
        }

        return MaybeAutoCheckpoint();
    }

    [[nodiscard]]
    bool Contains(std::string_view key) override {
        return tree_.Contains(key);
    }

    [[nodiscard]]
    Status Scan(std::string_view start, std::string_view end,
                const ScanCallback& callback) override {
        // Seek handles the empty-start case: the lower bound of "" is the
        // first key in the tree.
        auto cursor = tree_.Seek(start);
        for (; cursor.Valid(); cursor.Next()) {
            if (!end.empty() && cursor.key() >= end) {
                break;  // Exclusive upper bound reached
            }
            if (!callback(cursor.key(), cursor.value())) {
                break;  // Caller stopped the scan
            }
        }
        // Ok on exhaustion / bound / early stop; the error from a failed
        // page read mid-scan otherwise.
        return cursor.status();
    }

    // Defined out of line (below TxnImpl, which needs DbImpl complete).
    [[nodiscard]]
    Result<std::unique_ptr<Transaction>> Begin(TxId retry_of) override;

    [[nodiscard]]
    Status Sync() override {
        // Phase 4: WAL durability is the commit contract - once the log is
        // flushed, every acknowledged op survives a crash (via recovery).
        return wal_writer_.Flush();
    }

    [[nodiscard]]
    Status Checkpoint() override {
        // A checkpoint runs alone: take the checkpoint latch exclusively so no
        // writer is mid-operation while pages, meta, and the WAL are captured
        // and the log is truncated (Phase 5: many writers, one checkpointer).
        auto ck = AcquireCheckpointLatch();
        return CheckpointLocked();
    }

    // -------------------------------------------------------------------------
    // Checkpoint latching. std::shared_mutex makes no fairness promise, so a
    // sustained stream of overlapping writers (shared holders) could starve
    // the checkpointer's exclusive acquisition forever. The fix is a gate:
    // a pending checkpoint bumps pending_ckpts_, and NEW writers park on
    // gate_cv_ instead of piling onto the shared side; the shared holders
    // already in flight drain (each holds the latch for one bounded
    // operation), the checkpoint acquires, and the gate reopens.
    //
    // A writer parked at the gate holds no latch, so it can never block the
    // checkpoint it is waiting for. The one indirect stall: a writer that
    // holds the shared latch while waiting for a key lock whose holder is
    // parked at the gate - the lock manager's 1 s wait backstop breaks that
    // stall (the waiter gets Busy and releases the latch).
    // -------------------------------------------------------------------------

    // Shared side: every mutation (autocommit and transactional ops alike)
    // wraps itself in this latch so a checkpoint never sees a write mid-op.
    [[nodiscard]]
    std::shared_lock<std::shared_mutex> AcquireWriterLatch() {
        if (pending_ckpts_.load(std::memory_order_acquire) != 0) [[unlikely]] {
            std::unique_lock<std::mutex> lk(gate_mu_);
            gate_cv_.wait(lk, [this] {
                return pending_ckpts_.load(std::memory_order_acquire) == 0;
            });
        }
        return std::shared_lock<std::shared_mutex>(checkpoint_mu_);
    }

    // Exclusive side: raise the gate, acquire, reopen the gate. Writers that
    // arrive while the checkpoint holds the latch just block on the shared
    // acquisition itself.
    [[nodiscard]]
    std::unique_lock<std::shared_mutex> AcquireCheckpointLatch() {
        pending_ckpts_.fetch_add(1, std::memory_order_acq_rel);
        std::unique_lock<std::shared_mutex> ck(checkpoint_mu_);
        {
            std::lock_guard<std::mutex> lg(gate_mu_);
            pending_ckpts_.fetch_sub(1, std::memory_order_acq_rel);
        }
        gate_cv_.notify_all();
        return ck;
    }

    // The checkpoint body. Caller must hold checkpoint_mu_ exclusively (the
    // public Checkpoint above, the auto-checkpoint path, or single-threaded
    // open/close).
    [[nodiscard]]
    Status CheckpointLocked() {
        // A poisoned handle may hold a half-applied structural change in
        // memory. Flushing that state - and worse, truncating the WAL that
        // recovery needs to repair it - would make the damage permanent, so
        // refuse (this also covers the destructor's best-effort checkpoint:
        // the WAL survives and the next open recovers).
        if (needs_reopen_.load(std::memory_order_acquire)) [[unlikely]] {
            return Status::Internal(
                "A prior write failed mid-operation; reopen the database");
        }

        // 1. Log first (write-ahead), then data pages, then fsync the data.
        if (Status s = wal_writer_.Flush(); !s.ok()) [[unlikely]] {
            return s;
        }
        if (Status s = pool_.FlushAll(); !s.ok()) [[unlikely]] {
            return s;
        }
        if (Status s = pager_.SyncData(); !s.ok()) [[unlikely]] {
            return s;
        }

        // Fuzzy-checkpoint rule: with a begun-but-unfinished transaction in
        // flight, everything above is safe (flushing its uncommitted pages is
        // ordinary steal), but truncating the log would discard the very
        // records recovery needs to roll it back after a crash. Keep the log;
        // the transaction's own Commit/Abort re-triggers the auto-checkpoint
        // and truncates then. The count is stable here: it only moves under
        // the shared latch, which we hold exclusively.
        if (active_begun_txns_.load(std::memory_order_acquire) > 0) {
            return Status::Ok();
        }

        // 2. Append + flush the checkpoint marker.
        wal::LogRecord marker;
        marker.type = wal::LogRecordType::kCheckpoint;
        marker.op_end = true;  // A checkpoint is its own (empty) op group
        if (auto lsn = wal_writer_.Append(marker); !lsn) [[unlikely]] {
            return lsn.error();
        }
        if (Status s = wal_writer_.Flush(); !s.ok()) [[unlikely]] {
            return s;
        }

        // 3. Advance the meta past everything logged so far and make it
        //    durable (dual meta write + fsync). A crash before this point
        //    replays from the old checkpoint; after it, from the new one.
        const Lsn checkpoint_lsn = wal_writer_.end_lsn();
        if (Status s = pager_.SetCheckpointLsn(checkpoint_lsn); !s.ok()) [[unlikely]] {
            return s;
        }
        if (Status s = pager_.Sync(); !s.ok()) [[unlikely]] {
            return s;
        }

        // 4. Everything before checkpoint_lsn is now in the data file:
        //    restart the log there.
        return wal_writer_.Truncate(checkpoint_lsn);
    }

    // ---------------------------------------------------------------------
    // Recovery (Phase 4, task 4.7) - called by DB::Open before the handle
    // is handed out. Replays records past the checkpoint, page by page,
    // guarded by page_lsn (idempotent: recovering twice is a no-op).
    // ---------------------------------------------------------------------
    [[nodiscard]]
    Status Recover(const std::vector<wal::LogRecord>& records) {
        const Lsn checkpoint = pager_.checkpoint_lsn();
        bool applied_any = false;

        // Extend the data file until `page_id` exists. AllocatePage would
        // pop the free list instead of extending if one were populated -
        // nothing frees pages yet (decision 2.8), but guard against the
        // resulting infinite loop anyway.
        auto ensure_page_exists = [&](PageId page_id) -> Status {
            while (pager_.num_pages() <= page_id) {
                const PageId before = pager_.num_pages();
                if (auto r = pager_.AllocatePage(); !r) [[unlikely]] {
                    return r.error();
                }
                if (pager_.num_pages() == before) [[unlikely]] {
                    return Status::Corruption(
                        "Free-list interference while re-extending during recovery");
                }
            }
            return Status::Ok();
        };

        // Records interleave across transactions under concurrent writers, so
        // reconstruct operations by (txid, op boundary) rather than by WAL
        // byte-contiguity. An op-group is a run of one txid's records ending at
        // op_end; a txid's final group with no op_end was cut by the crash (or
        // by a mid-op WAL-rule flush) and is dropped - applying half a
        // structural change would strand or duplicate keys.
        //
        // Then redo the surviving records in strict LSN order (repeat history):
        // a page's records MUST apply in LSN order or the page_lsn guard would
        // silently drop an earlier update. This matters under concurrency
        // because a split briefly touches a sibling then releases its latch, so
        // two operations' groups can overlap in LSN on that shared page.
        std::vector<char> applicable(records.size(), 0);
        std::unordered_map<TxId, std::vector<size_t>> open_group;
        for (size_t i = 0; i < records.size(); ++i) {
            const wal::LogRecord& record = records[i];
            if (record.lsn < checkpoint) {
                continue;  // Already reflected in checkpointed data pages
            }
            auto& grp = open_group[record.txid];
            grp.push_back(i);
            if (record.op_end) {
                for (size_t j : grp) applicable[j] = 1;
                grp.clear();
            }
        }
        // Leftover open groups are operations that never completed: dropped.
        for (size_t i = 0; i < records.size(); ++i) {
            if (!applicable[i]) {
                continue;
            }
            if (Status s = ApplyRecord(records[i], ensure_page_exists,
                                       applied_any);
                !s.ok()) [[unlikely]] {
                return s;
            }
        }

        // Undo losers (Phase 5.4). A transaction that logged kBegin but neither
        // kCommit nor kAbort was in flight at the crash. Repeat-history redo
        // above re-applied its writes (steal: its dirty pages may have reached
        // the data file), so now roll them back logically, newest write first,
        // using each delta's before-image. The inverses go through the tree
        // under a fresh (committed) txid, so they are logged and made durable
        // by the end-of-recovery checkpoint - and re-running recovery after a
        // crash mid-undo converges (redo replays the durable inverses, the
        // idempotent undo re-derives the rest).
        constexpr uint8_t kBeganBit = 1;
        constexpr uint8_t kEndedBit = 2;
        std::unordered_map<TxId, uint8_t> txn_flags;
        for (const auto& record : records) {
            if (record.lsn < checkpoint) {
                continue;
            }
            if (record.type == wal::LogRecordType::kBegin) {
                txn_flags[record.txid] |= kBeganBit;
            } else if (record.type == wal::LogRecordType::kCommit ||
                       record.type == wal::LogRecordType::kAbort) {
                txn_flags[record.txid] |= kEndedBit;
            }
        }
        auto is_loser = [&](TxId txid) {
            auto it = txn_flags.find(txid);
            return it != txn_flags.end() &&
                   (it->second & kBeganBit) && !(it->second & kEndedBit);
        };

        const TxId undo_txid = txn_manager_.Begin();
        for (size_t i = records.size(); i-- > 0;) {
            if (!applicable[i]) {
                continue;
            }
            const wal::LogRecord& record = records[i];
            if (record.lsn < checkpoint || !is_loser(record.txid)) {
                continue;
            }
            if (record.type == wal::LogRecordType::kPut) {
                // Undo an insert (no before) by deleting; undo an update by
                // restoring the prior value.
                Status s = record.before.has_value()
                               ? tree_.Put(record.key, *record.before, undo_txid)
                               : tree_.Delete(record.key, undo_txid);
                if (!s.ok() && s.code() != StatusCode::kNotFound) [[unlikely]] {
                    return s;
                }
                applied_any = true;
            } else if (record.type == wal::LogRecordType::kDelete &&
                       record.before.has_value()) {
                // Undo a delete by reinserting the removed value.
                if (Status s = tree_.Put(record.key, *record.before, undo_txid);
                    !s.ok()) [[unlikely]] {
                    return s;
                }
                applied_any = true;
            }
        }

        if (applied_any) {
            // num_keys is persisted only at checkpoints, so after a crash it
            // can be stale; recount from the leaf chain (bounded workload:
            // checkpoints cap how much a crash can lose).
            uint64_t count = 0;
            auto cursor = tree_.SeekToFirst();
            for (; cursor.Valid(); cursor.Next()) {
                ++count;
            }
            if (!cursor.status().ok()) [[unlikely]] {
                return cursor.status();
            }
            if (Status s = pager_.SetNumKeys(count); !s.ok()) [[unlikely]] {
                return s;
            }
        }
        return Status::Ok();
    }

private:
    // Redo one record against its target page (see Recover).
    template <typename EnsurePageExists>
    [[nodiscard]]
    Status ApplyRecord(const wal::LogRecord& record,
                       EnsurePageExists& ensure_page_exists,
                       bool& applied_any) {
        {
            switch (record.type) {
                case wal::LogRecordType::kAllocPage: {
                    // Redo must extend the file before any record targets
                    // the new page (plan.md risk: allocation vs redo order).
                    if (Status s = ensure_page_exists(record.page_id);
                        !s.ok()) [[unlikely]] {
                        return s;
                    }
                    break;
                }
                case wal::LogRecordType::kPageImage: {
                    if (record.value.size() != storage::kPageSize) [[unlikely]] {
                        return Status::Corruption("Bad page image size in WAL");
                    }
                    if (Status s = ensure_page_exists(record.page_id);
                        !s.ok()) [[unlikely]] {
                        return s;
                    }
                    auto guard = pool_.FetchPage(record.page_id);
                    if (!guard) {
                        if (guard.error().code() != StatusCode::kCorruption) {
                            return guard.error();
                        }
                        // Torn data-page write: the on-disk page is
                        // unreadable, but this image IS its reconstruction
                        // (full-page-write protocol: the first post-
                        // checkpoint record per page is an image, flushed
                        // before the page could be torn). Overwrite in
                        // place; later records refetch the healed page.
                        storage::Page healed;
                        std::memcpy(healed.RawData(), record.value.data(),
                                    storage::kPageSize);
                        if (Status s = pager_.WritePage(record.page_id, healed);
                            !s.ok()) [[unlikely]] {
                            return s;
                        }
                        applied_any = true;
                        break;
                    }
                    if (record.lsn > guard->page().page_lsn()) {
                        std::memcpy(guard->page().RawData(), record.value.data(),
                                    storage::kPageSize);
                        guard->MarkDirty();  // Image embeds its own page_lsn
                        applied_any = true;
                    }
                    break;
                }
                case wal::LogRecordType::kPut: {
                    auto guard = pool_.FetchPage(record.page_id);
                    if (!guard) [[unlikely]] {
                        return guard.error();
                    }
                    if (record.lsn > guard->page().page_lsn()) {
                        if (Status s = RedoPut(*guard, record); !s.ok()) [[unlikely]] {
                            return s;
                        }
                        applied_any = true;
                    }
                    break;
                }
                case wal::LogRecordType::kDelete: {
                    auto guard = pool_.FetchPage(record.page_id);
                    if (!guard) [[unlikely]] {
                        return guard.error();
                    }
                    if (record.lsn > guard->page().page_lsn()) {
                        btree::NodeView node(guard->page());
                        bool found;
                        uint16_t index = node.FindKeyIndex(record.key, found);
                        if (found && !node.DeleteAt(index)) [[unlikely]] {
                            return Status::Corruption("WAL delete redo failed");
                        }
                        guard->page().header()->page_lsn = record.lsn;
                        guard->MarkDirty();
                        applied_any = true;
                    }
                    break;
                }
                case wal::LogRecordType::kRootChange: {
                    if (Status s = pager_.SetRootPageId(record.page_id);
                        !s.ok()) [[unlikely]] {
                        return s;
                    }
                    applied_any = true;
                    break;
                }
                case wal::LogRecordType::kFreePage:   // Nothing emits this yet
                case wal::LogRecordType::kCheckpoint:
                case wal::LogRecordType::kBegin:       // Txn markers carry no
                case wal::LogRecordType::kCommit:      // page mutation; redo is
                case wal::LogRecordType::kAbort:       // a no-op (undo uses them
                    break;                             // in Phase 5.4).
            }
        }
        return Status::Ok();
    }

    // Redo one kPut against its target leaf, repeating runtime history:
    // in-place update, or delete + reinsert when the value outgrew its slot.
    [[nodiscard]]
    static Status RedoPut(storage::PageGuard& guard, const wal::LogRecord& record) {
        btree::NodeView node(guard.page());
        bool found;
        uint16_t index = node.FindKeyIndex(record.key, found);
        if (found) {
            if (!node.UpdateLeaf(record.key, record.value)) {
                if (!node.DeleteAt(index) ||
                    !node.InsertLeaf(record.key, record.value)) [[unlikely]] {
                    return Status::Corruption("WAL put redo failed (update)");
                }
            }
        } else if (!node.InsertLeaf(record.key, record.value)) [[unlikely]] {
            return Status::Corruption("WAL put redo failed (insert)");
        }
        guard.page().header()->page_lsn = record.lsn;
        guard.MarkDirty();
        return Status::Ok();
    }

    [[nodiscard]]
    Status MaybeAutoCheckpoint() {
        if (options_.max_wal_bytes == 0) {
            return Status::Ok();
        }
        if (wal_writer_.SizeBytes() < options_.max_wal_bytes) {
            return Status::Ok();
        }
        // With a begun transaction in flight the checkpoint couldn't truncate
        // anyway (see CheckpointLocked) - skip instead of paying a full pool
        // flush per write while over the threshold. The transaction's own
        // Commit/Abort lands here again with the count at zero. Racy read,
        // but safe either way: a stale nonzero skips once (the next write
        // retries), a stale zero just runs a checkpoint that skips truncation.
        if (active_begun_txns_.load(std::memory_order_acquire) > 0) {
            return Status::Ok();
        }
        // Crossed the threshold: take the checkpoint latch exclusively, then
        // re-check - a concurrent writer may have already checkpointed and
        // truncated the log while we waited for the latch.
        auto ck = AcquireCheckpointLatch();
        if (wal_writer_.SizeBytes() < options_.max_wal_bytes) {
            return Status::Ok();
        }
        return CheckpointLocked();
    }

    // Append a transaction marker (kBegin / kCommit / kAbort). Each marker is
    // its own complete op-group; the caller holds the writer latch for the
    // operation that logs it, so it can't race a checkpoint truncation.
    [[nodiscard]]
    Status LogTxnMarker(TxId txid, wal::LogRecordType type) {
        wal::LogRecord record;
        record.type = type;
        record.txid = txid;
        record.op_end = true;
        record.page_id = kInvalidPageId;
        auto lsn = wal_writer_.Append(record);
        if (!lsn) [[unlikely]] {
            return lsn.error();
        }
        return Status::Ok();
    }

    friend class TxnImpl;  // Explicit transactions drive tree_/wal_/txn_manager_

public:

    [[nodiscard]]
    Stats GetStats() const override {
        const auto& pool_stats = pool_.stats();
        return Stats{.pool_hits = pool_stats.hits,
                     .pool_misses = pool_stats.misses,
                     .pool_evictions = pool_stats.evictions};
    }
    
    [[nodiscard]]
    uint64_t ApproximateKeyCount() const override {
        return pager_.KeyCount();  // Synchronized: may run alongside writers
    }
    
    [[nodiscard]]
    const std::string& GetPath() const override {
        return path_;
    }

private:
    std::string path_;
    Options options_;
    // Set when a write op fails half-applied (redo-only WAL, no undo until 5.4).
    std::atomic<bool> needs_reopen_{false};
    // Phase 5: writers hold this shared for the duration of ONE mutation (a
    // transaction takes it per-operation, never across user think time); a
    // checkpoint holds it exclusive so it runs alone.
    std::shared_mutex checkpoint_mu_;
    // Checkpoint gate (see AcquireWriterLatch/AcquireCheckpointLatch): a
    // pending exclusive acquisition parks NEW writers here so the in-flight
    // shared holders can drain - std::shared_mutex itself promises no writer
    // preference, so sustained write traffic could otherwise starve the
    // checkpointer forever.
    std::mutex gate_mu_;
    std::condition_variable gate_cv_;
    std::atomic<int> pending_ckpts_{0};
    // Transactions that have logged kBegin but not yet ended. While nonzero,
    // checkpoints skip WAL truncation: the log still holds the records a
    // crash-recovery undo of those transactions would need. Moves only under
    // checkpoint_mu_ held shared, so it is stable under the exclusive hold.
    std::atomic<int> active_begun_txns_{0};
    // Hands out a txid to every operation - autocommit and explicit alike -
    // so recovery can group interleaved WAL records by transaction.
    txn::TxnManager txn_manager_;
    // Per-key S/X locks for explicit transactions (strict 2PL, Phase 5.5).
    txn::LockManager lock_manager_;
    // Holds the flock on <path>/LOCK for this DbImpl's lifetime (single
    // process per database directory). Declared before pager_/wal_writer_ so
    // it is released only after they are closed.
    storage::PosixFile lock_file_;
    storage::Pager pager_;
    wal::LogWriter wal_writer_;  // Declared before pool_/tree_ (they hold pointers)
    storage::BufferPool pool_;   // References pager_ + wal_writer_
    btree::BPlusTree tree_;      // References pool_ + wal_writer_
};

// ==============================================================================
// TxnImpl - explicit multi-statement transaction
// ==============================================================================
//
// Takes the DB-level checkpoint latch shared PER OPERATION - one bounded hold
// around each tree write and around the commit/abort logging - never across
// user think time. An idle open transaction therefore holds no latch at all:
// checkpoints run (and flush pages) freely alongside it. What an in-flight
// transaction does pin is the LOG TAIL: from its kBegin until its end marker
// it keeps active_begun_txns_ nonzero, and checkpoints skip WAL truncation
// while that count is up, because the log still holds the records a
// crash-recovery undo of this transaction would need (steal is allowed, so
// its uncommitted pages may already be on disk). Writes apply to the shared
// tree in place under the transaction's txid; an in-memory undo list lets
// Abort put the prior values back. kBegin is logged lazily on the first
// write, kCommit / kAbort on end. Recovery of a committed or runtime-aborted
// transaction needs no special undo pass - repeat-history redo replays its
// records (an abort's inverse writes included); crash-in-flight losers are
// the job of Phase 5.4.

class TxnImpl final : public Transaction {
public:
    // `txid` is this attempt's WAL identity (always fresh); `priority` is its
    // wait-die age - equal to txid on a first attempt, inherited from the
    // aborted attempt on a retry (DB::Begin(retry_of)).
    TxnImpl(DbImpl* db, TxId txid, TxId priority)
        : db_(db), txid_(txid), priority_(priority) {}

    ~TxnImpl() override {
        if (active_) {
            (void)Abort();  // Roll back an abandoned transaction
        }
    }

    [[nodiscard]]
    Result<std::string> Get(std::string_view key) override {
        if (!active_) [[unlikely]] {
            return std::unexpected(Status::Internal("Transaction is not active"));
        }
        // Strict 2PL: an S-lock held to commit excludes concurrent writers, so
        // no dirty read. Busy means wait-die told us to abort and retry.
        if (Status s = db_->lock_manager_.Acquire(txid_, priority_, key,
                                                  txn::LockMode::kShared);
            !s.ok()) [[unlikely]] {
            return std::unexpected(s);
        }
        return db_->tree_.Get(key);  // Sees this txn's own uncommitted writes
    }

    [[nodiscard]]
    Status Put(std::string_view key, std::string_view value) override {
        if (!active_) [[unlikely]] {
            return Status::Internal("Transaction is not active");
        }
        if (Status s = db_->lock_manager_.Acquire(txid_, priority_, key,
                                                  txn::LockMode::kExclusive);
            !s.ok()) [[unlikely]] {
            return s;  // wait-die abort: caller aborts + retries
        }
        // Key lock first (may block), THEN the writer latch (bounded hold):
        // no thread ever waits on a key lock while holding the latch, so a
        // pending checkpoint always drains. kBegin and the write share one
        // hold - a checkpoint cannot slice between them.
        auto ck = db_->AcquireWriterLatch();
        if (Status s = EnsureBegun(); !s.ok()) [[unlikely]] return s;
        std::optional<std::string> before;
        if (Status s = db_->tree_.Put(key, value, txid_, &before);
            !s.ok()) [[unlikely]] {
            if (s.code() == StatusCode::kInvalidArgument) {
                return s;  // Oversized record rejected up front: no mutation
            }
            Poison();  // Half-applied: no clean runtime rollback (recovery's job)
            return s;
        }
        undo_.push_back({std::string(key), std::move(before)});
        return Status::Ok();
    }

    [[nodiscard]]
    Status Delete(std::string_view key) override {
        if (!active_) [[unlikely]] {
            return Status::Internal("Transaction is not active");
        }
        if (Status s = db_->lock_manager_.Acquire(txid_, priority_, key,
                                                  txn::LockMode::kExclusive);
            !s.ok()) [[unlikely]] {
            return s;  // wait-die abort: caller aborts + retries
        }
        auto ck = db_->AcquireWriterLatch();  // Key lock first, then latch
        if (Status s = EnsureBegun(); !s.ok()) [[unlikely]] return s;
        std::optional<std::string> before;
        Status s = db_->tree_.Delete(key, txid_, &before);
        if (s.code() == StatusCode::kNotFound) {
            return s;  // Nothing changed, nothing to undo
        }
        if (!s.ok()) [[unlikely]] {
            Poison();
            return s;
        }
        undo_.push_back({std::string(key), std::move(before)});
        return Status::Ok();
    }

    [[nodiscard]]
    Status Commit() override {
        if (!active_) [[unlikely]] {
            return Status::Internal("Transaction is not active");
        }
        active_ = false;
        if (begun_) {
            // One bounded latch hold: the commit record, its flush, and the
            // "this txn no longer pins the log" decrement land atomically
            // with respect to a checkpoint's exclusive hold.
            auto ck = db_->AcquireWriterLatch();
            Status s = db_->LogTxnMarker(txid_, wal::LogRecordType::kCommit);
            if (s.ok() && db_->options_.sync_on_write) {
                // Durability contract: the commit record is flushed.
                s = db_->wal_writer_.Flush();
            }
            if (!s.ok()) [[unlikely]] {
                // Neither committed nor abortable (the WAL path just failed):
                // poison the handle. Reopen + recovery decides the outcome
                // from what actually reached the log; the counter stays up,
                // which is moot - a poisoned handle refuses checkpoints.
                Poison();
                return s;
            }
            db_->active_begun_txns_.fetch_sub(1, std::memory_order_acq_rel);
        }
        // Strict 2PL: every lock is held to the end of the transaction and
        // released here, in one shot.
        db_->lock_manager_.ReleaseAll(txid_);
        return db_->MaybeAutoCheckpoint();
    }

    [[nodiscard]]
    Status Abort() override {
        if (!active_) [[unlikely]] {
            return Status::Internal("Transaction is not active");
        }
        active_ = false;
        if (begun_) {
            // Undo writes newest-first: restore the prior value, or delete a
            // key this txn freshly inserted. Applied to the shared tree under
            // our txid; the inverse records make the rollback durable
            // (recovery redoes them). One latch hold across the whole undo -
            // bounded by the transaction's own write count.
            auto ck = db_->AcquireWriterLatch();
            Status result = Status::Ok();
            for (auto it = undo_.rbegin(); it != undo_.rend() && result.ok();
                 ++it) {
                if (it->before.has_value()) {
                    result = db_->tree_.Put(it->key, *it->before, txid_);
                } else {
                    Status s = db_->tree_.Delete(it->key, txid_);
                    result =
                        (s.code() == StatusCode::kNotFound) ? Status::Ok() : s;
                }
            }
            if (result.ok()) {
                result = db_->LogTxnMarker(txid_, wal::LogRecordType::kAbort);
            }
            if (!result.ok()) [[unlikely]] {
                // A failed undo leaves the tree half-rolled-back with the
                // locks about to drop - poison so nothing else writes through
                // (or checkpoints) that state; reopen + recovery repairs it.
                Poison();
                return result;
            }
            db_->active_begun_txns_.fetch_sub(1, std::memory_order_acq_rel);
        }
        // Strict 2PL: locks are released only after the rollback writes are
        // applied, so no other transaction sees a half-undone state. Gets may
        // have taken S locks even in a never-begun transaction.
        db_->lock_manager_.ReleaseAll(txid_);
        return Status::Ok();
    }

    [[nodiscard]]
    TxId id() const override { return priority_; }

private:
    // One buffered write, enough to invert it: restore `before` (an update or
    // delete) or, when nullopt, delete the freshly inserted `key`.
    struct UndoAction {
        std::string key;
        std::optional<std::string> before;
    };

    // A write failed half-applied (or the WAL path failed mid-end-marker): no
    // clean runtime repair exists (reopen + recovery restores consistency).
    // End the transaction WITHOUT undo, and poison the whole DB handle - the
    // shared tree may contain a half-completed change, so letting OTHER
    // threads keep writing through it (or checkpointing it - see
    // CheckpointLocked's refusal) would compound the damage. Locks are still
    // released - leaking them would starve every later transaction that
    // touches the same keys. active_begun_txns_ is deliberately NOT
    // decremented: the poisoned handle refuses checkpoints anyway.
    void Poison() {
        active_ = false;
        db_->needs_reopen_.store(true, std::memory_order_release);
        db_->lock_manager_.ReleaseAll(txid_);
    }

    // Log kBegin lazily on the first write and start pinning the log tail
    // (checkpoints stop truncating until our end marker). Caller holds the
    // writer latch.
    [[nodiscard]]
    Status EnsureBegun() {
        if (begun_) {
            return Status::Ok();
        }
        if (Status s = db_->LogTxnMarker(txid_, wal::LogRecordType::kBegin);
            !s.ok()) [[unlikely]] {
            return s;
        }
        begun_ = true;
        db_->active_begun_txns_.fetch_add(1, std::memory_order_acq_rel);
        return Status::Ok();
    }

    DbImpl* db_;
    TxId txid_;      // WAL identity - fresh per attempt
    TxId priority_;  // Wait-die age - survives retries via Begin(retry_of)
    std::vector<UndoAction> undo_;
    bool active_ = true;
    bool begun_ = false;  // kBegin logged lazily on the first write
};

Result<std::unique_ptr<Transaction>> DbImpl::Begin(TxId retry_of) {
    if (needs_reopen_.load(std::memory_order_acquire)) [[unlikely]] {
        return std::unexpected(Status::Internal(
            "A prior write failed mid-operation; reopen the database"));
    }
    const TxId txid = txn_manager_.Begin();
    // A retry keeps the aborted attempt's wait-die age; the WAL txid is
    // always fresh (recovery must not see one txid begin twice).
    const TxId priority = retry_of == kInvalidTxId ? txid : retry_of;
    return std::unique_ptr<Transaction>(
        std::make_unique<TxnImpl>(this, txid, priority));
}

// Resolve the buffer pool size: explicit option, else the test-hook env
// variable, else a 64-frame (256 KB) default. Clamped to kMinTreeFrames:
// a Put whose split cascades to the root pins the whole path plus every
// page the op rewrote (op-group atomicity keeps them pinned until the
// op-end WAL record is appended) - up to 2*depth+2 frames. 16 covers
// depth 7 (billions of keys at the observed fanout).
static size_t ResolvePoolFrames(const Options& options) {
    constexpr size_t kMinTreeFrames = 16;
    size_t frames = 64;
    if (options.buffer_pool_frames != 0) {
        frames = options.buffer_pool_frames;
    } else if (const char* env = std::getenv("KVSTORE_POOL_FRAMES");
               env != nullptr && *env != '\0') {
        long parsed = std::strtol(env, nullptr, 10);
        if (parsed > 0) {
            frames = static_cast<size_t>(parsed);
        }
    }
    return frames < kMinTreeFrames ? kMinTreeFrames : frames;
}

// ==============================================================================
// DB::Open Implementation
// ==============================================================================

Result<std::unique_ptr<DB>> DB::Open(const std::string& path, const Options& options) {
    namespace fs = std::filesystem;
    
    // -------------------------------------------------------------------------
    // Step 1: Ensure the directory exists
    // -------------------------------------------------------------------------
    fs::path dir_path(path);
    
    if (fs::exists(dir_path)) {
        // Path exists - make sure it's a directory
        if (!fs::is_directory(dir_path)) [[unlikely]] {
            return std::unexpected(Status::InvalidArgument("Path exists but is not a directory: " + path));
        }
    } else {
        // Path doesn't exist
        if (!options.create_if_missing) [[unlikely]] {
            return std::unexpected(Status::NotFound("Database directory not found: " + path));
        }
        
        // Create the directory
        std::error_code ec;
        if (!fs::create_directories(dir_path, ec)) [[unlikely]] {
            return std::unexpected(Status::IoError("Failed to create directory: " + path + " - " + ec.message()));
        }
    }
    
    // -------------------------------------------------------------------------
    // Step 2: Take the single-process lock BEFORE touching data.db or the WAL
    // (a second process must not get as far as running recovery). LOCK is a
    // zero-byte file whose flock, held for the DbImpl's lifetime, is the lock;
    // the kernel drops it when the holder exits or crashes, so it never goes
    // stale and reopening after a crash needs no cleanup.
    // -------------------------------------------------------------------------
    auto lock_file_result =
        storage::PosixFile::Open((dir_path / "LOCK").string(),
                                 /*create_if_missing=*/true);
    if (!lock_file_result) [[unlikely]] {
        return std::unexpected(Status::IoError(
            "Failed to open lock file: " + lock_file_result.error().message));
    }
    auto lock_file = std::move(*lock_file_result);
    if (auto locked = lock_file.TryLockExclusive(); !locked) [[unlikely]] {
        if (locked.error().kind == storage::FileError::Kind::Busy) {
            return std::unexpected(Status::Busy(
                "Database is in use by another process: " + path));
        }
        return std::unexpected(Status::IoError(
            "Failed to lock database: " + locked.error().message));
    }

    // -------------------------------------------------------------------------
    // Step 3: Open the data file
    // -------------------------------------------------------------------------
    auto data_file_path = (dir_path / "data.db").string();
    
    auto pager_result = storage::Pager::Open(data_file_path, options.create_if_missing,
                                             options.verify_checksums);
    if (!pager_result) [[unlikely]] {
        return std::unexpected(pager_result.error());
    }
    
    auto pager = std::move(*pager_result);
    
    // -------------------------------------------------------------------------
    // Step 4: Read any leftover WAL (crash evidence) and open the writer
    // -------------------------------------------------------------------------
    auto wal_path = (dir_path / "wal.log").string();

    auto records_result = wal::LogReader::ReadAll(wal_path);
    if (!records_result) [[unlikely]] {
        return std::unexpected(records_result.error());
    }
    auto& records = *records_result;

    std::error_code wal_ec;
    const bool wal_has_bytes = fs::exists(wal_path, wal_ec) &&
                               fs::file_size(wal_path, wal_ec) > 0;

    // The writer's base is the LSN of the file's first byte: derived from
    // the first surviving record, else the checkpoint the meta remembers.
    const Lsn base_lsn =
        !records.empty() ? records.front().lsn
        : (pager.checkpoint_lsn() == kInvalidLsn ? kFirstLsn
                                                 : pager.checkpoint_lsn());
    auto wal_writer_result = wal::LogWriter::Open(wal_path, base_lsn);
    if (!wal_writer_result) [[unlikely]] {
        return std::unexpected(wal_writer_result.error());
    }

    // -------------------------------------------------------------------------
    // Step 5: Create the DbImpl instance and recover if the WAL is non-empty
    // -------------------------------------------------------------------------
    auto db = std::make_unique<DbImpl>(
        path, 
        options, 
        std::move(lock_file),
        std::move(pager),
        std::move(*wal_writer_result),
        ResolvePoolFrames(options)
    );

    if (wal_has_bytes) {
        if (Status s = db->Recover(records); !s.ok()) [[unlikely]] {
            return std::unexpected(s);
        }
        // End-of-recovery checkpoint: persists the replayed state and
        // truncates the log (also clears a torn tail the reader skipped).
        if (Status s = db->Checkpoint(); !s.ok()) [[unlikely]] {
            return std::unexpected(s);
        }
    }
    
    return db;
}

}  // namespace kvstore
