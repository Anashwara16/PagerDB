// ==============================================================================
// kvstore/storage/file.cc - POSIX File Implementation (C++23)
// ==============================================================================
// PosixFile: RAII wrapper over a POSIX file descriptor exposing positioned
// I/O (pread/pwrite) for the page-oriented storage layer.
//
// Design notes:
//   - Errors are returned as std::expected<T, FileError>; the I/O path does not
//     throw. errno is captured at the failure site (see IoErrorFromErrno).
//   - Transfers must complete in full: short reads/writes are reported as
//     IoError rather than retried, since callers operate on fixed-size page
//     buffers. A failed Write() may still have modified part of the range.
//   - Positioned I/O never touches the shared file offset, so concurrent
//     Read() calls on one PosixFile require no external locking.
//
// ==============================================================================

#include "kvstore/storage/file.h"

#include <utility>      // std::exchange, std::move
#include <fcntl.h>      // open(), O_RDWR, O_CREAT, etc.
#include <unistd.h>     // close(), pread(), pwrite(), fdatasync(), ftruncate()
#include <sys/file.h>   // flock(), LOCK_EX, LOCK_NB
#include <sys/stat.h>   // fstat(), struct stat
#include <cerrno>       // errno
#include <cstring>      // strerror()
#include <format>       // std::format (C++23)

namespace kvstore::storage {

// ==============================================================================
// Default Env
// ==============================================================================

namespace {

class PosixEnv final : public Env {
public:
    FileResult<std::unique_ptr<File>> OpenFile(std::string_view path,
                                               bool create_if_missing) override {
        auto file = PosixFile::Open(path, create_if_missing);
        if (!file) [[unlikely]] {
            return std::unexpected(std::move(file.error()));
        }
        return std::make_unique<PosixFile>(std::move(*file));
    }
};

}  // namespace

Env* Env::Default() {
    // Function-local static: thread-safe init, and intentionally leaked so it
    // outlives any static DB handle destroyed at exit.
    static Env* const env = new PosixEnv();
    return env;
}

// ==============================================================================
// Helper Function
// ==============================================================================

// Builds an IoError from the current errno; call before anything that can
// clobber it. strerror() is not required to be thread-safe, which is tolerated
// because the message is diagnostic only and never parsed.

[[nodiscard]] static std::unexpected<FileError> IoErrorFromErrno(
    std::string_view context, 
    std::string_view path
) {
    return std::unexpected(
        FileError::IoError(std::format("{} '{}': {}", context, path, strerror(errno)))
    );
}

// ==============================================================================
// Destructor
// ==============================================================================

PosixFile::~PosixFile() {
    // close() errors are ignored; durability is established by Sync(), not close().
    // fd_ == -1 denotes a moved-from or already-closed file.
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

// ==============================================================================
// Move Operations
// ==============================================================================

PosixFile::PosixFile(PosixFile&& other) noexcept
    : fd_(std::exchange(other.fd_, -1))
    , path_(std::move(other.path_)) 
{
}

PosixFile& PosixFile::operator=(PosixFile&& other) noexcept {
    // Self-move guard: the close below would otherwise discard the descriptor.
    if (this != &other) {
        if (fd_ >= 0){
            ::close(fd_);
        }
        
        fd_ = std::exchange(other.fd_, -1);
        path_ = std::move(other.path_);
        
    }
    return *this;
}

// ==============================================================================
// Open
// ==============================================================================

FileResult<PosixFile> PosixFile::Open(std::string_view path, bool create_if_missing) {
    // O_RDWR: a single descriptor serves both page reads and writes.
    // O_CLOEXEC: keep the descriptor, and the flock() held through it, from
    // leaking into exec'd children.
    int flags = O_RDWR | O_CLOEXEC;

    if (create_if_missing) {
        flags |= O_CREAT;
    }

    // 0644 applies only when O_CREAT creates the file. open() needs a
    // NUL-terminated path, so copy into a std::string (also kept as path_ for
    // diagnostics).
    std::string path_str(path);
    int fd = ::open(path_str.c_str(), flags, 0644);
    
    if (fd < 0) {
        // ENOENT without O_CREAT is a distinct, caller-actionable condition; with
        // O_CREAT it means a missing parent directory and is reported as IoError.
        if (errno == ENOENT && !create_if_missing) {
            return std::unexpected(
                FileError::NotFound(std::format("File not found: {}", path))
            );
        }
        return IoErrorFromErrno("Failed to open", path);
    }

    // Ownership of fd transfers to the returned PosixFile.
    return PosixFile(fd, std::move(path_str));
}

// ==============================================================================
// Read
// ==============================================================================

FileStatus PosixFile::Read(uint64_t offset, void* buf, size_t size) const {
    if (fd_ < 0) {
        return std::unexpected(FileError::Internal("Read called on closed file"));
    }

    ssize_t bytes_read = ::pread(fd_, buf, size, static_cast<off_t>(offset));
    
    if (bytes_read < 0) {
        return IoErrorFromErrno("Read failed", path_);
    }
    
    // Callers always request whole pages, so a short read means EOF inside an
    // expected page (truncation or corruption). Treated as an error, not retried.
    if (static_cast<size_t>(bytes_read) != size) {
        return std::unexpected(FileError::IoError(
            std::format("Short read: expected {} bytes, got {} from '{}'",
                        size, bytes_read, path_)
        ));
    }
    
    return {};
}

// ==============================================================================
// Write
// ==============================================================================

FileStatus PosixFile::Write(uint64_t offset, const void* buf, size_t size) {
    if (fd_ < 0) {
        return std::unexpected(FileError::Internal("Write called on closed file"));
    }

    // pwrite() past EOF extends the file; any gap reads back as zeros.

    ssize_t bytes_written = ::pwrite(fd_, buf, size, static_cast<off_t>(offset));
    
    if (bytes_written < 0) {
        return IoErrorFromErrno("Write failed", path_);
    }
    
    // Short writes (e.g. ENOSPC) are surfaced as errors, not retried; the target
    // range is then in an undefined state and the caller must treat it as failed.
    if (static_cast<size_t>(bytes_written) != size) {
        return std::unexpected(FileError::IoError(
            std::format("Short write: expected {} bytes, wrote {} to '{}'",
                        size, bytes_written, path_)
        ));
    }
    
    return {};
}

// ==============================================================================
// Sync
// ==============================================================================

FileStatus PosixFile::Sync() {
    if (fd_ < 0) {
        return std::unexpected(FileError::Internal("Sync called on closed file"));
    }

    // fdatasync() skips metadata not needed to read the data back (timestamps)
    // but still persists size changes, avoiding the extra journal commit fsync()
    // may incur.

    // macOS: fsync() only reaches the drive's volatile write cache, so this path
    // is not power-loss durable. TODO(phase-4): use fcntl(F_FULLFSYNC).
#if defined(__APPLE__)
    if (::fsync(fd_) < 0) {
        return IoErrorFromErrno("Sync failed", path_);
    }
#else
    if (::fdatasync(fd_) < 0) {
        return IoErrorFromErrno("Sync failed", path_);
    }
#endif

    return {};
}

// ==============================================================================
// TryLockExclusive
// ==============================================================================

FileStatus PosixFile::TryLockExclusive() {
    if (fd_ < 0) {
        return std::unexpected(
            FileError::Internal("TryLockExclusive called on closed file"));
    }
    // Advisory, non-blocking lock guarding against multiple processes opening the
    // same database. flock() locks are per open file description, so a second
    // open() of the same path conflicts even within one process.
    if (::flock(fd_, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) {
            return std::unexpected(FileError::Busy(std::format(
                "'{}' is locked by another process", path_)));
        }
        return IoErrorFromErrno("flock failed", path_);
    }
    // No explicit unlock: the lock dies with this open file description
    // (Close/destructor), or with the process.
    return {};
}

// ==============================================================================
// Size
// ==============================================================================

// Logical size (st_size); includes sparse holes.

FileResult<uint64_t> PosixFile::Size() const {
    if (fd_ < 0) {
        return std::unexpected(FileError::Internal("Size called on closed file"));
    }

    struct stat file_stat;
    if (::fstat(fd_, &file_stat) < 0) {
        return IoErrorFromErrno("fstat failed", path_);
    }
    
    return static_cast<uint64_t>(file_stat.st_size);
}

// ==============================================================================
// Truncate
// ==============================================================================

FileStatus PosixFile::Truncate(uint64_t size) {
    if (fd_ < 0) {
        return std::unexpected(FileError::Internal("Truncate called on closed file"));
    }

    // Grow zero-fills (sparse on most filesystems); shrink discards the tail.
    // Neither is durable until Sync().
    
    if (::ftruncate(fd_, static_cast<off_t>(size)) < 0) {
        return IoErrorFromErrno("Truncate failed", path_);
    }
    
    return {};
}

}  // namespace kvstore::storage
