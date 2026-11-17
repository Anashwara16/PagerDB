// ==============================================================================
// kvstore/storage/file.h - File I/O abstraction
// ==============================================================================
//
// Lowest layer of the storage stack: a thin, RAII-managed POSIX file wrapper
// behind a small virtual interface (File) and a factory (Env).
//
// Conventions:
//   - Errors are returned as std::expected values; nothing throws across this
//     boundary, so callers (WAL, pager) can make explicit durability decisions
//     on every failure.
//   - All I/O is positioned (pread/pwrite). No shared file offset means no
//     seek/read races and no per-file I/O lock at this layer.
//   - Read/Write are all-or-error: a short transfer is reported as a failure
//     rather than surfaced to the caller.
//
// ==============================================================================

#pragma once

#include <cstddef>      // size_t
#include <cstdint>      // uint64_t
#include <expected>     // std::expected (C++23)
#include <memory>       // std::unique_ptr (Env::OpenFile)
#include <string>
#include <string_view>  // std::string_view
#include <system_error> // std::error_code

namespace kvstore::storage {

// ==============================================================================
// Error Types
// ==============================================================================

struct FileError {
    enum class Kind {
        NotFound,       // Path does not exist
        IoError,        // errno-level failure or short read/write
        Busy,           // Lock contention (TryLockExclusive)
        InternalError   // Invariant violation, e.g. operation on a closed file
    };

    Kind kind;
    std::string message;

    [[nodiscard]] static FileError NotFound(std::string_view msg) {
        return {Kind::NotFound, std::string(msg)};
    }

    [[nodiscard]] static FileError IoError(std::string_view msg) {
        return {Kind::IoError, std::string(msg)};
    }

    [[nodiscard]] static FileError Busy(std::string_view msg) {
        return {Kind::Busy, std::string(msg)};
    }

    [[nodiscard]] static FileError Internal(std::string_view msg) {
        return {Kind::InternalError, std::string(msg)};
    }
};

template <typename T>
using FileResult = std::expected<T, FileError>;

using FileStatus = std::expected<void, FileError>;

// ==============================================================================
// File Interface
// ==============================================================================
//
// Pager and LogWriter hold a std::unique_ptr<File> rather than a concrete
// PosixFile so tests can interpose decorators (tests/fault_injection_env.h) to
// count syncs, drop unsynced data, or tear writes, with no test hooks in
// production code. Cost is one virtual call per I/O, negligible next to the
// syscall. Operation contracts are documented on PosixFile.

class File {
public:
    virtual ~File() = default;

    [[nodiscard]] virtual FileStatus Read(uint64_t offset, void* buf,
                                          size_t size) const = 0;
    [[nodiscard]] virtual FileStatus Write(uint64_t offset, const void* buf,
                                           size_t size) = 0;
    [[nodiscard]] virtual FileStatus Sync() = 0;
    [[nodiscard]] virtual FileStatus TryLockExclusive() = 0;
    [[nodiscard]] virtual FileResult<uint64_t> Size() const = 0;
    [[nodiscard]] virtual FileStatus Truncate(uint64_t size) = 0;
    [[nodiscard]] virtual std::string_view path() const = 0;
};

// ==============================================================================
// Env - File Factory
// ==============================================================================
//
// Single injection point for all file creation (LevelDB/RocksDB-style Env).
// Options::env lets tests wrap every file the database opens.

class Env {
public:
    virtual ~Env() = default;

    // Same semantics as PosixFile::Open.
    [[nodiscard]] virtual FileResult<std::unique_ptr<File>> OpenFile(
        std::string_view path, bool create_if_missing) = 0;

    // Process-wide POSIX environment. Never null, never destroyed (avoids
    // static destruction order issues).
    [[nodiscard]] static Env* Default();
};

// ==============================================================================
// PosixFile
// ==============================================================================
//
// Sole owner of a file descriptor; move-only. A moved-from object is closed
// (fd_ == -1) and its destructor is a no-op.
//
// Concurrency: pread/pwrite make individual calls safe from multiple threads,
// but overlapping ranges are not ordered. Page-level exclusion is the buffer
// pool's responsibility.

class PosixFile final : public File {
public:
    // Closed placeholder; also the moved-from state.
    PosixFile() = default;

    ~PosixFile() override;

    // Opens O_RDWR. Without create_if_missing, a missing path yields NotFound.
    [[nodiscard]] static FileResult<PosixFile> Open(std::string_view path, 
                                                     bool create_if_missing);

    // Reads exactly `size` bytes at `offset`; a short read is an error.
    [[nodiscard]] FileStatus Read(uint64_t offset, void* buf,
                                  size_t size) const override;

    // Writes exactly `size` bytes at `offset`. Not durable until Sync().
    [[nodiscard]] FileStatus Write(uint64_t offset, const void* buf,
                                   size_t size) override;

    // fdatasync: flushes data and the metadata needed to read it back (e.g.
    // file size), skipping timestamps. This is the durability barrier for the
    // WAL and is the dominant latency on the write path, so callers batch
    // writes ahead of it.
    [[nodiscard]] FileStatus Sync() override;

    // Non-blocking exclusive flock on the whole file; Busy if held, including
    // by another open in this process. Enforces single-writer access to a
    // database directory.
    //
    // flock locks are tied to the open file description: released on close, and
    // by the kernel on process death, so a crash cannot leave a stale lock.
    // Advisory only; it does not stop non-cooperating writers.
    [[nodiscard]] FileStatus TryLockExclusive() override;

    [[nodiscard]] FileResult<uint64_t> Size() const override;

    // Shrinking discards the tail; growing zero-fills.
    [[nodiscard]] FileStatus Truncate(uint64_t size) override;

    [[nodiscard]] bool IsOpen() const { return fd_ >= 0; }

    [[nodiscard]] std::string_view path() const override { return path_; }

    // Move-only: a descriptor has exactly one owner.
    PosixFile(const PosixFile&) = delete;
    PosixFile& operator=(const PosixFile&) = delete;

    PosixFile(PosixFile&& other) noexcept;
    PosixFile& operator=(PosixFile&& other) noexcept;

private:
    // Reached only via Open().
    PosixFile(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}

    int fd_ = -1;
    std::string path_;
};

}  // namespace kvstore::storage
