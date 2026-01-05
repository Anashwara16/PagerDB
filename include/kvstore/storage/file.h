// ==============================================================================
// kvstore/storage/file.h - POSIX File Abstraction (C++23)
// ==============================================================================
//
// This file provides a C++ wrapper around POSIX file I/O operations.
// It's the lowest layer of our storage stack.
//
// C++23 Features Used:
// --------------------
//   - std::expected<T, E>  : Modern error handling (replaces custom Result<T>)
//   - std::string_view     : Non-owning string references for parameters
//   - [[nodiscard]]        : Compiler warnings for ignored return values
//   - std::format          : Type-safe string formatting
//
// Why Wrap POSIX I/O?
// -------------------
// Raw POSIX I/O (open, read, write, close) has several issues:
//   1. Uses raw integers for file descriptors (-1 means error)
//   2. Manual resource management (forgetting close() leaks FDs)
//   3. Error codes are global (errno)
//   4. No type safety (can pass any int as a file descriptor)
//
// Our PosixFile class provides:
//   1. RAII - file is closed automatically when object is destroyed
//   2. Type safety - can't confuse file descriptors with other ints
//   3. Consistent error handling via std::expected
//   4. Move semantics for efficient transfers
//
// Key I/O Operations:
// -------------------
// We use positioned I/O (pread/pwrite) instead of read/write because:
//   - Thread-safe: doesn't modify shared file offset
//   - Atomic: position and data transfer in one syscall
//   - Clear semantics: caller specifies exactly where to read/write
//
// File Descriptor Lifecycle:
// --------------------------
//   auto file = PosixFile::Open(path, true).value();  // Acquires FD
//   file.Write(0, data, size);                        // Uses FD
//   // ... more operations ...
//   // FD automatically closed when 'file' goes out of scope (destructor)
//
// ==============================================================================

#pragma once

#include <cstddef>      // size_t
#include <cstdint>      // uint64_t
#include <expected>     // std::expected (C++23)
#include <string>
#include <string_view>  // std::string_view
#include <system_error> // std::error_code

namespace kvstore::storage {

// ==============================================================================
// Error Types (C++23 style using std::expected)
// ==============================================================================
//
// std::expected<T, E> is C++23's way of returning "either a value or an error"
// It's similar to Rust's Result<T, E> or your previous custom Result<T>
//
// Key differences from your old Result<T>:
//   - Standard library type (no custom code needed)
//   - .value() returns the success value (throws if error)
//   - .error() returns the error (undefined if success)
//   - .has_value() or operator bool() checks for success
//   - .value_or(default) returns value or default if error

// Error information for file operations
struct FileError {
    enum class Kind {
        NotFound,       // File doesn't exist
        IoError,        // General I/O error
        InternalError   // Programming error (e.g., operation on closed file)
    };

    Kind kind;
    std::string message;

    // Factory methods for creating errors (like your old Status class)
    [[nodiscard]] static FileError NotFound(std::string_view msg) {
        return {Kind::NotFound, std::string(msg)};
    }

    [[nodiscard]] static FileError IoError(std::string_view msg) {
        return {Kind::IoError, std::string(msg)};
    }

    [[nodiscard]] static FileError Internal(std::string_view msg) {
        return {Kind::InternalError, std::string(msg)};
    }
};

// Type aliases for cleaner function signatures
template <typename T>
using FileResult = std::expected<T, FileError>;

// For operations that don't return a value, we use std::expected<void, FileError>
// This is cleaner than returning a status object
using FileStatus = std::expected<void, FileError>;

// ==============================================================================
// PosixFile Class
// ==============================================================================
//
// RAII wrapper around a POSIX file descriptor.
//
// Thread Safety:
// --------------
// Individual operations (Read, Write) are thread-safe due to using pread/pwrite.
// However, concurrent reads and writes to overlapping regions may race.
// Higher layers (BufferPool) are responsible for proper synchronization.
//
// Move Semantics:
// ---------------
// PosixFile is movable but not copyable. Moving transfers ownership of the
// file descriptor:
//
//   auto a = PosixFile::Open(...).value();
//   auto b = std::move(a);  // b now owns the FD, a is empty
//   // a is now in "moved-from" state (fd_ == -1)
//   // Destroying a does nothing
//   // Destroying b closes the file

class PosixFile {
public:
    // -------------------------------------------------------------------------
    // Construction & Destruction
    // -------------------------------------------------------------------------

    // Default constructor - creates an invalid (empty) file object
    // This is useful for:
    //   - Declaring a variable before knowing if open will succeed
    //   - Move-from state
    PosixFile() = default;

    // Destructor - closes the file if open
    // This is the "R" (Resource release) in RAII
    ~PosixFile();

    // -------------------------------------------------------------------------
    // Static Factory Method
    // -------------------------------------------------------------------------

    // Open a file at the given path.
    //
    // Parameters:
    //   path              - Filesystem path to the file
    //   create_if_missing - If true, create the file if it doesn't exist
    //                       If false, fail with NotFound if file doesn't exist
    //
    // Returns:
    //   On success: expected containing PosixFile with open file descriptor
    //   On failure: expected containing FileError with error details
    //
    // Example (C++23 style):
    //   auto result = PosixFile::Open("/tmp/mydb.dat", true);
    //   if (!result) {
    //       std::println(stderr, "Failed: {}", result.error().message);
    //       return;
    //   }
    //   auto file = std::move(*result);  // or result.value()
    //
    // The file is opened in read-write mode (O_RDWR).
    [[nodiscard]] static FileResult<PosixFile> Open(std::string_view path, 
                                                     bool create_if_missing);

    // -------------------------------------------------------------------------
    // I/O Operations
    // -------------------------------------------------------------------------

    // Read data from file at specified offset.
    //
    // Parameters:
    //   offset - Byte position in file to start reading from
    //   buf    - Buffer to read data into (must be at least 'size' bytes)
    //   size   - Number of bytes to read
    //
    // Returns:
    //   std::expected<void, FileError>:
    //     - Has value (success) if exactly 'size' bytes were read
    //     - Has error if read failed or returned fewer bytes
    //
    // Note: Uses pread() which doesn't change the file's seek position.
    //       This is important for thread safety.
    [[nodiscard]] FileStatus Read(uint64_t offset, void* buf, size_t size) const;

    // Write data to file at specified offset.
    //
    // Parameters:
    //   offset - Byte position in file to start writing to
    //   buf    - Buffer containing data to write
    //   size   - Number of bytes to write
    //
    // Returns:
    //   std::expected<void, FileError>:
    //     - Has value (success) if exactly 'size' bytes were written
    //     - Has error if write failed
    //
    // Note: Uses pwrite() which doesn't change the file's seek position.
    //       Data may be buffered by the OS - call Sync() to force to disk.
    [[nodiscard]] FileStatus Write(uint64_t offset, const void* buf, size_t size);

    // Force buffered data to disk.
    //
    // Why this matters:
    //   When you call Write(), data goes to OS buffers, not directly to disk.
    //   If the system crashes before the OS flushes buffers, data is lost.
    //   Sync() forces the OS to write all pending data to the physical disk.
    //
    // Performance note:
    //   Sync() is SLOW (can take milliseconds). Use sparingly.
    //   The WAL (Write-Ahead Log) will handle durability guarantees.
    //
    // Implementation uses fdatasync() which is slightly faster than fsync()
    // because it doesn't update file metadata (modification time, etc.)
    // unless necessary for data integrity.
    [[nodiscard]] FileStatus Sync();

    // Get the current size of the file in bytes.
    //
    // Returns:
    //   std::expected<uint64_t, FileError>:
    //     - Has value: the file size in bytes
    //     - Has error: if the operation failed
    //
    // C++23 improvement: Returns the value directly instead of using
    // an output parameter. Much cleaner API!
    [[nodiscard]] FileResult<uint64_t> Size() const;

    // Truncate or extend the file to the specified size.
    //
    // If size < current size: file is truncated, data beyond 'size' is lost
    // If size > current size: file is extended, new bytes are zero
    //
    // Parameters:
    //   size - New size in bytes
    //
    // Returns:
    //   std::expected<void, FileError>:
    //     - Has value (success) on success
    //     - Has error on failure
    [[nodiscard]] FileStatus Truncate(uint64_t size);

    // -------------------------------------------------------------------------
    // State Queries
    // -------------------------------------------------------------------------

    // Check if this object holds a valid file descriptor
    [[nodiscard]] bool IsOpen() const { return fd_ >= 0; }

    // Get the file path (for debugging/logging)
    [[nodiscard]] std::string_view path() const { return path_; }

    // -------------------------------------------------------------------------
    // Move Operations
    // -------------------------------------------------------------------------
    // Files are movable but not copyable (each FD should have exactly one owner)

    // Disable copy operations
    PosixFile(const PosixFile&) = delete;
    PosixFile& operator=(const PosixFile&) = delete;

    // Enable move operations
    PosixFile(PosixFile&& other) noexcept;
    PosixFile& operator=(PosixFile&& other) noexcept;

private:
    // Private constructor - use Open() factory method
    PosixFile(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}

    // The file descriptor (-1 if not open)
    int fd_ = -1;

    // Path to the file (for error messages and debugging)
    std::string path_;
};

}  // namespace kvstore::storage
