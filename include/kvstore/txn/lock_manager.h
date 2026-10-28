// ==============================================================================
// kvstore/txn/lock_manager.h - Per-Key S/X Locks, Strict 2PL (Phase 5.5)
// ==============================================================================
//
// Gives explicit transactions row-level (per-key) shared/exclusive locks held
// until commit or abort (strict two-phase locking), which serializes
// conflicting transactions and rules out dirty reads, lost updates, and
// write-write races.
//
// Deadlock avoidance is WAIT-DIE, keyed on transaction AGE: a requester that
// is OLDER than every conflicting holder waits; a YOUNGER requester is told
// to die (Status::Busy) so its caller aborts and retries. Because a
// transaction only ever waits for younger holders, the waits-for graph has
// no cycles - no deadlock is possible. A one-second timeout is a backstop
// against a lost wakeup, not the primary mechanism.
//
// Age is a `priority` passed separately from the txid (smaller = older).
// A first-attempt transaction's priority IS its txid; a retry of a wait-die
// victim inherits the victim's priority while logging under a fresh txid
// (see DB::Begin(retry_of)). That separation is what makes wait-die
// starvation-free - a victim that kept losing under an ever-newer txid
// would never become "the oldest"; with its original priority it eventually
// outranks every conflicting holder and waits instead of dying. The fresh
// txid keeps recovery sound: reusing the WAL txid would make a crash
// mid-retry look like an already-ended transaction and skip its undo.
//
// Autocommit Get/Put/Delete do NOT take these locks: each is a single
// operation made atomic by the tree's page latches, and is documented as not
// isolated from an explicit transaction's uncommitted writes. The lock manager
// isolates explicit transactions from one another.
//
// Locking: one mutex guards the whole table (coarse but simple; sharding is a
// documented future refinement). It is a leaf lock - Acquire/ReleaseAll never
// call back into the tree, pool, or WAL while holding it.
//
// ==============================================================================

#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kvstore/status.h"
#include "kvstore/types.h"

namespace kvstore::txn {

enum class LockMode : uint8_t {
    kShared,     // Reader: compatible with other shared holders
    kExclusive,  // Writer: sole holder
};

class LockManager {
public:
    LockManager() = default;

    LockManager(const LockManager&) = delete;
    LockManager& operator=(const LockManager&) = delete;

    // Acquire `key` in `mode` for `txid`, blocking under wait-die until the
    // lock is granted. `priority` is the transaction's age (smaller = older;
    // a first attempt passes its own txid, a retry passes the aborted
    // attempt's priority). Returns Ok on grant (including a re-acquire or an
    // S->X upgrade of a lock this txn already holds), or Status::Busy when
    // wait-die requires this transaction to abort (younger than a conflicting
    // holder) or the one-second backstop elapses. Held until ReleaseAll.
    [[nodiscard("Acquire returns status - Busy means abort + retry")]]
    Status Acquire(TxId txid, TxId priority, std::string_view key,
                   LockMode mode);

    // Release every lock held by `txid` and wake waiters. Called once at the
    // end of a transaction (commit or abort). Idempotent.
    void ReleaseAll(TxId txid);

private:
    struct Holder {
        LockMode mode;
        TxId priority;  // Wait-die age of this holder (smaller = older)
    };
    struct Entry {
        std::unordered_map<TxId, Holder> holders;
    };

    std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_map<std::string, Entry> table_;   // key -> current holders
    std::unordered_map<TxId, std::vector<std::string>> held_;  // txid -> its keys
};

}  // namespace kvstore::txn
