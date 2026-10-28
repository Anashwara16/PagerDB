// ==============================================================================
// kvstore/storage/page.cc - Page Implementation (C++23)
// ==============================================================================
//
// Most of the Page functionality is implemented inline in the header since
// the methods are simple. This file exists for:
//   1. More complex future operations (compaction, checksum calculation)
//   2. Keeping the translation unit structure consistent
//   3. Debugging utilities
//
// C++23 Features Used:
// --------------------
//   - [[maybe_unused]] attribute for intentionally unused items
//   - std::bit_cast for safe type punning (in future implementations)
//   - constexpr where applicable
//
// ==============================================================================

#include "kvstore/storage/page_format.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <ostream>
#include <ranges>
#include <string_view>
#include <vector>
#include <bit>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>  // __crc32b / __crc32d (hardware CRC-32, same polynomial)
#endif

namespace kvstore::storage {

// ==============================================================================
// Future Implementation Notes
// ==============================================================================
//
// When you're ready to implement these features, add them here:
//
// 1. Page Compaction:
//    Status Page::Compact() {
//        // Move all valid records to end of page contiguously
//        // Update slot offsets
//        // Reclaim space from deleted records
//        // Could use std::ranges for iteration
//    }
//
// 2. Checksum Calculation (using CRC32):
//    uint32_t Page::CalculateChecksum() const {
//        // CRC32 of all bytes except the checksum field itself
//        // Consider using std::bit_cast for safe byte access
//    }
//
// 3. Debug Dump:
//    void Page::Dump(std::ostream& out) const {
//        // Print page contents in human-readable format
//        // Use std::format (C++20/23) for formatting
//    }
//
// 4. Page Validation:
//    bool Page::Validate() const {
//        // Verify internal consistency:
//        //   - Slots don't overlap
//        //   - free_space_start <= free_space_end
//        //   - All slot offsets are within bounds
//    }

// ==============================================================================
// Implementation Placeholder
// ==============================================================================
//
// This namespace-scope function ensures the translation unit isn't empty.
// Empty translation units can cause linker warnings on some platforms.

namespace {

// C++23: [[maybe_unused]] suppresses warnings for intentionally unused code
[[maybe_unused]] 
constexpr void placeholder() noexcept {
    // This function exists only to prevent empty translation unit warnings.
    // It will be optimized away completely by the compiler.
}

// CRC-32 (polynomial 0xEDB88320, reflected), byte-identical on every path.
//
// Phase 6 optimization #1: profiling the random-load benchmark showed ~55%
// of wall time inside the old byte-at-a-time table CRC (WAL record CRCs +
// page checksum stamping/verification, each pass covering a full 4 KB page).
// Two faster implementations of the SAME function:
//   - ARMv8 CRC32 instructions (__crc32b/__crc32d) where available - these
//     implement exactly this reflected polynomial, so stored checksums are
//     unchanged.
//   - Slicing-by-8 tables (8 x 256 x 4 B, compile-time generated) elsewhere;
//     processes 8 bytes per step instead of 1.
// docs/BENCHMARKS.md records the before/after numbers.

#if defined(__ARM_FEATURE_CRC32)

inline uint32_t Crc32Update(uint32_t crc, const uint8_t* data, size_t len) noexcept {
    // Align to 8 bytes, then consume 8-byte chunks.
    while (len > 0 && (reinterpret_cast<uintptr_t>(data) & 7) != 0) {
        crc = __crc32b(crc, *data++);
        --len;
    }
    while (len >= 8) {
        uint64_t chunk;
        std::memcpy(&chunk, data, sizeof(chunk));
        crc = __crc32d(crc, chunk);
        data += 8;
        len -= 8;
    }
    while (len > 0) {
        crc = __crc32b(crc, *data++);
        --len;
    }
    return crc;
}

#else  // Portable slicing-by-8

constexpr std::array<std::array<uint32_t, 256>, 8> GenerateCrc32Tables() noexcept {
    std::array<std::array<uint32_t, 256>, 8> tables{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
        tables[0][i] = crc;
    }
    for (uint32_t i = 0; i < 256; ++i) {
        for (size_t k = 1; k < 8; ++k) {
            tables[k][i] = (tables[k - 1][i] >> 8) ^
                           tables[0][tables[k - 1][i] & 0xFF];
        }
    }
    return tables;
}

// Compile-time CRC32 tables (8 KB)
inline constexpr auto kCrc32Tables = GenerateCrc32Tables();

inline uint32_t Crc32Update(uint32_t crc, const uint8_t* data, size_t len) noexcept {
    const auto& t = kCrc32Tables;
    while (len >= 8) {
        uint32_t low;
        uint32_t high;
        std::memcpy(&low, data, sizeof(low));
        std::memcpy(&high, data + 4, sizeof(high));
        if constexpr (std::endian::native != std::endian::little) {
            low = __builtin_bswap32(low);
            high = __builtin_bswap32(high);
        }
        low ^= crc;
        crc = t[7][low & 0xFF] ^ t[6][(low >> 8) & 0xFF] ^
              t[5][(low >> 16) & 0xFF] ^ t[4][low >> 24] ^
              t[3][high & 0xFF] ^ t[2][(high >> 8) & 0xFF] ^
              t[1][(high >> 16) & 0xFF] ^ t[0][high >> 24];
        data += 8;
        len -= 8;
    }
    for (size_t i = 0; i < len; ++i) {
        crc = t[0][(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

#endif  // __ARM_FEATURE_CRC32

}  // namespace

// ==============================================================================
// Checksums (format v2)
// ==============================================================================

uint32_t Crc32WithZeroedField(const void* data, size_t len,
                              size_t field_offset) noexcept {
    const auto* bytes = static_cast<const uint8_t*>(data);
    constexpr uint8_t kZeros[4] = {0, 0, 0, 0};

    uint32_t crc = 0xFFFFFFFFu;
    crc = Crc32Update(crc, bytes, field_offset);
    crc = Crc32Update(crc, kZeros, sizeof(kZeros));
    crc = Crc32Update(crc, bytes + field_offset + sizeof(kZeros),
                      len - field_offset - sizeof(kZeros));
    return crc ^ 0xFFFFFFFFu;
}

uint32_t Crc32(const void* data, size_t len) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    crc = Crc32Update(crc, static_cast<const uint8_t*>(data), len);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t Page::ComputeChecksum() const noexcept {
    return Crc32WithZeroedField(data_, kPageSize, offsetof(PageHeader, checksum));
}

bool Page::IsZeroed() const noexcept {
    return std::all_of(data_, data_ + kPageSize,
                       [](uint8_t byte) { return byte == 0; });
}

// ==============================================================================
// Compaction (Phase 2)
// ==============================================================================

void Page::Compact() {
    PageHeader* hdr = header();

    // Rebuild the record area in a scratch page: header + slot directory are
    // carried over, live records are packed back-to-front, everything else
    // (dead records, stale free space) becomes zeros.
    Page scratch;
    std::memset(scratch.data_, 0, kPageSize);
    std::memcpy(scratch.data_, data_,
                sizeof(PageHeader) +
                    static_cast<size_t>(hdr->num_slots) * sizeof(SlotEntry));

    uint16_t end = kPageSize;
    for (SlotId i = 0; i < hdr->num_slots; ++i) {
        SlotEntry& out = scratch.slot(i);
        if (!out.IsValid()) {
            // Tombstone: its bytes are dropped; clear the stale reference.
            out.offset = 0;
            out.length = 0;
            continue;
        }
        end = static_cast<uint16_t>(end - out.length);
        std::memcpy(scratch.data_ + end, data_ + out.offset, out.length);
        out.offset = end;
    }

    scratch.header()->free_space_end = end;
    scratch.header()->dead_bytes = 0;

    std::memcpy(data_, scratch.data_, kPageSize);
    assert(Validate());
}

// ==============================================================================
// Validation and Debug Dump (format v2)
// ==============================================================================

bool Page::Validate() const {
    const PageHeader* hdr = header();

    // Slot directory and free-space bookkeeping must be consistent.
    const size_t expected_start =
        sizeof(PageHeader) + static_cast<size_t>(hdr->num_slots) * sizeof(SlotEntry);
    if (hdr->free_space_start != expected_start) {
        return false;
    }
    if (hdr->free_space_start > hdr->free_space_end ||
        hdr->free_space_end > kPageSize) {
        return false;
    }

    // Every live record must lie fully inside [free_space_end, kPageSize].
    std::vector<std::pair<uint16_t, uint16_t>> live;  // (offset, length)
    live.reserve(hdr->num_slots);
    for (uint16_t i = 0; i < hdr->num_slots; ++i) {
        const SlotEntry& s = slot(i);
        if (!s.IsValid()) {
            continue;  // Tombstones keep stale offsets until compaction
        }
        if (s.offset < hdr->free_space_end ||
            static_cast<size_t>(s.offset) + s.length > kPageSize) {
            return false;
        }
        live.emplace_back(s.offset, s.length);
    }

    // Live records must not overlap.
    std::ranges::sort(live);
    for (size_t i = 1; i < live.size(); ++i) {
        if (static_cast<size_t>(live[i - 1].first) + live[i - 1].second >
            live[i].first) {
            return false;
        }
    }

    return true;
}

void Page::Dump(std::ostream& out) const {
    const PageHeader* hdr = header();

    const std::string_view type_name = [&] {
        switch (hdr->type) {
            case PageType::kMeta:     return std::string_view("meta");
            case PageType::kInternal: return std::string_view("internal");
            case PageType::kLeaf:     return std::string_view("leaf");
            case PageType::kFree:     return std::string_view("free");
            case PageType::kOverflow: return std::string_view("overflow");
        }
        return std::string_view("unknown");
    }();

    out << "page_id=" << hdr->page_id
        << " type=" << type_name
        << " num_slots=" << hdr->num_slots
        << " free=[" << hdr->free_space_start << "," << hdr->free_space_end << ")"
        << " next=" << hdr->next_page
        << " prev=" << hdr->prev_page
        << " rightmost=" << hdr->rightmost_child
        << " dead_bytes=" << hdr->dead_bytes
        << " lsn=" << hdr->page_lsn
        << " checksum=0x" << std::hex << hdr->checksum << std::dec
        << " valid=" << (Validate() ? "yes" : "NO")
        << "\n";

    for (uint16_t i = 0; i < hdr->num_slots; ++i) {
        const SlotEntry& s = slot(i);
        out << "  slot[" << i << "] offset=" << s.offset
            << " length=" << s.length
            << (s.IsValid() ? "" : " (deleted)")
            << "\n";
    }
}

}  // namespace kvstore::storage
