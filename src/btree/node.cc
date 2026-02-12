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
    // For simplicity in MVP, we don't support inserting in the middle
    // We'll rebuild the page with entries in order
    
    uint16_t num_keys = NumKeys();
    
    // Collect all existing records
    std::vector<std::string> records;
    records.reserve(num_keys + 1);
    
    for (auto i : std::views::iota(uint16_t{0}, num_keys)) {
        if (i == index) {
            records.push_back(encoded_record);
        }
        
        const uint8_t* data = page_.GetRecordPtr(i);
        uint16_t len = page_.GetRecordLength(i);
        records.emplace_back(std::bit_cast<const char*>(data), len);
    }
    
    if (index == num_keys) {
        records.push_back(encoded_record);
    }
    
    // Calculate total space needed
    size_t total_needed = 0;
    for (const auto& rec : records) {
        total_needed += sizeof(storage::SlotEntry) + rec.size();
    }
    
    size_t available = storage::kPageSize - sizeof(storage::PageHeader);
    if (total_needed > available) [[unlikely]] {
        return false;  // Won't fit
    }
    
    // Reinitialize the page (keeping type and other header fields)
    auto type = page_.header()->type;
    auto page_id = page_.header()->page_id;
    auto next = page_.header()->next_page;
    auto prev = page_.header()->prev_page;
    auto lsn = page_.header()->page_lsn;
    
    page_.Init(page_id, type);
    page_.header()->next_page = next;
    page_.header()->prev_page = prev;
    page_.header()->page_lsn = lsn;
    
    // Re-insert all records in order
    for (const auto& rec : records) {
        auto result = page_.InsertRecord(rec.data(), static_cast<uint16_t>(rec.size()));
        if (!result.has_value()) [[unlikely]] {
            return false;  // Should not happen if we calculated correctly
        }
    }
    
    return true;
}

bool NodeView::InsertLeaf(std::string_view key, std::string_view value) {
    if (!IsLeaf()) [[unlikely]] {
        return false;
    }
    
    // Check if we have space
    if (!CanFit(static_cast<uint16_t>(key.size()), static_cast<uint16_t>(value.size()))) {
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
    if (!IsLeaf()) [[unlikely]] {
        return false;
    }
    
    bool found;
    uint16_t index = FindKeyIndex(key, found);
    
    if (!found) [[unlikely]] {
        return false;
    }
    
    // Rebuild the page with the updated value
    uint16_t num_keys = NumKeys();
    
    // Collect all records, replacing the one at 'index' with new value
    std::vector<std::string> records;
    records.reserve(num_keys);
    
    for (auto i : std::views::iota(uint16_t{0}, num_keys)) {
        if (i == index) {
            // Use the new value for this key
            records.push_back(EncodeLeafRecord(key, new_value));
        } else {
            // Copy existing record
            const uint8_t* data = page_.GetRecordPtr(i);
            uint16_t len = page_.GetRecordLength(i);
            if (data && len > 0) [[likely]] {
                records.emplace_back(std::bit_cast<const char*>(data), len);
            }
        }
    }
    
    // Calculate total space needed
    size_t total_needed = 0;
    for (const auto& rec : records) {
        total_needed += sizeof(storage::SlotEntry) + rec.size();
    }
    
    size_t available = storage::kPageSize - sizeof(storage::PageHeader);
    if (total_needed > available) [[unlikely]] {
        return false;  // Won't fit
    }
    
    // Reinitialize the page
    auto type = page_.header()->type;
    auto page_id = page_.header()->page_id;
    auto next = page_.header()->next_page;
    auto prev = page_.header()->prev_page;
    auto lsn = page_.header()->page_lsn;
    
    page_.Init(page_id, type);
    page_.header()->next_page = next;
    page_.header()->prev_page = prev;
    page_.header()->page_lsn = lsn;
    
    // Re-insert all records in order
    for (const auto& rec : records) {
        auto result = page_.InsertRecord(rec.data(), static_cast<uint16_t>(rec.size()));
        if (!result.has_value()) [[unlikely]] {
            return false;
        }
    }
    
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

PageId NodeView::FindChild(std::string_view key) const {
    if (!IsInternal()) [[unlikely]] {
        return kInvalidPageId;
    }
    
    uint16_t num_keys = NumKeys();
    if (num_keys == 0) [[unlikely]] {
        return GetRightmostChild();
    }
    
    // Find the first key that is > search key
    // The child we want is the one to the left of that key
    for (auto i : std::views::iota(uint16_t{0}, num_keys)) {
        std::string_view node_key = GetKey(i);
        if (key < node_key) {
            return GetChild(i);
        }
    }
    
    // Key is >= all keys, go to rightmost child
    return GetRightmostChild();
}

bool NodeView::InsertInternal(std::string_view key, PageId left_child) {
    if (!IsInternal()) [[unlikely]] {
        return false;
    }
    
    // Check if we have space
    if (!CanFitInternal(static_cast<uint16_t>(key.size()))) {
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
    
    return true;
}

}  // namespace kvstore::btree
