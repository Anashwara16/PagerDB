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
// Thread Safety (Phase 5):
// ------------------------
// A DB handle is safe to share across threads: Get/Put/Delete/Scan/Begin/
// Sync/Checkpoint may all be called concurrently (latch crabbing in the
// tree, per-frame latches in the buffer pool, an internally locked WAL
// writer, and a writers-shared/checkpoint-exclusive latch taken per
// operation - a gated acquisition keeps sustained write traffic from
// starving the checkpointer, and an idle open transaction holds no latch,
// so it never blocks a checkpoint). A single Transaction object is NOT
// thread-safe - use each transaction from one thread at a time (different
// transactions on different threads are the intended pattern).
//
// C++23 Features Used:
// --------------------
//   - [[nodiscard("reason")]] with explanatory messages
//   - constexpr default member initializers
//   - Improved aggregate initialization
//
// ==============================================================================

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "kvstore/status.h"
#include "kvstore/types.h"

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

    // If true, verify page CRC32 checksums on every read (and the meta page
    // checksum on open). Detects on-disk corruption at the cost of a CRC pass
    // per page read.
    bool verify_checksums = true;

    // Buffer pool capacity in 4KB frames (Phase 3). 0 means "default": the
    // KVSTORE_POOL_FRAMES environment variable if set (test hook for the
    // pool-size regression gate), otherwise 64. Values below 16 are clamped
    // to 16: a splitting Put pins its whole root-to-leaf path plus every
    // page the operation rewrote until its WAL op-group is complete (up to
    // 2*depth+2 frames; 16 covers depth 7).
    size_t buffer_pool_frames = 0;

    // Phase 4: checkpoint automatically once the WAL grows past this many
    // bytes (bounds recovery time and disk usage). 0 disables the automatic
    // trigger; Checkpoint() can always be called manually. Checkpoints also
    // run on clean close.
    uint64_t max_wal_bytes = 4 * 1024 * 1024;

    // C++23: Default comparison operator
    [[nodiscard]]
    constexpr bool operator==(const Options&) const noexcept = default;
};

// ==============================================================================
// Transaction (Phase 5.3)
// ==============================================================================
//
// A multi-statement transaction obtained from DB::Begin(). Reads and writes go
// through the transaction; nothing it writes is durable until Commit(), and
// Abort() rolls back every write it made.
//
//   auto txn = std::move(db->Begin().value());
//   txn->Put("a", "1");
//   txn->Put("b", "2");
//   if (ok) txn->Commit(); else txn->Abort();
//
// A single transaction object is NOT thread-safe: use it from one thread at a
// time (different transactions on different threads are fine, and are the
// point). The plain DB::Get/Put/Delete calls are autocommit - each wraps a
// one-operation transaction internally.
//
// Isolation (Phase 5.5) - stated precisely, because "strict 2PL" alone
// overpromises. The exact guarantee is:
//
//   - Explicit transactions are serializable AGAINST EACH OTHER, for POINT
//     operations (Get/Put/Delete on individual keys) ONLY. Each takes per-key
//     S/X locks held to Commit/Abort (strict 2PL), so between transactions
//     there are no dirty reads, lost updates, or write-write races on the
//     keys they touch.
//   - No phantom protection. Locks cover key names, not ranges or
//     predicates; nothing stops another transaction from inserting a key
//     "between" the ones this transaction read.
//   - Scan is READ-UNCOMMITTED, always. It takes no key locks (it walks the
//     tree under page latches only), so it can observe another transaction's
//     uncommitted writes - or a rollback in progress.
//   - Autocommit Get/Put/Delete take no key locks either. Each is a single
//     atomic, durable operation, but it ignores transaction boundaries: an
//     autocommit Get is read-uncommitted, and an autocommit Put/Delete can
//     clobber a key an explicit transaction holds X-locked. Worse, if that
//     transaction later aborts, its rollback restores the before-image and
//     silently erases the committed autocommit write. Do not mix autocommit
//     writes and explicit transactions on the same keys.
//
// Deadlocks between transactions are avoided with wait-die: an operation may
// return Status::Busy, which means THIS transaction must Abort() and the
// caller should retry it from the start. Retry with Begin(txn->id()) - it
// re-runs under the aborted attempt's age, so a victim eventually becomes
// the oldest contender and waits instead of dying again (starvation-freedom;
// a plain Begin() would restart it ever-younger).
//
//   for (;;) {
//       auto txn = std::move(db->Begin(retry_id).value());
//       Status s = DoWork(*txn);                  // May return Busy
//       if (s.ok() && (s = txn->Commit()).ok()) break;
//       retry_id = txn->id();                     // Keep our age
//       (void)txn->Abort();
//   }
//
// Destroying an active transaction aborts it.

class Transaction {
public:
    virtual ~Transaction() = default;

    // Read a key (sees this transaction's own uncommitted writes).
    [[nodiscard("Get returns the value")]]
    virtual Result<std::string> Get(std::string_view key) = 0;

    // Buffer a write in this transaction (durable only after Commit).
    [[nodiscard("Put returns status - check for errors")]]
    virtual Status Put(std::string_view key, std::string_view value) = 0;

    [[nodiscard("Delete returns status - check for errors")]]
    virtual Status Delete(std::string_view key) = 0;

    // Make every write durable and end the transaction. After Commit the
    // transaction object is spent (further calls return an error).
    [[nodiscard("Commit returns status - check for errors")]]
    virtual Status Commit() = 0;

    // Roll back every write and end the transaction.
    [[nodiscard("Abort returns status - check for errors")]]
    virtual Status Abort() = 0;

    // This transaction's wait-die age. Pass it to DB::Begin(retry_of) when
    // retrying after a Busy abort so the retry keeps the original priority
    // instead of restarting ever-younger (see the class comment). Callable
    // on a spent (committed/aborted) transaction.
    [[nodiscard("id returns the age to pass to Begin on retry")]]
    virtual TxId id() const = 0;

protected:
    Transaction() = default;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
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
    //     - Status::Busy if another process has the database open (at most
    //       one process may hold a database directory at a time; the flock
    //       on LOCK is released automatically even if the holder crashes)
    //
    // The path should be a directory. The database will create files inside:
    //   - data.db  : Main B+ tree data file (dual meta pages + tree)
    //   - wal.log  : Write-ahead log (replayed on open after a crash)
    //   - LOCK     : Zero-byte file carrying the single-process flock
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
    //
    // These are AUTOCOMMIT: each call is a single atomic, durable operation.
    // They take no key locks, so they ignore explicit transaction boundaries
    // entirely - reads are read-uncommitted, and a write can land on a key an
    // open transaction holds X-locked (whose later rollback would then erase
    // it). Keep autocommit traffic and explicit transactions on disjoint keys;
    // see the Transaction class comment for the precise isolation statement.
    
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
    //   Status::InvalidArgument if key+value cannot fit in one page (~4 KB) -
    //     there are no overflow pages (documented limitation, Phase 7
    //     candidate). Reliable limit: keep key+value under ~1 KB. Larger
    //     records (up to the page bound) usually work but can fail mid-split
    //     when a page packs unluckily, and that failure poisons the handle
    //     until reopen.
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
    //   Status::Ok() if the key was found and removed
    //   Status::NotFound() if the key did not exist (nothing changed)
    //   Status with error details on failure
    //
    // Successful deletes honor Options::sync_on_write just like Put.
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
    // Range Scans (Phase 2)
    // -------------------------------------------------------------------------

    // Callback for Scan. The views are only valid during the call - copy
    // them out to keep them. Return false to stop the scan early.
    using ScanCallback =
        std::function<bool(std::string_view key, std::string_view value)>;

    // Visit all key-value pairs in the half-open range [start, end) in
    // sorted key order.
    //
    // Isolation: READ-UNCOMMITTED. A scan takes no key locks - each visited
    // entry is consistent (page latches), but the scan as a whole can observe
    // another transaction's uncommitted writes or a rollback in progress, and
    // concurrent writers may change entries ahead of or behind the cursor
    // mid-scan. See the Transaction class comment for the full isolation
    // statement.
    //
    // Parameters:
    //   start    - First key to include (empty = from the smallest key)
    //   end      - Exclusive upper bound (empty = to the largest key)
    //   callback - Invoked per entry; return false to stop early
    //
    // Returns:
    //   Status::Ok() when the scan completes or the callback stops it
    //   Status with error details if a page read fails mid-scan
    [[nodiscard("Scan returns status - check for I/O errors")]]
    virtual Status Scan(std::string_view start, std::string_view end,
                        const ScanCallback& callback) = 0;

    // -------------------------------------------------------------------------
    // Transactions (Phase 5.3)
    // -------------------------------------------------------------------------

    // Begin a multi-statement transaction. Returns a handle whose Get/Put/
    // Delete operate transactionally until Commit() or Abort(); see the
    // Transaction class. The plain Get/Put/Delete above are autocommit.
    //
    // `retry_of` (default kInvalidTxId = none): the id() of a transaction
    // that was just aborted after a wait-die Busy. The new transaction runs
    // under that original age, so a retried victim eventually becomes the
    // oldest contender and stops losing (wait-die's starvation-freedom
    // depends on this). It still logs under a fresh internal txid - reusing
    // the old one would corrupt recovery's loser detection.
    [[nodiscard("Begin returns the transaction handle")]]
    virtual Result<std::unique_ptr<Transaction>> Begin(
        TxId retry_of = kInvalidTxId) = 0;

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
    // Phase 4: durability is provided by the write-ahead log - Sync flushes
    // the WAL, which is sufficient for every acknowledged operation to
    // survive a crash (recovery replays the log on the next Open).
    //
    // Returns:
    //   Status::Ok() on success
    //   Status::IoError() on failure
    [[nodiscard("Sync returns status - check for I/O errors")]]
    virtual Status Sync() = 0;

    // Phase 4: flush all dirty pages and the meta, then truncate the WAL.
    // Bounds recovery time; runs automatically when the WAL exceeds
    // Options::max_wal_bytes and on clean close.
    //
    // Fuzzy with respect to transactions (Phase 5): an open transaction never
    // blocks a checkpoint - pages flush regardless - but while a transaction
    // that has written is in flight, the WAL is NOT truncated (it still holds
    // the records a crash-recovery rollback of that transaction would need).
    // Truncation resumes with the first checkpoint after the last such
    // transaction ends, so log growth is bounded by transaction lifetime:
    // keep transactions short.
    [[nodiscard("Checkpoint returns status - check for I/O errors")]]
    virtual Status Checkpoint() = 0;
    
    // -------------------------------------------------------------------------
    // Statistics (Optional)
    // -------------------------------------------------------------------------
    
    // Get approximate number of keys in the database.
    // This may not be exact if there are deleted keys.
    [[nodiscard("ApproximateKeyCount returns the count")]]
    virtual uint64_t ApproximateKeyCount() const = 0;

    // Buffer pool cache statistics (Phase 3).
    struct Stats {
        uint64_t pool_hits = 0;
        uint64_t pool_misses = 0;
        uint64_t pool_evictions = 0;
    };

    [[nodiscard("GetStats returns cache counters")]]
    virtual Stats GetStats() const = 0;
    
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
