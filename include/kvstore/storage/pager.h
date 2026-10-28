// ==============================================================================
// kvstore/storage/pager.h - Page-Level File I/O (C++23)
// ==============================================================================
//
// The Pager sits between the B+ tree and raw file I/O. It provides:
//   1. Page-level read/write operations (instead of byte offsets)
//   2. Page allocation and tracking
//   3. Meta page management (database metadata)
//
// Architecture:
// -------------
//   B+ Tree
//      ↓
//   Pager (this file)  ← Translates PageId to file offset
//      ↓
//   PosixFile          ← Raw byte I/O
//      ↓
//   Operating System
//
// File Layout:
// ------------
//   ┌─────────────┬─────────────┬─────────────┬─────────────┐
//   │   Page 0    │   Page 1    │   Page 2    │   Page 3    │ ...
//   │  (Meta)     │  (Data)     │  (Data)     │  (Data)     │
//   └─────────────┴─────────────┴─────────────┴─────────────┘
//   Offset: 0        4096          8192          12288
//
//   Page 0 is special - it stores database metadata (root page ID, page count)
//   Pages 1+ are data pages (B+ tree nodes)
//
// MVP Simplifications:
// --------------------
//   - No buffer pool (pages read/written directly to disk)
//   - No free list (deleted pages are not reused)
//   - Single-threaded access assumed
//
// C++23 Features Used:
// --------------------
//   - [[nodiscard("reason")]] with explanatory messages
//   - Deducing this for const/non-const accessors
//   - constexpr/consteval where applicable
//   - std::unreachable() for impossible paths
//
// ==============================================================================

#pragma once

#include <string>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

#include "kvstore/status.h"
#include "kvstore/types.h"
#include "kvstore/storage/file.h"
#include "kvstore/storage/page_format.h"

namespace kvstore::storage {

// ==============================================================================
// Meta Page Structure
// ==============================================================================
//
// The meta page (Page 0) stores persistent database state.
// This is loaded when opening the database and updated when:
//   - New pages are allocated
//   - The root page changes (after a root split)
//
// Layout: Stored at the beginning of the meta page's payload area

struct MetaPage {
    // Magic number to identify valid database files
    // "KVST" in ASCII = 0x4B565354
    static constexpr uint32_t kMagicNumber = 0x4B565354;
    
    // Current format version (increment if format changes)
    // v2 (Phase 1): PageHeader.reserved split into rightmost_child/dead_bytes,
    //               MetaPage gains free_list_head + checksum, checksums enforced.
    // v3 (Phase 4): dual meta pages (0 and 1) with a generation counter,
    //               checkpoint_lsn for WAL recovery, data pages start at 2.
    static constexpr uint32_t kFormatVersion = 3;

    uint32_t magic;           // Must be kMagicNumber
    uint32_t version;         // Format version
    PageId   root_page_id;    // Root of the B+ tree (kInvalidPageId if empty)
    PageId   num_pages;       // Total pages in file (including meta page)
    uint64_t num_keys;        // Total key-value pairs (for stats)
    PageId   free_list_head;  // Head of freed-page list (kInvalidPageId if empty)
    uint32_t checksum;        // CRC32 of the meta page (checksum field as zero)
    uint64_t generation;      // v3: bumped on every persist; higher wins at open
    Lsn      checkpoint_lsn;  // v3: WAL replay starts after this LSN

    // Reserved space for future use
    uint8_t  reserved[4096 - 48];  // Pad to page size

    // Initialize a new meta page
    constexpr void Init() noexcept {
        magic = kMagicNumber;
        version = kFormatVersion;
        root_page_id = kInvalidPageId;
        num_pages = 2;  // The two meta copies (pages 0 and 1)
        num_keys = 0;
        free_list_head = kInvalidPageId;
        checksum = 0;
        generation = 0;
        checkpoint_lsn = kInvalidLsn;
        // Note: In constexpr context, we can't use memset
        // The reserved array will be zero-initialized by default
        for (auto& byte : reserved) {
            byte = 0;
        }
    }
    
    // Validate the meta page
    [[nodiscard("IsValid returns validation status")]]
    constexpr bool IsValid() const noexcept {
        return magic == kMagicNumber && version == kFormatVersion;
    }
    
    // C++23: Default comparison for testing
    [[nodiscard]]
    constexpr bool operator==(const MetaPage&) const noexcept = default;
};

static_assert(sizeof(MetaPage) == kPageSize, "MetaPage must be exactly one page");

// ==============================================================================
// Pager Class
// ==============================================================================
//
// Usage:
//   // Open or create a database file
//   auto result = Pager::Open("/path/to/db.dat", true);
//   if (!result.ok()) { handle error }
//   Pager pager = std::move(result.value());
//
//   // Allocate a new page
//   auto alloc_result = pager.AllocatePage();
//   PageId new_page = alloc_result.value();
//
//   // Read a page
//   Page page;
//   pager.ReadPage(new_page, page);
//
//   // Modify and write back
//   // ... modify page ...
//   pager.WritePage(new_page, page);
//
//   // Ensure durability
//   pager.Sync();

class Pager {
public:
    // v3: meta copies live at pages 0 and 1; data pages start here.
    static constexpr PageId kFirstDataPage = 2;

    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------
    
    // Default constructor - creates invalid pager
    Pager() = default;
    
    // Open or create a database file
    //
    // Parameters:
    //   path              - Path to the database file
    //   create_if_missing - If true, create new database if file doesn't exist
    //   verify_checksums  - If true (default), verify page/meta CRC32 on read
    //
    // Returns:
    //   On success: Pager ready for use
    //   On failure: Status with error details
    //
    // If creating a new database:
    //   - Initializes meta page with default values
    //   - root_page_id is set to kInvalidPageId (empty tree)
    //
    // If opening existing database:
    //   - Validates magic number, version, file size, and meta checksum
    //   - Loads meta page into memory
    [[nodiscard("Open returns the Pager - ignoring it loses the file handle")]]
    static Result<Pager> Open(const std::string& path, bool create_if_missing,
                              bool verify_checksums = true);
    
    // -------------------------------------------------------------------------
    // Page I/O
    // -------------------------------------------------------------------------
    
    // Read a page from disk
    //
    // Parameters:
    //   page_id - Which page to read (must be < num_pages)
    //   out     - Page object to fill with data
    //
    // Returns:
    //   Status::Ok() on success
    //   Status::InvalidArgument() if page_id is out of range
    //   Status::IoError() if read fails
    //   Status::Corruption() if the checksum does not match (when verifying;
    //                        all-zero pages are exempt - they are allocated
    //                        but never written)
    [[nodiscard("ReadPage returns status - check for I/O errors")]]
    Status ReadPage(PageId page_id, Page& out) const;

    // Write a page to disk
    //
    // Parameters:
    //   page_id - Which page to write (must be < num_pages)
    //   page    - Page data to write; its header checksum is stamped in place
    //             before writing (hence non-const)
    //
    // Returns:
    //   Status::Ok() on success
    //   Status::InvalidArgument() if page_id is out of range
    //   Status::IoError() if write fails
    //
    // Note: This does NOT update the page header's page_id field.
    //       The caller should set that before calling WritePage.
    [[nodiscard("WritePage returns status - check for I/O errors")]]
    Status WritePage(PageId page_id, Page& page);
    
    // -------------------------------------------------------------------------
    // Page Allocation
    // -------------------------------------------------------------------------
    
    // Allocate a new page
    //
    // Returns:
    //   On success: The PageId of the newly allocated page
    //   On failure: Status with error details
    //
    // The new page is:
    //   - Zeroed out
    //   - NOT initialized (caller must set up header and content)
    //   - Written to disk to extend the file
    //
    // This also updates the meta page on disk.
    //
    // Phase 2: if the free list is non-empty, the most recently freed page
    // is recycled (LIFO) instead of extending the file. Recycled pages are
    // re-zeroed on disk first, so they read back like fresh allocations.
    [[nodiscard("AllocatePage returns the new page ID")]]
    Result<PageId> AllocatePage();

    // Return a page to the free list (Phase 2, task 2.6).
    //
    // The page is rewritten as type kFree with the previous free-list head
    // in its first 4 payload bytes (little-endian), and becomes the new head.
    // The meta page is updated on disk.
    //
    // Returns:
    //   Status::InvalidArgument() for the meta page (id 0) or out-of-range ids
    //   Status::IoError() if a write fails
    [[nodiscard("FreePage returns status - check for errors")]]
    Status FreePage(PageId page_id);
    
    // -------------------------------------------------------------------------
    // Meta Page Access (using deducing this where beneficial)
    // -------------------------------------------------------------------------
    
    // Get the root page ID of the B+ tree
    [[nodiscard("root_page_id returns the B+ tree root")]]
    constexpr PageId root_page_id(this auto&& self) noexcept { 
        return self.meta_.root_page_id; 
    }
    
    // Set the root page ID (e.g., after a root split)
    // This updates the in-memory meta and writes it to disk
    [[nodiscard("SetRootPageId returns status - check for errors")]]
    Status SetRootPageId(PageId new_root);
    
    // Get total number of pages in the file
    [[nodiscard("num_pages returns the page count")]]
    constexpr PageId num_pages(this auto&& self) noexcept { 
        return self.meta_.num_pages; 
    }
    
    // Get/set key count (for statistics)
    [[nodiscard("num_keys returns the key count")]]
    constexpr uint64_t num_keys(this auto&& self) noexcept {
        return self.meta_.num_keys;
    }

    // v3: meta generation counter (bumped on every PersistMeta)
    [[nodiscard("generation returns the meta generation")]]
    constexpr uint64_t generation(this auto&& self) noexcept {
        return self.meta_.generation;
    }

    // v3: WAL replay starts after this LSN
    [[nodiscard("checkpoint_lsn returns the recovery start point")]]
    constexpr Lsn checkpoint_lsn(this auto&& self) noexcept {
        return self.meta_.checkpoint_lsn;
    }

    // In-memory only, like the other meta setters; persist via Sync() or
    // PersistMeta() (a checkpoint does this).
    [[nodiscard("SetCheckpointLsn returns status")]]
    Status SetCheckpointLsn(Lsn lsn);

    // Head of the freed-page list (kInvalidPageId when empty; consumed in Phase 2)
    [[nodiscard("free_list_head returns the free list head")]]
    constexpr PageId free_list_head(this auto&& self) noexcept {
        return self.meta_.free_list_head;
    }
    
    [[nodiscard("SetNumKeys returns status - check for errors")]]
    Status SetNumKeys(uint64_t count);

    // Phase 5: atomic key-count adjustment. Concurrent tree writers bump the
    // stat from different leaves after releasing the root latch, so the plain
    // read-modify-write on meta_.num_keys would race. delta is +1 per fresh
    // insert, -1 per delete. In memory only (persisted at Sync/checkpoint).
    [[nodiscard("AddToNumKeys returns status - check for errors")]]
    Status AddToNumKeys(int64_t delta);

    // Synchronized read of the key count for callers that may run alongside
    // concurrent writers (DB::ApproximateKeyCount). The plain num_keys()
    // accessor stays for single-threaded paths (recovery, open).
    [[nodiscard("KeyCount returns the current key count")]]
    uint64_t KeyCount() const;

    // -------------------------------------------------------------------------
    // Durability
    // -------------------------------------------------------------------------
    
    // Persist the in-memory meta (v3: setters no longer write it) and flush
    // all pending writes to disk. Without Sync(), meta changes and data may
    // be lost on crash - the WAL (Phase 4) covers that gap between syncs.
    [[nodiscard("Sync returns status - check for I/O errors")]]
    Status Sync();

    // fsync the data file WITHOUT persisting the meta. Checkpoints need this
    // ordering: data pages must be durable before the meta that references
    // them advances.
    [[nodiscard("SyncData returns status - check for I/O errors")]]
    Status SyncData();

    // Write the in-memory meta to disk (v3): bumps the generation and writes
    // to page (generation % 2), alternating between the two copies so a torn
    // meta write can never destroy the last good state.
    [[nodiscard("PersistMeta returns status - check for I/O errors")]]
    Status PersistMeta();
    
    // -------------------------------------------------------------------------
    // Move Operations
    // -------------------------------------------------------------------------
    
    Pager(Pager&& other) noexcept;
    Pager& operator=(Pager&& other) noexcept;
    
    // Disable copying
    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

private:
    // Private constructor - use Open() factory method
    Pager(PosixFile file, MetaPage meta, bool verify_checksums);
    
    // Calculate file offset for a given page ID
    [[nodiscard("PageOffset returns byte offset")]]
    static constexpr uint64_t PageOffset(PageId id) noexcept {
        return static_cast<uint64_t>(id) * kPageSize;
    }
    
    // The underlying file
    PosixFile file_;
    
    // In-memory copy of the meta page
    // Updated on allocation and root changes
    MetaPage meta_{};

    // Whether ReadPage / Open verify CRC32 checksums (Options::verify_checksums)
    bool verify_checksums_ = true;

    // Phase 5: guards the key-count read-modify-write against concurrent tree
    // writers, and the meta snapshot PersistMeta takes. unique_ptr keeps Pager
    // movable (std::mutex is not). Null on a default-constructed (invalid)
    // Pager; Open() always allocates it. num_pages / free_list_head stay
    // serialized by the buffer pool latch (allocation goes through NewPage);
    // root_page_id stays serialized by the pool root latch.
    std::unique_ptr<std::mutex> meta_mu_ = std::make_unique<std::mutex>();
};

}  // namespace kvstore::storage
