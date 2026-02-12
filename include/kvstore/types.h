// ==============================================================================
// kvstore/types.h - Core Type Definitions (C++23 Version)
// ==============================================================================
//
// This file defines the fundamental types used throughout the key-value store.
// Having these in a single location ensures consistency and makes it easy to
// change the size of identifiers if needed (e.g., upgrading to 64-bit page IDs).
//
// Design Philosophy:
// -----------------
// We use type aliases (using/typedef) rather than raw integers because:
//   1. Self-documenting code: PageId is clearer than uint32_t
//   2. Type safety: Prevents accidentally mixing page IDs with LSNs
//   3. Easy to change: If we need 64-bit page IDs, change once here
//
// Note: In C++, "using X = Y" is the modern form of "typedef Y X"
//       They're functionally equivalent, but "using" is preferred in modern C++
//
// C++23 Improvements:
// ------------------
//   - Using std::numeric_limits for sentinel values (clearer than hex magic numbers)
//   - Added static_assert checks to verify type sizes at compile time
//
// ==============================================================================

#pragma once  // Modern include guard - prevents double inclusion

#include <cstdint>   // For fixed-width integers: uint8_t, uint16_t, uint32_t, uint64_t
#include <cstddef>  // For size_t
#include <limits>  // For std::numeric_limits - gives us max/min values for any type

namespace kvstore {

// ==============================================================================
// Page Identifier (PageId)
// ==============================================================================
//
// Every page in our database file has a unique ID. The ID directly corresponds
// to the page's position in the file:
//
//   offset_in_file = page_id * PAGE_SIZE
//
// With 32-bit page IDs and 4KB pages, we can address:
//   2^32 pages × 4KB = 16 TB of data
//
// This is sufficient for a learning project. Production systems like PostgreSQL
// use larger identifiers (64-bit) or have multiple files.
//
// Why uint32_t instead of int?
//   - Guaranteed size (int varies by platform)
//   - No negative values (page IDs are always >= 0)
//   - Full 32-bit range available

using PageId = std::uint32_t;

// Compile-time check: ensure PageId is exactly 4 bytes
// static_assert runs at compile time - if it fails, you get a clear error message
static_assert(sizeof(PageId) == 4, "PageId must be 4 bytes");

// Special value indicating "no page" or "invalid page"
// 
// Why use std::numeric_limits<PageId>::max()?
//   - Self-documenting: clearly says "maximum possible value"
//   - Type-safe: automatically correct if we change PageId's underlying type
//   - No magic numbers: 0xFFFFFFFFu is harder to understand at a glance
//
// Why max value rather than 0?
//   - Page 0 is valid (it's our metadata page)
//   - Max value is obviously "special" and easy to spot in debugging
//   - Common convention in systems programming
constexpr PageId kInvalidPageId = std::numeric_limits<PageId>::max();

// ==============================================================================
// Log Sequence Number (LSN)
// ==============================================================================
//
// The LSN is a monotonically increasing identifier for each log record written
// to the Write-Ahead Log (WAL). It serves several critical purposes:
//
//   1. Ordering: Tells us the order operations occurred
//   2. Recovery: Tells us which log records to replay after a crash
//   3. Page tracking: Each page stores its LSN, telling us if it's up-to-date
//
// Why 64-bit?
//   With 64 bits, even writing 1 billion log records per second, we'd need
//   ~585 years to overflow. This ensures LSNs are practically unlimited.
//
// In practice, the LSN often directly maps to the byte offset in the log file,
// which makes it easy to seek to a specific log record.

using Lsn = std::uint64_t;

static_assert(sizeof(Lsn) == 8, "Lsn must be 8 bytes");

// Starting LSN value - log sequence numbers begin at 1
// Zero is reserved to mean "no LSN" or "page has never been modified"
constexpr Lsn kInvalidLsn = 0;
constexpr Lsn kFirstLsn = 1;

// ==============================================================================
// Transaction Identifier (TxId)
// ==============================================================================
//
// Each transaction gets a unique ID. This is used for:
//   1. Associating log records with transactions
//   2. Tracking which transactions are active
//   3. MVCC (Multi-Version Concurrency Control) in advanced implementations
//
// 64-bit ensures we won't run out of transaction IDs in practice.

using TxId = std::uint64_t;

static_assert(sizeof(TxId) == 8, "TxId must be 8 bytes");

// Special values for transactions
constexpr TxId kInvalidTxId = 0;

// ==============================================================================
// Slot Index
// ==============================================================================
//
// Within a slotted page, each record is identified by a slot number.
// 16 bits allows up to 65,535 slots per page, which is far more than we'd
// ever need in a 4KB page.

using SlotId = std::uint16_t;

static_assert(sizeof(SlotId) == 2, "SlotId must be 2 bytes");

// Maximum meaningful slot ID (used for bounds checking)
constexpr SlotId kInvalidSlotId = std::numeric_limits<SlotId>::max();

// ==============================================================================
// Size Types
// ==============================================================================
//
// For clarity when working with sizes within pages (which are small),
// we use specific types.

// Offset within a page (max 4KB = 4096, easily fits in 16 bits)
using PageOffset = std::uint16_t;

// Length of data within a page
using RecordLength = std::uint16_t;

static_assert(sizeof(PageOffset) == 2, "PageOffset must be 2 bytes");
static_assert(sizeof(RecordLength) == 2, "RecordLength must be 2 bytes");

// ==============================================================================
// Compile-Time Validation
// ==============================================================================
//
// These checks ensure our invalid values are what we expect.
// If someone accidentally changes the types, these will catch it at compile time.

static_assert(kInvalidPageId == 0xFFFFFFFFu, "kInvalidPageId should be max uint32");
static_assert(kInvalidSlotId == 0xFFFFu, "kInvalidSlotId should be max uint16");

}  // namespace kvstore
