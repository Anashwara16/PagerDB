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
    static constexpr uint32_t kFormatVersion = 1;
    
    uint32_t magic;           // Must be kMagicNumber
    uint32_t version;         // Format version
    PageId   root_page_id;    // Root of the B+ tree (kInvalidPageId if empty)
    PageId   num_pages;       // Total pages in file (including meta page)
    uint64_t num_keys;        // Total key-value pairs (for stats)
    
    // Reserved space for future use
    uint8_t  reserved[4096 - 24];  // Pad to page size
    
    // Initialize a new meta page
    constexpr void Init() noexcept {
        magic = kMagicNumber;
        version = kFormatVersion;
        root_page_id = kInvalidPageId;
        num_pages = 1;  // Just the meta page itself
        num_keys = 0;
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
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------
    
    // Default constructor - creates invalid pager
    Pager() = default;
    
    // Open or create a database file
    //
    // Parameters:
    //   path             - Path to the database file
    //   create_if_missing - If true, create new database if file doesn't exist
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
    //   - Validates magic number and version
    //   - Loads meta page into memory
    [[nodiscard("Open returns the Pager - ignoring it loses the file handle")]]
    static Result<Pager> Open(const std::string& path, bool create_if_missing);
    
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
    [[nodiscard("ReadPage returns status - check for I/O errors")]]
    Status ReadPage(PageId page_id, Page& out) const;
    
    // Write a page to disk
    //
    // Parameters:
    //   page_id - Which page to write (must be < num_pages)
    //   page    - Page data to write
    //
    // Returns:
    //   Status::Ok() on success
    //   Status::InvalidArgument() if page_id is out of range
    //   Status::IoError() if write fails
    //
    // Note: This does NOT update the page header's page_id field.
    //       The caller should set that before calling WritePage.
    [[nodiscard("WritePage returns status - check for I/O errors")]]
    Status WritePage(PageId page_id, const Page& page);
    
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
    [[nodiscard("AllocatePage returns the new page ID")]]
    Result<PageId> AllocatePage();
    
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
    
    [[nodiscard("SetNumKeys returns status - check for errors")]]
    Status SetNumKeys(uint64_t count);
    
    // -------------------------------------------------------------------------
    // Durability
    // -------------------------------------------------------------------------
    
    // Flush all pending writes to disk
    //
    // Call this after a batch of writes to ensure durability.
    // Without Sync(), data may be lost on crash.
    [[nodiscard("Sync returns status - check for I/O errors")]]
    Status Sync();
    
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
    Pager(PosixFile file, MetaPage meta);
    
    // Write the current meta page to disk
    [[nodiscard("WriteMetaPage returns status")]]
    Status WriteMetaPage();
    
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
};

}  // namespace kvstore::storage
