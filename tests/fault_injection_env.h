// ==============================================================================
// tests/fault_injection_env.h - Test-only Env that counts syncs and tears writes
// ==============================================================================
//
// FaultInjectionEnv opens real PosixFiles and wraps each in a FaultyFile that
// reports back to the Env. State is per Env instance, never process-wide, so
// tests can't leak faults into each other and can run in parallel.
//
// Usage:
//   test::FaultInjectionEnv env;
//   Options options;
//   options.env = &env;            // env must outlive the DB
//   auto db = DB::Open(dir, options).value();
//   env.FailAfterNWrites(3);       // 4th write (any file) tears, then all fail
//   ...
//   EXPECT_EQ(env.syncs(), 0u);
//
// Counting is across every file the Env opened (data file and WAL alike),
// because a crash cuts power to the whole disk, not to one file.
//
// ==============================================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <tuple>

#include "kvstore/storage/file.h"

namespace kvstore::test {

class FaultInjectionEnv final : public storage::Env {
public:
    // After `n` more successful writes, the next write persists only half its
    // bytes and returns IoError (a torn write); every write after that fails
    // too, until ClearWriteFailure().
    void FailAfterNWrites(int64_t n) noexcept {
        fail_after_writes_.store(n, std::memory_order_relaxed);
    }
    void ClearWriteFailure() noexcept {
        fail_after_writes_.store(kDisarmed, std::memory_order_relaxed);
    }

    // Successful Sync() calls on files opened through this Env.
    [[nodiscard]] uint64_t syncs() const noexcept {
        return syncs_.load(std::memory_order_relaxed);
    }

    storage::FileResult<std::unique_ptr<storage::File>> OpenFile(
        std::string_view path, bool create_if_missing) override {
        auto file = storage::Env::Default()->OpenFile(path, create_if_missing);
        if (!file) {
            return std::unexpected(std::move(file.error()));
        }
        return std::make_unique<FaultyFile>(std::move(*file), *this);
    }

private:
    class FaultyFile final : public storage::File {
    public:
        FaultyFile(std::unique_ptr<storage::File> base, FaultInjectionEnv& env)
            : base_(std::move(base)), env_(env) {}

        storage::FileStatus Write(uint64_t offset, const void* buf,
                                  size_t size) override {
            if (env_.ConsumeWriteCredit()) {
                // Half the bytes land, then failure: what a power cut
                // mid-pwrite leaves behind.
                std::ignore = base_->Write(offset, buf, size / 2);
                return std::unexpected(storage::FileError::IoError(std::format(
                    "Injected write failure (torn) on '{}'", base_->path())));
            }
            return base_->Write(offset, buf, size);
        }

        storage::FileStatus Sync() override {
            auto result = base_->Sync();
            if (result) {
                env_.syncs_.fetch_add(1, std::memory_order_relaxed);
            }
            return result;
        }

        storage::FileStatus Read(uint64_t offset, void* buf,
                                 size_t size) const override {
            return base_->Read(offset, buf, size);
        }
        storage::FileStatus TryLockExclusive() override {
            return base_->TryLockExclusive();
        }
        storage::FileResult<uint64_t> Size() const override {
            return base_->Size();
        }
        storage::FileStatus Truncate(uint64_t size) override {
            return base_->Truncate(size);
        }
        std::string_view path() const override { return base_->path(); }

    private:
        std::unique_ptr<storage::File> base_;
        FaultInjectionEnv& env_;
    };

    // True if this write must tear. CAS decrement so concurrent writers each
    // consume a distinct credit - a load-then-store pair could hand two
    // writers the same count. Whoever decrements from 0 (or below) is in the
    // failing regime.
    bool ConsumeWriteCredit() noexcept {
        int64_t remaining = fail_after_writes_.load(std::memory_order_relaxed);
        while (remaining != kDisarmed &&
               !fail_after_writes_.compare_exchange_weak(
                   remaining, remaining - 1, std::memory_order_relaxed)) {
            // remaining reloaded by the failed CAS; re-check for a
            // concurrent ClearWriteFailure.
        }
        return remaining != kDisarmed && remaining <= 0;
    }

    // INT64_MIN, not -1: post-fire values keep decrementing below zero and
    // must never collide with "disarmed".
    static constexpr int64_t kDisarmed = std::numeric_limits<int64_t>::min();

    std::atomic<int64_t> fail_after_writes_{kDisarmed};
    std::atomic<uint64_t> syncs_{0};
};

}  // namespace kvstore::test
