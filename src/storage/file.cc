// ==============================================================================
// kvstore/storage/file.cc - POSIX File Implementation (C++23)
// ==============================================================================
//
// This file implements the PosixFile class using POSIX system calls.
//
// C++23 Features Used:
// --------------------
//   - std::expected<T, E>   : Return success value OR error (no exceptions)
//   - std::format()         : Type-safe string formatting (replaces concatenation)
//   - std::string_view      : Efficient string parameter passing
//   - std::unexpected       : Create error values for std::expected
//
// System Calls Used:
// ------------------
//   open()      - Open or create a file, returns file descriptor
//   close()     - Close a file descriptor
//   pread()     - Read at offset without changing file position
//   pwrite()    - Write at offset without changing file position
//   fdatasync() - Flush data (not metadata) to disk
//   fstat()     - Get file information (we use it for file size)
//   ftruncate() - Change file size
//
// Error Handling:
// ---------------
// All POSIX calls return -1 on error and set 'errno'. We convert errno to
// a human-readable message using strerror() and wrap it in std::expected.
//
// ==============================================================================

#include "kvstore/storage/file.h"

#include <utility>      // move & std::exchange
#include <fcntl.h>      // open(), O_RDWR, O_CREAT, etc.
#include <unistd.h>     // close(), pread(), pwrite(), fdatasync(), ftruncate()
#include <sys/stat.h>   // fstat(), struct stat
#include <cerrno>       // errno
#include <cstring>      // strerror()
#include <format>       // std::format (C++23)

namespace kvstore::storage {

// ==============================================================================
// Helper Function
// ==============================================================================

// Create an IoError with the current errno using std::format
//
// std::format is C++23's type-safe string formatting. It's like printf but:
//   - Type-safe: wrong types cause compile errors, not runtime bugs
//   - Extensible: can format custom types
//   - Cleaner syntax: {} placeholders instead of %s, %d, etc.
//
// Example:
//   std::format("Value is {} and name is {}", 42, "test")
//   // Returns: "Value is 42 and name is test"

[[nodiscard]] static std::unexpected<FileError> IoErrorFromErrno(
    std::string_view context, 
    std::string_view path
) {
    // strerror() converts errno to a human-readable message like:
    //   2  -> "No such file or directory"
    //   13 -> "Permission denied"
    //   28 -> "No space left on device"
    
    // std::format replaces ugly string concatenation:
    // OLD: context + " '" + path + "': " + strerror(errno)
    // NEW: std::format("{} '{}': {}", context, path, strerror(errno))
    
    return std::unexpected(
        FileError::IoError(std::format("{} '{}': {}", context, path, strerror(errno)))
    );
}

// ==============================================================================
// Destructor
// ==============================================================================

PosixFile::~PosixFile() {
    // Only close if we have a valid file descriptor
    // After move, fd_ will be -1, so this is safe
    if (fd_ >= 0) {
        ::close(fd_);
        fd_= -1;
    }
}

// ==============================================================================
// Move Operations
// ==============================================================================

PosixFile::PosixFile(PosixFile&& other) noexcept
    : fd_(std::exchange(other.fd_, -1))
    , path_(std::move(other.path_)) 
{
    // Constructor body can be empty - all initialization in member initializer list
}

PosixFile& PosixFile::operator=(PosixFile&& other) noexcept {
    // Handle self-assignment: (file = std::move(file))
    if (this != &other) {
        // Close our current file (if any) before taking ownership of other's
        if (fd_ >= 0){
            ::close(fd_);
        }
        
        // Take ownership
        fd_ = std::exchange(other.fd_, -1);
        path_ = std::move(other.path_);
        
    }
    return *this;
}

// ==============================================================================
// Open
// ==============================================================================

FileResult<PosixFile> PosixFile::Open(std::string_view path, bool create_if_missing) {
    // -------------------------------------------------------------------------
    // Build flags for open()
    // -------------------------------------------------------------------------
    // O_RDWR: Open for both reading and writing
    // This is necessary because we'll both read pages and write pages
    int flags = O_RDWR;

    if (create_if_missing) {
        // O_CREAT: Create file if it doesn't exist
        // Requires a mode argument (permissions) - see below
        flags |= O_CREAT;
    }

    // -------------------------------------------------------------------------
    // Open the file
    // -------------------------------------------------------------------------
    // mode 0644 = rw-r--r-- (owner can read/write, others can only read)
    // This is only used when O_CREAT is set and file doesn't exist
    //
    // Note: We need to convert string_view to C-string for POSIX
    // std::string_view doesn't guarantee null-termination, so we create
    // a std::string to get a proper c_str()
    std::string path_str(path);
    int fd = ::open(path_str.c_str(), flags, 0644);
    
    if (fd < 0) {
        // open() failed - check errno for the reason
        if (errno == ENOENT && !create_if_missing) {
            // File doesn't exist and we weren't asked to create it
            return std::unexpected(
                FileError::NotFound(std::format("File not found: {}", path))
            );
        }
        return IoErrorFromErrno("Failed to open", path);
    }

    // -------------------------------------------------------------------------
    // Return the wrapped file descriptor
    // -------------------------------------------------------------------------
    // We use the private constructor here
    // std::expected automatically wraps the success value
    return PosixFile(fd, std::move(path_str));
}

// ==============================================================================
// Read
// ==============================================================================

FileStatus PosixFile::Read(uint64_t offset, void* buf, size_t size) const {
    // -------------------------------------------------------------------------
    // Validate state
    // -------------------------------------------------------------------------
    if (fd_ < 0) {
        return std::unexpected(FileError::Internal("Read called on closed file"));
    }

    // -------------------------------------------------------------------------
    // Use pread() for positioned read
    // -------------------------------------------------------------------------
    // pread(fd, buf, count, offset) reads 'count' bytes starting at 'offset'
    // 
    // Why pread instead of lseek + read?
    //   1. Atomic: Position and read happen together
    //   2. Thread-safe: Doesn't modify shared file position
    //   3. Simpler: One syscall instead of two
    //
    // pread() returns:
    //   - Number of bytes read (may be less than requested at EOF)
    //   - -1 on error (errno is set)
    //   - 0 at end of file
    
    ssize_t bytes_read = ::pread(fd_, buf, size, static_cast<off_t>(offset));
    
    if (bytes_read < 0) {
        return IoErrorFromErrno("Read failed", path_);
    }
    
    // -------------------------------------------------------------------------
    // Verify we read the expected amount
    // -------------------------------------------------------------------------
    // For database pages, we always want to read exactly 'size' bytes.
    // A short read means we hit EOF, which shouldn't happen with valid pages.
    if (static_cast<size_t>(bytes_read) != size) {
        return std::unexpected(FileError::IoError(
            std::format("Short read: expected {} bytes, got {} from '{}'",
                        size, bytes_read, path_)
        ));
    }
    
    // Success! For std::expected<void, E>, we return {} to indicate success
    return {};
}

// ==============================================================================
// Write
// ==============================================================================

FileStatus PosixFile::Write(uint64_t offset, const void* buf, size_t size) {
    // -------------------------------------------------------------------------
    // Validate state
    // -------------------------------------------------------------------------
    if (fd_ < 0) {
        return std::unexpected(FileError::Internal("Write called on closed file"));
    }

    // -------------------------------------------------------------------------
    // Use pwrite() for positioned write
    // -------------------------------------------------------------------------
    // pwrite(fd, buf, count, offset) writes 'count' bytes at 'offset'
    //
    // pwrite() returns:
    //   - Number of bytes written (may be less if disk is full)
    //   - -1 on error
    //
    // Note: pwrite() extends the file if writing past current EOF
    
    ssize_t bytes_written = ::pwrite(fd_, buf, size, static_cast<off_t>(offset));
    
    if (bytes_written < 0) {
        return IoErrorFromErrno("Write failed", path_);
    }
    
    // -------------------------------------------------------------------------
    // Verify we wrote the expected amount
    // -------------------------------------------------------------------------
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

    // -------------------------------------------------------------------------
    // Use fdatasync() to flush data to disk
    // -------------------------------------------------------------------------
    // fdatasync() vs fsync():
    //   - fsync(): Flushes data AND metadata (size, timestamps, etc.)
    //   - fdatasync(): Flushes data and only metadata needed for data integrity
    //
    // fdatasync() is slightly faster because it skips metadata updates that
    // don't affect data integrity (like access time).
    //
    // For our use case:
    //   - File size changes are flushed (needed to read data back)
    //   - Access/modification times may not be flushed (we don't care)
    
    if (::fdatasync(fd_) < 0) {
        return IoErrorFromErrno("Sync failed", path_);
    }
    
    return {};
}

// ==============================================================================
// Size
// ==============================================================================

// C++23 improvement: Return the value directly instead of output parameter!
//
// OLD API: Status Size(uint64_t& out) const;
//          uint64_t size;
//          if (!file.Size(size).ok()) { ... }
//
// NEW API: FileResult<uint64_t> Size() const;
//          auto size = file.Size();
//          if (!size) { ... }
//          use *size or size.value()

FileResult<uint64_t> PosixFile::Size() const {
    if (fd_ < 0) {
        return std::unexpected(FileError::Internal("Size called on closed file"));
    }

    // -------------------------------------------------------------------------
    // Use fstat() to get file information
    // -------------------------------------------------------------------------
    // fstat() fills a 'struct stat' with file metadata including:
    //   - st_size: File size in bytes
    //   - st_mode: File type and permissions
    //   - st_mtime: Last modification time
    //   - etc.
    
    struct stat file_stat;
    if (::fstat(fd_, &file_stat) < 0) {
        return IoErrorFromErrno("fstat failed", path_);
    }
    
    // Return the size directly - much cleaner than output parameters!
    return static_cast<uint64_t>(file_stat.st_size);
}

// ==============================================================================
// Truncate
// ==============================================================================

FileStatus PosixFile::Truncate(uint64_t size) {
    if (fd_ < 0) {
        return std::unexpected(FileError::Internal("Truncate called on closed file"));
    }

    // -------------------------------------------------------------------------
    // Use ftruncate() to change file size
    // -------------------------------------------------------------------------
    // ftruncate() can both shrink and grow a file:
    //   - Shrinking: Data beyond new size is lost
    //   - Growing: New bytes are filled with zeros (sparse file on most FSes)
    
    if (::ftruncate(fd_, static_cast<off_t>(size)) < 0) {
        return IoErrorFromErrno("Truncate failed", path_);
    }
    
    return {};
}

}  // namespace kvstore::storage
