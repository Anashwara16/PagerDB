// ==============================================================================
// kvstore/btree/node.h - B+ Tree Node Encoding (C++23)
// ==============================================================================
//
// This file provides NodeView - a helper class that interprets a Page as a
// B+ tree node and provides operations for manipulating keys, values, and
// child pointers.
//
// B+ Tree Node Types:
// -------------------
//
// LEAF NODE: Stores actual key-value pairs
//   ┌─────────────────────────────────────────────────────────────┐
//   │ Page Header (type=kLeaf, num_slots=N)                       │
//   ├─────────────────────────────────────────────────────────────┤
//   │ Slot 0 │ Slot 1 │ ... │ Slot N-1 │     free space          │
//   ├─────────────────────────────────────────────────────────────┤
//   │                      free space                             │
//   ├─────────────────────────────────────────────────────────────┤
//   │  Record N-1  │ ... │  Record 1  │  Record 0                 │
//   └─────────────────────────────────────────────────────────────┘
//
//   Each record: [key_len:2][key:key_len][value_len:2][value:value_len]
//
// INTERNAL NODE: Stores keys and child page pointers
//   ┌─────────────────────────────────────────────────────────────┐
//   │ Page Header (type=kInternal, num_slots=N)                   │
//   │ rightmost_child stored in header.next_leaf field            │
//   ├─────────────────────────────────────────────────────────────┤
//   │ Slot 0 │ Slot 1 │ ... │ Slot N-1 │     free space          │
//   ├─────────────────────────────────────────────────────────────┤
//   │                      free space                             │
//   ├─────────────────────────────────────────────────────────────┤
//   │  Record N-1  │ ... │  Record 1  │  Record 0                 │
//   └─────────────────────────────────────────────────────────────┘
//
//   Each record: [key_len:2][key:key_len][child_page:4]
//   The child pointer is for keys < this key
//   rightmost_child is for keys >= last key
//
// Key Ordering:
// -------------
//   Keys are stored in SORTED ORDER (ascending).
//   Slot 0 has the smallest key, Slot N-1 has the largest.
//
// C++23 Features Used:
// --------------------
//   - [[nodiscard("reason")]] with explanatory messages
//   - Deducing this for const/non-const accessors
//   - constexpr where applicable
//   - std::to_underlying for enum conversion
//
// ==============================================================================

#pragma once

#include <string_view>
#include <string>
#include <cstring>
#include <cstdint>
#include <optional>
#include <utility>

#include "kvstore/types.h"
#include "kvstore/status.h"
#include "kvstore/storage/page_format.h"

namespace kvstore::btree {

// ==============================================================================
// NodeView Class
// ==============================================================================
//
// NodeView provides a B+ tree-specific interface over a raw Page.
// It does NOT own the page - it's just a view/helper.
//
// Usage:
//   Page page;
//   pager.ReadPage(page_id, page);
//   
//   NodeView node(page);
//   if (node.IsLeaf()) {
//       auto value = node.GetValue(key);
//   }

class NodeView {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------
    
    // Create a view over an existing page
    explicit constexpr NodeView(storage::Page& page) noexcept : page_(page) {}
    
    // Initialize a new node (call after page.Init())
    constexpr void InitLeaf() noexcept {
        page_.header()->type = storage::PageType::kLeaf;
        page_.header()->next_page = kInvalidPageId;  // No next leaf yet
        page_.header()->prev_page = kInvalidPageId;  // No prev leaf yet
    }
    
    constexpr void InitInternal() noexcept {
        page_.header()->type = storage::PageType::kInternal;
        page_.header()->next_page = kInvalidPageId;  // Used as rightmost_child
    }
    
    // -------------------------------------------------------------------------
    // Node Type (using deducing this for const-correctness)
    // -------------------------------------------------------------------------
    
    [[nodiscard("IsLeaf returns node type information")]]
    constexpr bool IsLeaf(this auto&& self) noexcept {
        return self.page_.header()->type == storage::PageType::kLeaf;
    }
    
    [[nodiscard("IsInternal returns node type information")]]
    constexpr bool IsInternal(this auto&& self) noexcept {
        return self.page_.header()->type == storage::PageType::kInternal;
    }
    
    // -------------------------------------------------------------------------
    // Key Count (using deducing this)
    // -------------------------------------------------------------------------
    
    [[nodiscard("NumKeys returns the key count")]]
    constexpr uint16_t NumKeys(this auto&& self) noexcept {
        return self.page_.header()->num_slots;
    }
    
    [[nodiscard("IsEmpty returns whether node has no keys")]]
    constexpr bool IsEmpty(this auto&& self) noexcept {
        return self.NumKeys() == 0;
    }
    
    // -------------------------------------------------------------------------
    // Key Access
    // -------------------------------------------------------------------------
    
    // Get the key at a given index (0 to NumKeys()-1)
    [[nodiscard("GetKey returns the key at index")]]
    std::string_view GetKey(uint16_t index) const;
    
    // Find the index where a key is or should be inserted
    // Returns: index where key is found, or where it should be inserted
    // Sets found=true if exact match, found=false if not found
    [[nodiscard("FindKeyIndex returns index and found status")]]
    uint16_t FindKeyIndex(std::string_view key, bool& found) const;
    
    // Convenience: just find index without found flag
    [[nodiscard("LowerBound returns insertion point")]]
    uint16_t LowerBound(std::string_view key) const {
        bool found;
        return FindKeyIndex(key, found);
    }
    
    // -------------------------------------------------------------------------
    // Leaf Node Operations
    // -------------------------------------------------------------------------
    
    // Get value for a key (leaf nodes only)
    // Returns empty optional if key not found
    [[nodiscard("GetValue returns the value for a key")]]
    std::optional<std::string_view> GetValue(std::string_view key) const;
    
    // Get value at index (leaf nodes only)
    [[nodiscard("GetValueAt returns value at index")]]
    std::string_view GetValueAt(uint16_t index) const;
    
    // Insert a key-value pair (leaf nodes only)
    // Returns false if page is full
    [[nodiscard("InsertLeaf returns success status")]]
    bool InsertLeaf(std::string_view key, std::string_view value);
    
    // Update value for existing key (leaf nodes only)
    // Returns false if key not found or new value doesn't fit
    [[nodiscard("UpdateLeaf returns success status")]]
    bool UpdateLeaf(std::string_view key, std::string_view new_value);
    
    // -------------------------------------------------------------------------
    // Internal Node Operations
    // -------------------------------------------------------------------------
    
    // Get child page ID at index (internal nodes only)
    // For index i, this is the child for keys < GetKey(i)
    [[nodiscard("GetChild returns child page ID")]]
    PageId GetChild(uint16_t index) const;
    
    // Get the rightmost child (for keys >= last key)
    [[nodiscard("GetRightmostChild returns rightmost child page ID")]]
    constexpr PageId GetRightmostChild(this auto&& self) noexcept {
        return self.page_.header()->next_page;  // We reuse this field
    }
    
    // Set the rightmost child
    constexpr void SetRightmostChild(PageId child) noexcept {
        page_.header()->next_page = child;
    }
    
    // Find which child to follow for a given key
    // Returns the PageId of the child that might contain the key
    [[nodiscard("FindChild returns the child to descend into")]]
    PageId FindChild(std::string_view key) const;
    
    // Insert a key and left child pointer (internal nodes only)
    // The key separates left_child (keys < key) from the next child
    [[nodiscard("InsertInternal returns success status")]]
    bool InsertInternal(std::string_view key, PageId left_child);
    
    // Insert after a child split (internal nodes only)
    // Inserts split_key, with left_child for keys < split_key
    // and updates the following entry to point to right_child
    [[nodiscard("InsertInternalAfterSplit returns success status")]]
    bool InsertInternalAfterSplit(std::string_view split_key, 
                                   PageId left_child, 
                                   PageId right_child);
    
    // -------------------------------------------------------------------------
    // Leaf Linked List (for range scans) - using deducing this
    // -------------------------------------------------------------------------
    
    [[nodiscard("GetNextLeaf returns next leaf page ID")]]
    constexpr PageId GetNextLeaf(this auto&& self) noexcept {
        return self.page_.header()->next_page;
    }
    
    [[nodiscard("GetPrevLeaf returns previous leaf page ID")]]
    constexpr PageId GetPrevLeaf(this auto&& self) noexcept {
        return self.page_.header()->prev_page;
    }
    
    constexpr void SetNextLeaf(PageId next) noexcept {
        page_.header()->next_page = next;
    }
    
    constexpr void SetPrevLeaf(PageId prev) noexcept {
        page_.header()->prev_page = prev;
    }
    
    // -------------------------------------------------------------------------
    // Split Support (using deducing this)
    // -------------------------------------------------------------------------
    
    // Get the split point (middle index)
    [[nodiscard("GetSplitIndex returns the middle index for splitting")]]
    constexpr uint16_t GetSplitIndex(this auto&& self) noexcept {
        return self.NumKeys() / 2;
    }
    
    // Check if node needs to split (is "full enough" that insert might fail)
    [[nodiscard("ShouldSplit returns whether node is near capacity")]]
    constexpr bool ShouldSplit(this auto&& self) noexcept {
        // Split when less than 10% free space remains
        return self.page_.FreeSpace() < storage::kPageSize / 10;
    }
    
    // Check if we can fit a record of given size
    [[nodiscard("CanFit returns whether record fits in page")]]
    constexpr bool CanFit(this auto&& self, uint16_t key_len, uint16_t value_len) noexcept {
        // Need: slot entry + key_len(2) + key + value_len(2) + value
        uint16_t needed = sizeof(storage::SlotEntry) + 2 + key_len + 2 + value_len;
        return self.page_.FreeSpace() >= needed;
    }
    
    // For internal nodes
    [[nodiscard("CanFitInternal returns whether internal record fits")]]
    constexpr bool CanFitInternal(this auto&& self, uint16_t key_len) noexcept {
        // Need: slot entry + key_len(2) + key + child(4)
        uint16_t needed = sizeof(storage::SlotEntry) + 2 + key_len + 4;
        return self.page_.FreeSpace() >= needed;
    }
    
    // -------------------------------------------------------------------------
    // Raw Page Access (using deducing this)
    // -------------------------------------------------------------------------
    
    [[nodiscard("GetPage returns reference to underlying page")]]
    constexpr auto&& GetPage(this auto&& self) noexcept {
        return std::forward<decltype(self)>(self).page_;
    }

private:
    storage::Page& page_;
    
    // -------------------------------------------------------------------------
    // Internal Helpers
    // -------------------------------------------------------------------------
    
    // Encode a leaf record: [key_len:2][key][value_len:2][value]
    [[nodiscard]]
    static std::string EncodeLeafRecord(std::string_view key, std::string_view value);
    
    // Decode a leaf record
    static void DecodeLeafRecord(const uint8_t* data, uint16_t len,
                                  std::string_view& key, std::string_view& value);
    
    // Encode an internal record: [key_len:2][key][child:4]
    [[nodiscard]]
    static std::string EncodeInternalRecord(std::string_view key, PageId child);
    
    // Decode an internal record
    static void DecodeInternalRecord(const uint8_t* data, uint16_t len,
                                      std::string_view& key, PageId& child);
    
    // Insert at a specific position (shifts other entries)
    [[nodiscard]]
    bool InsertAt(uint16_t index, const std::string& encoded_record);
};

}  // namespace kvstore::btree
