// ==============================================================================
// kvstore/db.h - Public Database API (C++23)
// ==============================================================================
//
// This is the main public interface for the key-value store. Users only need
// to include this header to use the database.
//
// Features:
// ---------
//   - Simple Get/Put/Delete API
//   - Automatic file management
//   - Persistence to disk
//   - RAII-based resource management
//
// Usage:
// ------
//   #include "kvstore/db.h"
//
//   int main() {
//       // Open or create a database
//       kvstore::Options options;
//       options.create_if_missing = true;
//
//       auto result = kvstore::DB::Open("/path/to/mydb", options);
//       if (!result.ok()) {
//           std::cerr << "Failed to open: " << result.status().message() << "\n";
//           return 1;
//       }
//
//       auto db = std::move(result.value());
//
//       // Store data
//       db->Put("name", "Alice");
//       db->Put("age", "30");
//
//       // Retrieve data
//       auto value = db->Get("name");
//       if (value.ok()) {
//           std::cout << "Name: " << value.value() << "\n";
//       }
//
//       // Delete data
//       db->Delete("age");
//
//       // Database is automatically closed when 'db' goes out of scope
//       return 0;
//   }
//
// Thread Safety:
// --------------
// The current implementation is NOT thread-safe. Only use from a single thread,
// or add external synchronization.
//
// C++23 Features Used:
// --------------------
//   - [[nodiscard("reason")]] with explanatory messages
//   - constexpr default member initializers
//   - Improved aggregate initialization
//
// ==============================================================================

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "kvstore/status.h"

namespace kvstore {

// ==============================================================================
// Options
// ==============================================================================
//
// Configuration options for opening a database.

struct Options {
    // If true, create the database if it doesn't exist.
    // If false, return an error if the database doesn't exist.
    bool create_if_missing = false;
    
    // If true, call Sync() after every write operation.
    // This is slower but ensures durability - data survives crashes.
    // If false, data may be lost on crash but writes are faster.
    bool sync_on_write = true;
    
    // C++23: Default comparison operator
    [[nodiscard]]
    constexpr bool operator==(const Options&) const noexcept = default;
};

// ==============================================================================
// DB Class
// ==============================================================================
//
// The main database interface. Use DB::Open() to create an instance.
//
// This is an abstract base class. The actual implementation is in DbImpl.
// This separation allows:
//   1. Clean public API (users only see DB)
//   2. Implementation details hidden
//   3. Easier testing with mock implementations

class DB {
public:
    // -------------------------------------------------------------------------
    // Factory Method
    // -------------------------------------------------------------------------
    
    // Open a database at the specified path.
    //
    // Parameters:
    //   path    - Directory path for database files
    //   options - Configuration options
    //
    // Returns:
    //   On success: unique_ptr to the opened database
    //   On failure: Status with error details
    //
    // The path should be a directory. The database will create files inside:
    //   - data.db  : Main B+ tree data file
    //
    // Example:
    //   auto result = DB::Open("/var/lib/myapp/db", options);
    //   if (result.ok()) {
    //       auto db = std::move(result.value());
    //       // use db...
    //   }
    [[nodiscard("Open returns the database handle - ignoring it loses the connection")]]
    static Result<std::unique_ptr<DB>> Open(const std::string& path, 
                                             const Options& options = Options{});
    
    // -------------------------------------------------------------------------
    // Destructor
    // -------------------------------------------------------------------------
    
    // Virtual destructor for proper cleanup of derived classes
    virtual ~DB() = default;
    
    // -------------------------------------------------------------------------
    // Core Operations
    // -------------------------------------------------------------------------
    
    // Get the value for a key.
    //
    // Parameters:
    //   key - The key to look up
    //
    // Returns:
    //   On success: The value associated with the key
    //   On not found: Status::NotFound
    //   On error: Status with error details
    //
    // Example:
    //   auto result = db->Get("user:123");
    //   if (result.ok()) {
    //       std::cout << result.value() << "\n";
    //   } else if (result.status().code() == StatusCode::kNotFound) {
    //       std::cout << "Key not found\n";
    //   }
    [[nodiscard("Get returns the value - ignoring it wastes the lookup")]]
    virtual Result<std::string> Get(std::string_view key) = 0;
    
    // Store a key-value pair.
    //
    // Parameters:
    //   key   - The key to store
    //   value - The value to associate with the key
    //
    // Returns:
    //   Status::Ok() on success
    //   Status with error details on failure
    //
    // If the key already exists, its value is replaced.
    //
    // Example:
    //   Status s = db->Put("user:123", "{\"name\": \"Alice\"}");
    //   if (!s.ok()) {
    //       std::cerr << "Put failed: " << s.message() << "\n";
    //   }
    [[nodiscard("Put returns status - check for write errors")]]
    virtual Status Put(std::string_view key, std::string_view value) = 0;
    
    // Delete a key-value pair.
    //
    // Parameters:
    //   key - The key to delete
    //
    // Returns:
    //   Status::Ok() on success (even if key didn't exist)
    //   Status with error details on failure
    //
    // Note: In this MVP, Delete is not implemented and returns NotFound.
    // A full implementation would mark the key as deleted in the B+ tree.
    [[nodiscard("Delete returns status - check for errors")]]
    virtual Status Delete(std::string_view key) = 0;
    
    // Check if a key exists.
    //
    // Parameters:
    //   key - The key to check
    //
    // Returns:
    //   true if key exists, false otherwise
    [[nodiscard("Contains returns existence status")]]
    virtual bool Contains(std::string_view key) = 0;
    
    // -------------------------------------------------------------------------
    // Durability
    // -------------------------------------------------------------------------
    
    // Force all buffered writes to disk.
    //
    // This ensures that all Put() operations completed before this call
    // will survive a system crash.
    //
    // If sync_on_write is true in Options, this is called automatically
    // after each Put().
    //
    // Returns:
    //   Status::Ok() on success
    //   Status::IoError() on failure
    [[nodiscard("Sync returns status - check for I/O errors")]]
    virtual Status Sync() = 0;
    
    // -------------------------------------------------------------------------
    // Statistics (Optional)
    // -------------------------------------------------------------------------
    
    // Get approximate number of keys in the database.
    // This may not be exact if there are deleted keys.
    [[nodiscard("ApproximateKeyCount returns the count")]]
    virtual uint64_t ApproximateKeyCount() const = 0;
    
    // Get the database file path
    [[nodiscard("GetPath returns the database path")]]
    virtual const std::string& GetPath() const = 0;

protected:
    // Protected constructor - use Open() to create instances
    DB() = default;
    
    // Disable copying (C++23 style with explicit delete)
    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;
    
    // Enable moving (default)
    DB(DB&&) noexcept = default;
    DB& operator=(DB&&) noexcept = default;
};

}  // namespace kvstore
