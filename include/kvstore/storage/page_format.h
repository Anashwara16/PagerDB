// ==============================================================================
// kvstore/storage/page_format.h - On-Disk Page Layout (C++23 Version)
// ==============================================================================
//
// This file defines the structure of pages stored on disk. Understanding this
// is fundamental to how the database stores data.
//
// C++23 Features Used:
// ====================
//   - std::expected<T, E>     : Modern error handling (replaces custom Result<T>)
//   - Deducing this           : Eliminates const/non-const method duplication
//   - [[assume(expr)]]        : Optimization hints to the compiler
//   - std::to_underlying      : Safe enum-to-integer conversion
//   - std::unreachable()      : Marks impossible code paths
//
// Page Overview:
// ==============
// Our database file is divided into fixed-size pages (4KB each). This size is
// chosen because:
//   - 4KB matches typical filesystem block size and OS page size
//   - Aligns well with SSD page sizes (4KB-16KB)
//   - Small enough to not waste space, large enough for efficient I/O
//   - Powers of 2 make offset calculations simple
//
// Page Layout (Slotted Page Design):
// ==================================
// We use the "slotted page" layout, which efficiently stores variable-length
// records while allowing in-place updates and deletions.
//
//   ┌─────────────────────────────────────────────────────────────┐
//   │                     PAGE HEADER                             │
//   │  (PageId, type, LSN, num_slots, free_offset, checksums...)  │
//   ├─────────────────────────────────────────────────────────────┤
//   │ Slot 0 │ Slot 1 │ Slot 2 │  ... unused slot space ...       │
//   │ (8B)   │ (8B)   │ (8B)   │                                  │
//   ├─────────────────────────────────────────────────────────────┤
//   │                                                             │
//   │                      FREE SPACE                             │
//   │                                                             │
//   ├─────────────────────────────────────────────────────────────┤
//   │  ... unused data space ...  │ Record 2 │ Record 1 │ Record 0│
//   └─────────────────────────────────────────────────────────────┘
//   byte 0                                                   byte 4095
//
// Key observations:
//   - Header is at the start (fixed size)
//   - Slot directory grows forward (toward higher addresses)
//   - Record data grows backward (toward lower addresses)
//   - Free space is in the middle
//   - When slot directory meets record data, the page is full
//
// Why Slotted Pages?
// ==================
//   1. Variable-length records: Keys and values can be any size
//   2. Stable pointers: External references use (PageId, SlotId) pairs
//      - If we compact the page, we update slots but slot IDs stay the same
//   3. Easy deletion: Just mark slot as deleted, optionally compact later
//   4. Efficient iteration: Walk the slot directory
//
// ==============================================================================

#pragma once

#include <cstdint>
#include <cstring>       // memset, memcpy
#include <cassert>
#include <string_view>
#include <expected>      // C++23: std::expected for error handling
#include <utility>       // C++23: std::to_underlying, std::unreachable

#include "kvstore/types.h"
#include "kvstore/status.h"

namespace kvstore::storage {

// Bring types from parent namespace
using kvstore::PageId;
using kvstore::SlotId;
using kvstore::Lsn;
using kvstore::kInvalidPageId;
using kvstore::kInvalidLsn;

// ==============================================================================
// Constants
// ==============================================================================

// Page size in bytes - 4KB is the standard choice
// All pages in the database are exactly this size
inline constexpr uint32_t kPageSize = 4096;

// ==============================================================================
// Page Types
// ==============================================================================
//
// Different page types have different internal layouts, though they all share
// the same header structure.

enum class PageType : uint8_t {
    // Meta page: Contains database metadata (root page ID, page count, etc.)
    // There's exactly one meta page, always at PageId 0
    kMeta = 0,
    
    // Internal B+ tree node: Contains keys and child page pointers
    // Does NOT contain values - only guides the search
    kInternal = 1,
    
    // Leaf B+ tree node: Contains actual key-value pairs
    // This is where user data lives
    kLeaf = 2,
    
    // Free page: Currently unused, available for allocation
    // Free pages are linked together in a free list
    kFree = 3,
    
    // Overflow page: For values too large to fit in a leaf page
    // (Advanced feature - implement later if needed)
    kOverflow = 4,
};

// ==============================================================================
// C++23 Helper: Convert PageType to its underlying integer value
// ==============================================================================
// std::to_underlying is safer than static_cast because it preserves the
// enum's declared underlying type (uint8_t in this case).
//
// Usage: auto value = to_underlying(PageType::kLeaf);  // returns uint8_t{2}

[[nodiscard]] constexpr auto to_page_type_value(PageType type) noexcept {
    return std::to_underlying(type);
}

// ==============================================================================
// Page Header
// ==============================================================================
//
// Every page starts with this header. The header contains metadata needed
// to interpret the rest of the page.
//
// Layout (40 bytes total):
//   Offset  Size  Field
//   ------  ----  -----
//   0       4     page_id         - This page's identifier
//   4       1     type            - PageType enum value
//   5       1     flags           - Bit flags (reserved for future use)
//   6       2     num_slots       - Number of slots in slot directory
//   8       8     page_lsn        - LSN of last modification
//   16      2     free_space_start - Offset where free space begins (after slots)
//   18      2     free_space_end   - Offset where free space ends (before data)
//   20      4     next_page       - Linked list pointer (for leaf pages, free list)
//   24      4     prev_page       - Linked list pointer (for leaf pages)
//   28      4     checksum        - CRC32 of page contents (excluding checksum)
//   32      8     reserved        - Padding for alignment / future use
//
// Total: 40 bytes

struct PageHeader {
    PageId   page_id;           // This page's ID (redundant but useful for validation)
    PageType type;              // What kind of page is this?
    uint8_t  flags;             // Reserved flags
    uint16_t num_slots;         // Number of slots in the slot directory
    Lsn      page_lsn;          // Log Sequence Number of last modification
    uint16_t free_space_start;  // Byte offset where free space begins
    uint16_t free_space_end;    // Byte offset where free space ends
    PageId   next_page;         // For leaf pages: next leaf in key order
    PageId   prev_page;         // For leaf pages: previous leaf in key order
    uint32_t checksum;          // CRC32 checksum for corruption detection
    uint64_t reserved;          // Padding / future use

    // Initialize a new page header
    constexpr void Init(PageId id, PageType t) noexcept {
        page_id = id;
        type = t;
        flags = 0;
        num_slots = 0;
        page_lsn = kInvalidLsn;
        // Free space starts right after header and extends to end of page
        free_space_start = sizeof(PageHeader);
        free_space_end = kPageSize;
        next_page = kInvalidPageId;
        prev_page = kInvalidPageId;
        checksum = 0;
        reserved = 0;
    }
    
    // Calculate available free space in bytes
    [[nodiscard]] constexpr uint16_t FreeSpace() const noexcept {
        if (free_space_end <= free_space_start) {
            return 0;
        }
        return free_space_end - free_space_start;
    }
};

// Verify header size at compile time
static_assert(sizeof(PageHeader) == 40, "PageHeader must be exactly 40 bytes");

// ==============================================================================
// Slot Entry
// ==============================================================================
//
// Each slot in the slot directory describes one record stored in the page.
// Slots are stored immediately after the header, growing toward higher addresses.
//
// Layout (8 bytes):
//   Offset  Size  Field
//   ------  ----  -----
//   0       2     offset   - Byte offset of record data (from start of page)
//   2       2     length   - Length of record data in bytes
//   4       2     flags    - Status flags (e.g., deleted, tombstone)
//   6       2     reserved - Padding / future use
//
// A "deleted" slot has flags set but offset/length preserved (for compaction).
// After compaction, deleted slots can be reused.

struct SlotEntry {
    uint16_t offset;    // Offset of record from page start
    uint16_t length;    // Length of record in bytes
    uint16_t flags;     // Status flags
    uint16_t reserved;  // Future use

    // Flag constants
    static constexpr uint16_t kFlagDeleted = 0x0001;  // Slot is deleted
    
    // Initialize a new slot
    constexpr void Init(uint16_t off, uint16_t len) noexcept {
        offset = off;
        length = len;
        flags = 0;
        reserved = 0;
    }
    
    // Check if this slot is in use (not deleted)
    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return (flags & kFlagDeleted) == 0;
    }
    
    // Mark this slot as deleted
    constexpr void MarkDeleted() noexcept {
        flags |= kFlagDeleted;
    }
};

// Verify slot entry size at compile time
static_assert(sizeof(SlotEntry) == 8, "SlotEntry must be exactly 8 bytes");

// ==============================================================================
// Page Class
// ==============================================================================
//
// The Page class wraps a raw byte buffer and provides a nice interface for
// working with slotted pages.
//
// Important: Page is exactly kPageSize bytes. No extra memory overhead.
// This means:
//   - We can read/write pages directly to/from disk
//   - When we write to disk, we write from Page::data_
//
// Usage:
//   Page page;
//   page.Init(42, PageType::kLeaf);
//   
//   // Insert a record
//   auto result = page.InsertRecord("hello", 5);
//   if (result) {
//       SlotId slot = *result;  // Use the slot ID
//   }
//
//   // Read it back
//   auto record_view = page.GetRecordView(slot);

class Page {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------
    
    // Default constructor - creates an uninitialized page
    // You must call Init() before using
    Page() = default;
    
    // Initialize this page with given ID and type
    void Init(PageId id, PageType type) {
        // Zero the entire page first
        std::memset(data_, 0, kPageSize);
        
        // Initialize the header
        header()->Init(id, type);
    }
    
    // -------------------------------------------------------------------------
    // Header Access (C++23: Deducing This)
    // -------------------------------------------------------------------------
    //
    // C++23 "Deducing this" feature eliminates the need for duplicate
    // const/non-const overloads. The compiler automatically deduces
    // the const-ness based on how the method is called.
    //
    // OLD C++20 way (required TWO methods):
    //     PageHeader* header() { return reinterpret_cast<PageHeader*>(data_); }
    //     const PageHeader* header() const { return reinterpret_cast<const PageHeader*>(data_); }
    //
    // NEW C++23 way (ONE method handles both):
    //     auto header(this auto&& self) { ... }
    //
    // The 'this auto&& self' parameter captures the object with its
    // const-ness, so calling on a const Page returns const PageHeader*.
    
    [[nodiscard]] auto header(this auto&& self) noexcept
        -> decltype(auto)
    {
        using SelfType = std::remove_reference_t<decltype(self)>;
        if constexpr (std::is_const_v<SelfType>) {
            return reinterpret_cast<const PageHeader*>(self.data_);
        } else {
            return reinterpret_cast<PageHeader*>(self.data_);
        }
    }
    
    // Convenience accessors
    [[nodiscard]] PageId page_id() const noexcept { return header()->page_id; }
    [[nodiscard]] PageType type() const noexcept { return header()->type; }
    [[nodiscard]] uint16_t num_slots() const noexcept { return header()->num_slots; }
    [[nodiscard]] Lsn page_lsn() const noexcept { return header()->page_lsn; }
    
    // -------------------------------------------------------------------------
    // Slot Directory Access (C++23: Deducing This)
    // -------------------------------------------------------------------------
    
    // Get pointer to the slot directory
    // Slots start immediately after the header
    [[nodiscard]] auto slots(this auto&& self) noexcept
        -> decltype(auto)
    {
        using SelfType = std::remove_reference_t<decltype(self)>;
        if constexpr (std::is_const_v<SelfType>) {
            return reinterpret_cast<const SlotEntry*>(self.data_ + sizeof(PageHeader));
        } else {
            return reinterpret_cast<SlotEntry*>(self.data_ + sizeof(PageHeader));
        }
    }
    
    // Get a specific slot (C++23: Deducing This)
    [[nodiscard]] auto slot(this auto&& self, SlotId index) noexcept
        -> decltype(auto)
    {
        // C++23: [[assume]] tells the compiler this condition is always true
        // This enables better optimizations (no bounds-check code generated)
        [[assume(index < self.header()->num_slots)]];
        
        return self.slots()[index];
    }
    
    // -------------------------------------------------------------------------
    // Record Access (C++23: Deducing This)
    // -------------------------------------------------------------------------
    
    // Get a pointer to record data
    // Returns nullptr if slot is invalid or deleted
    [[nodiscard]] auto GetRecordPtr(this auto&& self, SlotId slot_id) noexcept
        -> decltype(auto)
    {
        using SelfType = std::remove_reference_t<decltype(self)>;
        using ReturnType = std::conditional_t<
            std::is_const_v<SelfType>,
            const uint8_t*,
            uint8_t*
        >;
        
        if (slot_id >= self.header()->num_slots) {
            return static_cast<ReturnType>(nullptr);
        }
        
        const auto& s = self.slot(slot_id);
        if (!s.IsValid()) {
            return static_cast<ReturnType>(nullptr);
        }
        
        if constexpr (std::is_const_v<SelfType>) {
            return static_cast<ReturnType>(self.data_ + s.offset);
        } else {
            return static_cast<ReturnType>(self.data_ + s.offset);
        }
    }
    
    // Get record length
    [[nodiscard]] uint16_t GetRecordLength(SlotId slot_id) const noexcept {
        if (slot_id >= header()->num_slots) {
            return 0;
        }
        const SlotEntry& s = slot(slot_id);
        if (!s.IsValid()) {
            return 0;
        }
        return s.length;
    }
    
    // Get record as string_view (for convenience)
    [[nodiscard]] std::string_view GetRecordView(SlotId slot_id) const noexcept {
        const uint8_t* ptr = GetRecordPtr(slot_id);
        if (!ptr) {
            return {};
        }
        return {reinterpret_cast<const char*>(ptr), GetRecordLength(slot_id)};
    }
    
    // -------------------------------------------------------------------------
    // Space Calculations
    // -------------------------------------------------------------------------
    
    // How much free space is available?
    [[nodiscard]] uint16_t FreeSpace() const noexcept {
        return header()->FreeSpace();
    }
    
    // Can we fit a record of this size?
    // Need space for: slot entry (8 bytes) + record data
    [[nodiscard]] bool CanFit(uint16_t record_size) const noexcept {
        uint16_t needed = sizeof(SlotEntry) + record_size;
        return FreeSpace() >= needed;
    }
    
    // -------------------------------------------------------------------------
    // Record Manipulation (C++23: std::expected)
    // -------------------------------------------------------------------------
    //
    // C++23 introduces std::expected<T, E>, which is like Rust's Result<T, E>.
    // It holds EITHER a success value (T) OR an error (E), but never both.
    //
    // Benefits over custom Result types:
    //   - Standardized: Everyone uses the same type
    //   - Monadic operations: .and_then(), .or_else(), .transform()
    //   - Well-defined semantics
    //
    // Usage:
    //   std::expected<SlotId, Status> result = page.InsertRecord(data, len);
    //   if (result) {
    //       SlotId slot = *result;  // or result.value()
    //   } else {
    //       Status error = result.error();
    //   }
    
    // Insert a new record
    // Returns the slot ID where the record was stored, or error status
    //
    // How it works:
    //   1. Check if there's enough space
    //   2. Allocate a slot (at free_space_start, growing forward)
    //   3. Allocate record space (at free_space_end, growing backward)
    //   4. Copy data and update header
    [[nodiscard]] std::expected<SlotId, Status> InsertRecord(
        const void* data, 
        uint16_t length
    ) {
        // Check space
        if (!CanFit(length)) {
            // C++23: std::unexpected wraps the error value
            return std::unexpected(Status::NoSpace("Page full"));
        }
        
        // Allocate slot
        SlotId new_slot_id = header()->num_slots;
        
        // Calculate positions
        // New slot goes at: sizeof(PageHeader) + num_slots * sizeof(SlotEntry)
        // New data goes at: free_space_end - length
        uint16_t new_data_offset = header()->free_space_end - length;
        
        // Update header FIRST (before we might fail)
        header()->num_slots++;
        header()->free_space_start = 
            static_cast<uint16_t>(sizeof(PageHeader) + header()->num_slots * sizeof(SlotEntry));
        header()->free_space_end = new_data_offset;
        
        // Initialize the slot
        slot(new_slot_id).Init(new_data_offset, length);
        
        // Copy the data
        std::memcpy(data_ + new_data_offset, data, length);
        
        return new_slot_id;  // Implicitly converts to std::expected success
    }
    
    // Delete a record by marking its slot as deleted
    // The space is NOT immediately reclaimed - call Compact() to reclaim
    [[nodiscard]] std::expected<void, Status> DeleteRecord(SlotId slot_id) {
        if (slot_id >= header()->num_slots) {
            return std::unexpected(Status::InvalidArgument("Invalid slot ID"));
        }
        
        SlotEntry& s = slot(slot_id);
        if (!s.IsValid()) {
            return std::unexpected(Status::NotFound("Record already deleted"));
        }
        
        s.MarkDeleted();
        return {};  // Success: empty expected<void, E>
    }
    
    // -------------------------------------------------------------------------
    // Raw Data Access (for disk I/O)
    // -------------------------------------------------------------------------
    
    // Get pointer to the raw page data (for reading/writing to disk)
    // C++23: Deducing this handles const/non-const
    [[nodiscard]] auto RawData(this auto&& self) noexcept
        -> decltype(auto)
    {
        using SelfType = std::remove_reference_t<decltype(self)>;
        if constexpr (std::is_const_v<SelfType>) {
            return static_cast<const uint8_t*>(self.data_);
        } else {
            return static_cast<uint8_t*>(self.data_);
        }
    }
    
    // Size is always kPageSize
    [[nodiscard]] static constexpr size_t RawSize() noexcept { 
        return kPageSize; 
    }
    
private:
    // The actual page data - exactly kPageSize bytes
    alignas(8) uint8_t data_[kPageSize];
};

// Verify Page is exactly kPageSize
static_assert(sizeof(Page) == kPageSize, "Page must be exactly kPageSize bytes");

}  // namespace kvstore::storage