// Exercise: build a types.h from scratch, one identifier type at a time.
//
// Pattern for each type (see PageId below, worked example):
//   1. `using Name = <fixed-width int>;`   — alias, not raw int
//   2. static_assert(sizeof(Name) == N, "...")   — pin the size
//   3. constexpr Name kInvalidName = <sentinel>;  — "no value" marker
//
// Why fixed-width ints (uint32_t) not `int`/`long`? Platform-portable size,
// no negative values for things that can't be negative (page numbers).
// Why a sentinel constant instead of e.g. -1? Unsigned types can't hold -1
// meaningfully; std::numeric_limits<T>::max() is the idiomatic "invalid" mark.

#pragma once 
#include <cstdint>
#include <limits>

namespace kvstore_ex {

// ---- Worked example: PageId ----
// Identifies a page in the DB file. 32 bits: 4B pages * 4KB = 16TB addressable.
using PageId = std::uint32_t;
static_assert(sizeof(PageId) == 4, "PageId must be 4 bytes");
constexpr PageId kInvalidPageId = std::numeric_limits<PageId>::max();

// ---- Your turn: TxId ----
// A transaction identifier. Real kvstore uses this to tag every WAL record
// with which transaction wrote it, and to track active transactions.
// Needs: 64 bits (transactions can be numerous over a DB's lifetime),
// monotonically increasing, 0 reserved to mean "no transaction".
//
// TODO(human): define TxId following the PageId pattern above:
//   using TxId = ...;
//   static_assert(...)
//   constexpr TxId kInvalidTxId = ...;
using TxId = std::uint64_t;
static_assert(sizeof(TxId) == 8, "TxId must be 8 bytes");
constexpr TxId kInvalidTxId = 0;

// ---- Your turn: SlotId ----
// Identifies a record's slot within a slotted page (see page_format.h).
// Needs: fits far more slots than a 4KB page could ever hold, so 16 bits
// is plenty (65,535 max) — no need to burn 32/64 bits on this one.
// Sentinel: max value (slot 0 is a real, valid slot — same reasoning as
// PageId, unlike TxId where 0 was free).
//
// TODO(human): define SlotId following the PageId pattern above:
//   using SlotId = ...;
//   static_assert(...)
//   constexpr SlotId kInvalidSlotId = ...;

using SlotId = std::uint16_t;
static_assert(sizeof(SlotId) == 2, "SlotId must be 2 bytes");
constexpr SlotId kInvalidSlotId = std::numeric_limits<SlotId>::max();

}  // namespace kvstore_ex
