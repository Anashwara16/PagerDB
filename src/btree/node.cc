// ==============================================================================
// kvstore/btree/node.cc - B+ Tree Node Implementation (C++23)
// ==============================================================================
//
// This file implements the NodeView class which provides B+ tree node
// operations over raw pages.
//
// Record Formats:
// ---------------
//
// Leaf Record: [key_len:2][key:key_len][value_len:2][value:value_len]
//   - key_len: 2 bytes, little-endian
//   - key: variable length
//   - value_len: 2 bytes, little-endian  
//   - value: variable length
//
// Internal Record: [key_len:2][key:key_len][child:4]
//   - key_len: 2 bytes, little-endian
//   - key: variable length
//   - child: 4 bytes, little-endian PageId
//
// C++23 Features Used:
// --------------------
//   - std::bit_cast for safe type conversion
//   - std::ranges algorithms
//   - std::views for iteration
//   - [[likely]] / [[unlikely]] attributes
//   - Improved structured bindings
//
// ==============================================================================

#include "kvstore/btree/node.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <ranges>
#include <tuple>
#include <vector>

namespace kvstore::btree {

// ==============================================================================
// Little-Endian Encoding Helpers (C++23 style)
// ==============================================================================

namespace {

// Write a uint16_t in little-endian format
inline void WriteLE16(uint8_t* ptr, uint16_t value) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(ptr, &value, sizeof(value));
    } else {
        ptr[0] = static_cast<uint8_t>(value & 0xFF);
        ptr[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    }
}

// Read a uint16_t from little-endian format
[[nodiscard]]
inline uint16_t ReadLE16(const uint8_t* ptr) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        uint16_t value;
        std::memcpy(&value, ptr, sizeof(value));
        return value;
    } else {
        return static_cast<uint16_t>(ptr[0]) | 
               (static_cast<uint16_t>(ptr[1]) << 8);
    }
}

// Write a uint32_t (PageId) in little-endian format
inline void WriteLE32(uint8_t* ptr, uint32_t value) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(ptr, &value, sizeof(value));
    } else {
        ptr[0] = static_cast<uint8_t>(value & 0xFF);
        ptr[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        ptr[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
        ptr[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
    }
}

// Read a uint32_t (PageId) from little-endian format
[[nodiscard]]
inline uint32_t ReadLE32(const uint8_t* ptr) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        uint32_t value;
        std::memcpy(&value, ptr, sizeof(value));
        return value;
    } else {
        return static_cast<uint32_t>(ptr[0]) |
               (static_cast<uint32_t>(ptr[1]) << 8) |
               (static_cast<uint32_t>(ptr[2]) << 16) |
               (static_cast<uint32_t>(ptr[3]) << 24);
    }
}

}  // namespace

// ==============================================================================
// Encoding Helpers
// ==============================================================================

std::string NodeView::EncodeLeafRecord(std::string_view key, std::string_view value) {
    // Total size: 2 + key.size() + 2 + value.size()
    std::string record;
    record.resize(4 + key.size() + value.size());
    
    auto* ptr = std::bit_cast<uint8_t*>(record.data());
    
    // Key length (2 bytes, little-endian)
    WriteLE16(ptr, static_cast<uint16_t>(key.size()));
    ptr += 2;
    
    // Key data
    std::ranges::copy(key, std::bit_cast<char*>(ptr));
    ptr += key.size();
    
    // Value length (2 bytes, little-endian)
    WriteLE16(ptr, static_cast<uint16_t>(value.size()));
    ptr += 2;
    
    // Value data
    std::ranges::copy(value, std::bit_cast<char*>(ptr));
    
    return record;
}

void NodeView::DecodeLeafRecord(const uint8_t* data, [[maybe_unused]] uint16_t len,
                                 std::string_view& key, std::string_view& value) {
    // Key length
    uint16_t key_len = ReadLE16(data);
    data += 2;
    
    // Key
    key = std::string_view(std::bit_cast<const char*>(data), key_len);
    data += key_len;
    
    // Value length
    uint16_t value_len = ReadLE16(data);
    data += 2;
    
    // Value
    value = std::string_view(std::bit_cast<const char*>(data), value_len);
}

std::string NodeView::EncodeInternalRecord(std::string_view key, PageId child) {
    // Total size: 2 + key.size() + 4
    std::string record;
    record.resize(6 + key.size());
    
    auto* ptr = std::bit_cast<uint8_t*>(record.data());
    
    // Key length (2 bytes, little-endian)
    WriteLE16(ptr, static_cast<uint16_t>(key.size()));
    ptr += 2;
    
    // Key data
    std::ranges::copy(key, std::bit_cast<char*>(ptr));
    ptr += key.size();
    
    // Child PageId (4 bytes, little-endian)
    WriteLE32(ptr, child);
    
    return record;
}

void NodeView::DecodeInternalRecord(const uint8_t* data, [[maybe_unused]] uint16_t len,
                                     std::string_view& key, PageId& child) {
    // Key length
    uint16_t key_len = ReadLE16(data);
    data += 2;
    
    // Key
    key = std::string_view(std::bit_cast<const char*>(data), key_len);
    data += key_len;
    
    // Child PageId
    child = static_cast<PageId>(ReadLE32(data));
}

// ==============================================================================
// Key Access
// ==============================================================================

std::string_view NodeView::GetKey(uint16_t index) const {
    if (index >= NumKeys()) [[unlikely]] {
        return {};
    }
    
    const uint8_t* data = page_.GetRecordPtr(index);
    if (!data) [[unlikely]] {
        return {};
    }
    
    // Key length is first 2 bytes
    uint16_t key_len = ReadLE16(data);
    
    return std::string_view(std::bit_cast<const char*>(data + 2), key_len);
}

uint16_t NodeView::FindKeyIndex(std::string_view key, bool& found) const {
    found = false;
    
    uint16_t num_keys = NumKeys();
    if (num_keys == 0) [[unlikely]] {
        return 0;
    }
    
    // Binary search for the key
    uint16_t left = 0;
    uint16_t right = num_keys;
    
    while (left < right) {
        uint16_t mid = left + (right - left) / 2;
        std::string_view mid_key = GetKey(mid);
        
        auto cmp = key.compare(mid_key);
        if (cmp == 0) {
            found = true;
            return mid;
        }
        if (cmp < 0) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }
    
    return left;
}

// ==============================================================================
// Leaf Node Operations
// ==============================================================================

std::optional<std::string_view> NodeView::GetValue(std::string_view key) const {
    if (!IsLeaf()) [[unlikely]] {
        return std::nullopt;
    }
    
    bool found;
    uint16_t index = FindKeyIndex(key, found);
    
    if (!found) [[unlikely]] {
        return std::nullopt;
    }
    
    return GetValueAt(index);
}

std::string_view NodeView::GetValueAt(uint16_t index) const {
    if (index >= NumKeys()) [[unlikely]] {
        return {};
    }
    
    const uint8_t* data = page_.GetRecordPtr(index);
    uint16_t len = page_.GetRecordLength(index);
    
    if (!data || len == 0) [[unlikely]] {
        return {};
    }
    
    std::string_view key, value;
    DecodeLeafRecord(data, len, key, value);
    return value;
}

bool NodeView::InsertAt(uint16_t index, const std::string& encoded_record) {
    // In-place slotted insert (Phase 6 optimization #2, defect #10): the
    // record goes to free_space_end and the slot directory shifts by one
    // entry - the old path copied every record into a vector<string> and
    // rebuilt the page. Callers (InsertLeaf/InsertInternal) run EnsureFit
    // first, so contiguous free space is guaranteed here.
    if (index > NumKeys()) [[unlikely]] {
        return false;
    }

    auto result = page_.InsertRecordAt(
        index, encoded_record.data(),
        static_cast<uint16_t>(encoded_record.size()));
    if (!result.has_value()) [[unlikely]] {
        return false;
    }

    assert(page_.Validate());
    return true;
}

bool NodeView::EnsureFit(size_t needed) {
    if (page_.FreeSpace() >= needed) [[likely]] {
        return true;
    }
    // Dead record bytes are reclaimable: compact and re-check.
    if (page_.FreeSpace() + page_.header()->dead_bytes >= needed) {
        page_.Compact();
        return page_.FreeSpace() >= needed;
    }
    return false;
}

bool NodeView::InsertLeaf(std::string_view key, std::string_view value) {
    if (!IsLeaf()) [[unlikely]] {
        return false;
    }

    // Check if we have space: slot + [key_len:2][key][value_len:2][value].
    // Dead bytes count - EnsureFit compacts the page when they are needed.
    if (!EnsureFit(sizeof(storage::SlotEntry) + 4 + key.size() + value.size())) {
        return false;
    }
    
    // Find insertion point
    bool found;
    uint16_t index = FindKeyIndex(key, found);
    
    if (found) [[unlikely]] {
        // Key exists - this is an update, not insert
        // For MVP, we don't support in-place update of different-sized values
        // Return false to signal caller should handle this
        return false;
    }
    
    // Encode the record
    std::string record = EncodeLeafRecord(key, value);
    
    // Insert at the correct position
    return InsertAt(index, record);
}

bool NodeView::UpdateLeaf(std::string_view key, std::string_view new_value) {
    // In-place slotted update (same pattern as InsertAt, which Phase 6 moved
    // off the rebuild-the-whole-page path; review caught this one still
    // rebuilding). Measured with BM_Overwrite (added for exactly this - the
    // 95/5 mixed workload hid the update path): DB-level update p50 -21%
    // (4.25 -> 3.38 us), throughput +6% - modest because the WAL append
    // dominates a full update, but the whole-page memcpy tax scaled with
    // record count and this is now O(record). Three cases, none of which
    // touch any other record:
    //   shrink/same : overwrite the bytes where they are; surplus goes dead
    //   grow, fits  : new copy at free_space_end, old copy goes dead
    //   grow, tight : tombstone old copy, Compact(), then place the new copy
    if (!IsLeaf()) [[unlikely]] {
        return false;
    }

    bool found;
    uint16_t index = FindKeyIndex(key, found);
    if (!found) [[unlikely]] {
        return false;
    }

    const std::string record = EncodeLeafRecord(key, new_value);
    const auto new_len = static_cast<uint16_t>(record.size());
    const uint16_t old_len = page_.GetRecordLength(index);
    auto* header = page_.header();

    if (new_len <= old_len) {
        // The key prefix of the encoding is identical; overwriting the whole
        // record in place is simplest. A shrink strands the tail bytes -
        // account them so EnsureFit/Compact can reclaim them later.
        std::memcpy(page_.GetRecordPtr(index), record.data(), new_len);
        if (new_len < old_len) {
            page_.slots()[index].length = new_len;
            header->dead_bytes =
                static_cast<uint16_t>(header->dead_bytes + (old_len - new_len));
        }
        assert(page_.Validate());
        return true;
    }

    // Grow: the new copy needs contiguous free space (no new slot - the
    // directory is untouched).
    if (page_.FreeSpace() < new_len) {
        if (static_cast<size_t>(page_.FreeSpace()) + header->dead_bytes +
                old_len < new_len) [[unlikely]] {
            return false;  // Won't fit even compacted - caller splits
        }
        // Tombstone our old copy so compaction reclaims it too, then revive
        // the slot below. Slot IDs are stable across Compact().
        std::ignore = page_.DeleteRecord(index);
        page_.Compact();
    } else {
        header->dead_bytes =
            static_cast<uint16_t>(header->dead_bytes + old_len);
    }

    const auto off = static_cast<uint16_t>(header->free_space_end - new_len);
    header->free_space_end = off;
    page_.slots()[index].Init(off, new_len);
    std::memcpy(page_.GetRecordPtr(index), record.data(), new_len);

    assert(page_.Validate());
    return true;
}

bool NodeView::DeleteAt(uint16_t index) {
    uint16_t num_keys = NumKeys();
    if (index >= num_keys) [[unlikely]] {
        return false;
    }

    auto* header = page_.header();
    auto* slots = page_.slots();

    // The record bytes are not moved; they become dead weight in the record
    // area. Only the record length counts here: the slot's 8 bytes are
    // reclaimed immediately by the directory shift below.
    header->dead_bytes = static_cast<uint16_t>(
        header->dead_bytes + slots[index].length);

    // Shift the slot directory down over the removed entry.
    for (uint16_t i = index; i + 1 < num_keys; ++i) {
        slots[i] = slots[i + 1];
    }

    header->num_slots = static_cast<uint16_t>(num_keys - 1);
    // Zero the vacated last slot so free space stays deterministic.
    std::memset(&slots[header->num_slots], 0, sizeof(storage::SlotEntry));
    header->free_space_start = static_cast<uint16_t>(
        sizeof(storage::PageHeader) +
        header->num_slots * sizeof(storage::SlotEntry));

    assert(page_.Validate());
    return true;
}

// ==============================================================================
// Internal Node Operations
// ==============================================================================

PageId NodeView::GetChild(uint16_t index) const {
    if (index >= NumKeys()) [[unlikely]] {
        return kInvalidPageId;
    }
    
    const uint8_t* data = page_.GetRecordPtr(index);
    uint16_t len = page_.GetRecordLength(index);
    
    if (!data || len == 0) [[unlikely]] {
        return kInvalidPageId;
    }
    
    std::string_view key;
    PageId child;
    DecodeInternalRecord(data, len, key, child);
    return child;
}

uint16_t NodeView::FindChildIndex(std::string_view key) const {
    // Binary search for the key's position. On an exact separator match the
    // key lives in the RIGHT subtree (child_i holds keys < key_i), so step
    // past the separator.
    bool found;
    uint16_t index = FindKeyIndex(key, found);
    return found ? static_cast<uint16_t>(index + 1) : index;
}

PageId NodeView::FindChild(std::string_view key) const {
    if (!IsInternal()) [[unlikely]] {
        return kInvalidPageId;
    }

    uint16_t child_index = FindChildIndex(key);
    if (child_index < NumKeys()) {
        return GetChild(child_index);
    }

    // Key is >= all keys, go to rightmost child
    return GetRightmostChild();
}

bool NodeView::InsertInternal(std::string_view key, PageId left_child) {
    if (!IsInternal()) [[unlikely]] {
        return false;
    }

    // Check if we have space: slot + [key_len:2][key][child:4].
    // Dead bytes count - EnsureFit compacts the page when they are needed.
    if (!EnsureFit(sizeof(storage::SlotEntry) + 6 + key.size())) {
        return false;
    }
    
    // Find insertion point
    bool found;
    uint16_t index = FindKeyIndex(key, found);
    
    // Encode the record
    std::string record = EncodeInternalRecord(key, left_child);
    
    // Insert at the correct position
    return InsertAt(index, record);
}

bool NodeView::InsertInternalAfterSplit(std::string_view split_key,
                                         PageId left_child,
                                         PageId right_child) {
    if (!IsInternal()) [[unlikely]] {
        return false;
    }
    
    // Check if we have space
    if (!CanFitInternal(static_cast<uint16_t>(split_key.size()))) {
        return false;
    }
    
    // Find insertion point for split_key
    bool found;
    uint16_t insert_index = FindKeyIndex(split_key, found);
    
    uint16_t num_keys = NumKeys();
    
    // Collect all existing records, modifying as needed
    std::vector<std::pair<std::string, PageId>> entries;
    entries.reserve(num_keys + 1);
    
    for (auto i : std::views::iota(uint16_t{0}, num_keys)) {
        std::string key_str(GetKey(i));
        PageId child = GetChild(i);
        
        // When we reach the insertion point, insert the new entry
        if (i == insert_index) {
            entries.emplace_back(std::string(split_key), left_child);
        }
        
        // For the entry AT the insertion point (now shifted to after),
        // its child should be right_child
        if (i == insert_index) {
            entries.emplace_back(std::move(key_str), right_child);
        } else {
            entries.emplace_back(std::move(key_str), child);
        }
    }
    
    // If insert_index == num_keys, append at the end
    if (insert_index == num_keys) {
        entries.emplace_back(std::string(split_key), left_child);
        // right_child becomes the new rightmost child
    }
    
    // Save rightmost child
    PageId old_rightmost = GetRightmostChild();
    PageId new_rightmost = (insert_index == num_keys) ? right_child : old_rightmost;
    
    // Calculate total space needed
    size_t total_needed = 0;
    for (const auto& [k, _] : entries) {
        total_needed += sizeof(storage::SlotEntry) + 2 + k.size() + 4;
    }
    
    size_t available = storage::kPageSize - sizeof(storage::PageHeader);
    if (total_needed > available) [[unlikely]] {
        return false;
    }
    
    // Reinitialize the page
    auto page_id = page_.header()->page_id;
    auto lsn = page_.header()->page_lsn;
    
    page_.Init(page_id, storage::PageType::kInternal);
    page_.header()->page_lsn = lsn;
    SetRightmostChild(new_rightmost);
    
    // Re-insert all entries
    for (const auto& [key, child] : entries) {
        std::string record = EncodeInternalRecord(key, child);
        auto result = page_.InsertRecord(record.data(), static_cast<uint16_t>(record.size()));
        if (!result.has_value()) [[unlikely]] {
            return false;
        }
    }

    assert(page_.Validate());
    return true;
}

}  // namespace kvstore::btree
