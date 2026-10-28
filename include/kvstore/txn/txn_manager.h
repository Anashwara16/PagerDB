// ==============================================================================
// kvstore/txn/txn_manager.h - Transaction ID Allocation (Phase 5.3)
// ==============================================================================
//
// Hands out monotonically increasing transaction ids. Every operation - both
// explicit transactions (DB::Begin) and the single-op transactions that wrap
// autocommit Get/Put/Delete - gets its own id. That id is stamped into every
// WAL record the operation emits, which is what lets recovery reconstruct
// operations by id even when many writers interleave their records in the
// shared log (WAL byte-contiguity is not assumed - see DbImpl::Recover).
//
// Ordering doubles as transaction age for the lock manager's wait-die scheme
// (Phase 5.5): a smaller id is an older transaction.
//
// ids need not survive restarts: recovery truncates the WAL once it finishes,
// so no two runs ever share a live id, and the counter safely restarts at 1.
// Id 0 (kInvalidTxId) is reserved for "no transaction" and for the inverse
// records recovery writes while undoing losers (always treated as committed).
//
// ==============================================================================

#pragma once

#include <atomic>

#include "kvstore/types.h"

namespace kvstore::txn {

class TxnManager {
public:
    TxnManager() = default;

    TxnManager(const TxnManager&) = delete;
    TxnManager& operator=(const TxnManager&) = delete;

    // Allocate the next transaction id (>= 1, strictly increasing).
    [[nodiscard("Begin returns the new transaction id")]]
    TxId Begin() noexcept {
        return next_.fetch_add(1, std::memory_order_relaxed);
    }

    // The id the next Begin() would return (for tests / introspection).
    [[nodiscard("peek returns the next id without consuming it")]]
    TxId peek_next() const noexcept {
        return next_.load(std::memory_order_relaxed);
    }

private:
    // First handed-out id is kFirstTxId; 0 stays reserved (kInvalidTxId).
    static constexpr TxId kFirstTxId = 1;
    std::atomic<TxId> next_{kFirstTxId};
};

}  // namespace kvstore::txn
