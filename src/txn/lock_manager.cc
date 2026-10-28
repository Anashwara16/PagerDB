// ==============================================================================
// kvstore/txn/lock_manager.cc - Per-Key S/X Locks, Strict 2PL (Phase 5.5)
// ==============================================================================

#include "kvstore/txn/lock_manager.h"

#include <chrono>

namespace kvstore::txn {

namespace {

// Does granting `want` to `requester` conflict with a holder in mode `held`?
// Shared/shared is the only compatible pair; a holder that is the requester
// itself never conflicts (re-acquire / upgrade).
bool Conflicts(TxId requester, LockMode want, TxId holder, LockMode held) {
    if (holder == requester) {
        return false;
    }
    return want == LockMode::kExclusive || held == LockMode::kExclusive;
}

}  // namespace

Status LockManager::Acquire(TxId txid, TxId priority,
                            std::string_view key_view, LockMode mode) {
    std::string key(key_view);
    std::unique_lock<std::mutex> lk(mu_);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);

    for (;;) {
        auto it = table_.find(key);

        // Scan current holders for a conflict, and for wait-die note whether
        // any conflicting holder is OLDER than us (smaller priority) - if so
        // we are the younger party and must die rather than wait.
        bool conflict = false;
        bool older_conflict = false;
        if (it != table_.end()) {
            for (const auto& [holder, held] : it->second.holders) {
                if (Conflicts(txid, mode, holder, held.mode)) {
                    conflict = true;
                    if (held.priority < priority) {
                        older_conflict = true;
                        break;
                    }
                }
            }
        }

        if (!conflict) {
            Entry& entry = table_[key];
            auto [holder_it, inserted] =
                entry.holders.try_emplace(txid, Holder{mode, priority});
            if (inserted) {
                held_[txid].push_back(key);
            } else if (mode == LockMode::kExclusive) {
                holder_it->second.mode = LockMode::kExclusive;  // S -> X upgrade
            }
            return Status::Ok();
        }

        if (older_conflict) {
            // Wait-die: a younger transaction never waits for an older holder.
            return Status::Busy(
                "wait-die: transaction aborted to avoid deadlock");
        }

        // Older than every conflicting holder: wait for a release.
        if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) {
            return Status::Busy("lock wait timed out");
        }
    }
}

void LockManager::ReleaseAll(TxId txid) {
    std::lock_guard<std::mutex> lk(mu_);
    auto held_it = held_.find(txid);
    if (held_it == held_.end()) {
        return;
    }
    for (const std::string& key : held_it->second) {
        auto entry_it = table_.find(key);
        if (entry_it != table_.end()) {
            entry_it->second.holders.erase(txid);
            if (entry_it->second.holders.empty()) {
                table_.erase(entry_it);
            }
        }
    }
    held_.erase(held_it);
    cv_.notify_all();  // Wake every waiter to re-check (coarse but simple)
}

}  // namespace kvstore::txn
