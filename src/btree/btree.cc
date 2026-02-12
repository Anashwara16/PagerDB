// ==============================================================================
// kvstore/btree/btree.cc - B+ Tree Implementation (C++23)
// ==============================================================================
//
// This file implements the core B+ tree operations: Get and Put.
//
// Key Algorithms:
// ---------------
//
// GET (Search):
//   1. Start at root
//   2. At each internal node, find child to descend into
//   3. At leaf, binary search for key
//   4. Return value if found
//
// PUT (Insert):
//   1. Find the leaf that should contain the key
//   2. If key exists, update value
//   3. If key doesn't exist and leaf has space, insert
//   4. If leaf is full, split:
//      a. Create new leaf with upper half of keys
//      b. Insert new key into appropriate leaf
//      c. Push split key up to parent
//      d. If parent is full, recursively split parent
//      e. If root splits, create new root
//
// Split Logic:
// ------------
//
// Leaf Split:
//   Before: [A B C D E] (full)
//   After:  [A B C] -> [D E]  (push D up to parent)
//
// Internal Split:
//   Before: [K1 K2 K3 K4 K5] with children [C0 C1 C2 C3 C4 C5]
//   After:  [K1 K2] [C0 C1 C2]  and  [K4 K5] [C3 C4 C5]
//           Push K3 up to parent
//
// C++23 Features Used:
// --------------------
//   - std::unreachable() for impossible code paths
//   - std::ranges algorithms
//   - Structured bindings with auto&&
//   - [[likely]] / [[unlikely]] attributes
//
// ==============================================================================

#include "kvstore/btree/btree.h"

#include <algorithm>
#include <cstring>
#include <ranges>
#include <utility>
#include <vector>

namespace kvstore::btree {

// ==============================================================================
// Constructor
// ==============================================================================

BPlusTree::BPlusTree(storage::Pager& pager)
    : pager_(pager)
{}

// ==============================================================================
// Get
// ==============================================================================

Result<std::string> BPlusTree::Get(std::string_view key) {
    // Handle empty tree
    if (IsEmpty()) [[unlikely]] {
        return std::unexpected(Status::NotFound("Key not found (empty tree)"));
    }
    
    // Find the leaf that should contain this key
    auto leaf_result = FindLeaf(key);
    if (!leaf_result) [[unlikely]] {
        return std::unexpected(leaf_result.error());
    }
    
    PageId leaf_id = *leaf_result;
    
    // Read the leaf page
    storage::Page page;
    if (Status read_status = pager_.ReadPage(leaf_id, page); !read_status.ok()) [[unlikely]] {
        return std::unexpected(read_status);
    }
    
    // Search for the key in the leaf
    NodeView node(page);
    auto value_opt = node.GetValue(key);
    
    if (!value_opt.has_value()) [[unlikely]] {
        return std::unexpected(Status::NotFound("Key not found"));
    }
    
    // Return a copy of the value (since page is local)
    return std::string(value_opt.value());
}

// ==============================================================================
// Contains
// ==============================================================================

bool BPlusTree::Contains(std::string_view key) {
    return Get(key).has_value();
}

// ==============================================================================
// Put
// ==============================================================================

Status BPlusTree::Put(std::string_view key, std::string_view value) {
    // -------------------------------------------------------------------------
    // Case 1: Empty tree - create first leaf as root
    // -------------------------------------------------------------------------
    if (IsEmpty()) [[unlikely]] {
        auto alloc_result = pager_.AllocatePage();
        if (!alloc_result) {
            return alloc_result.error();
        }
        
        PageId root_id = *alloc_result;
        
        // Initialize as leaf node
        storage::Page page;
        page.Init(root_id, storage::PageType::kLeaf);
        
        NodeView node(page);
        node.InitLeaf();
        
        // Insert the key-value pair
        if (!node.InsertLeaf(key, value)) [[unlikely]] {
            return Status::Internal("Failed to insert into empty leaf");
        }
        
        // Write the page
        if (Status write_status = pager_.WritePage(root_id, page); !write_status.ok()) {
            return write_status;
        }
        
        // Set as root
        if (Status root_status = pager_.SetRootPageId(root_id); !root_status.ok()) {
            return root_status;
        }
        
        return pager_.Sync();
    }
    
    // -------------------------------------------------------------------------
    // Case 2: Non-empty tree - find path to leaf
    // -------------------------------------------------------------------------
    auto path_result = FindLeafPath(key);
    if (!path_result) [[unlikely]] {
        return path_result.error();
    }
    
    auto path = std::move(*path_result);
    if (path.empty()) [[unlikely]] {
        return Status::Internal("Empty path to leaf");
    }
    
    PageId leaf_id = path.back().page_id;
    
    // Read the leaf
    storage::Page leaf_page;
    if (Status read_status = pager_.ReadPage(leaf_id, leaf_page); !read_status.ok()) {
        return read_status;
    }
    
    NodeView leaf_node(leaf_page);
    
    // -------------------------------------------------------------------------
    // Try to insert or update in the leaf
    // -------------------------------------------------------------------------
    
    // First, check if key already exists
    bool found;
    std::ignore = leaf_node.FindKeyIndex(key, found);
    
    if (found) [[unlikely]] {
        // Update existing key
        if (!leaf_node.UpdateLeaf(key, value)) {
            return Status::Internal("Failed to update existing key");
        }
        
        if (Status write_status = pager_.WritePage(leaf_id, leaf_page); !write_status.ok()) {
            return write_status;
        }
        
        return pager_.Sync();
    }
    
    // Try to insert (may fail if leaf is full)
    if (leaf_node.InsertLeaf(key, value)) [[likely]] {
        // Success - write and return
        if (Status write_status = pager_.WritePage(leaf_id, leaf_page); !write_status.ok()) {
            return write_status;
        }
        
        return pager_.Sync();
    }
    
    // -------------------------------------------------------------------------
    // Case 3: Leaf is full - need to split
    // -------------------------------------------------------------------------
    
    auto split_result = SplitLeaf(leaf_page);
    if (!split_result) [[unlikely]] {
        return split_result.error();
    }
    
    auto&& [new_leaf_id, split_key] = *split_result;
    
    // Determine which leaf gets the new key
    if (key < split_key) {
        // Insert into original leaf
        NodeView original_node(leaf_page);
        if (!original_node.InsertLeaf(key, value)) [[unlikely]] {
            return Status::Internal("Failed to insert after split (left)");
        }
        std::ignore = pager_.WritePage(leaf_id, leaf_page);
    } else {
        // Insert into new leaf
        storage::Page new_page;
        std::ignore = pager_.ReadPage(new_leaf_id, new_page);
        NodeView new_node(new_page);
        if (!new_node.InsertLeaf(key, value)) [[unlikely]] {
            return Status::Internal("Failed to insert after split (right)");
        }
        std::ignore = pager_.WritePage(new_leaf_id, new_page);
    }
    
    // -------------------------------------------------------------------------
    // Propagate split up the tree
    // -------------------------------------------------------------------------
    
    PageId left_child = leaf_id;
    PageId right_child = new_leaf_id;
    std::string key_to_insert = std::move(split_key);
    
    // Walk up the path from leaf to root (using reverse iteration)
    for (auto i = std::ssize(path) - 2; i >= 0; --i) {
        PageId parent_id = path[static_cast<size_t>(i)].page_id;
        
        storage::Page parent_page;
        if (Status parent_read = pager_.ReadPage(parent_id, parent_page); !parent_read.ok()) {
            return parent_read;
        }
        
        NodeView parent_node(parent_page);
        
        // Try to insert the split key into parent
        if (parent_node.InsertInternalAfterSplit(key_to_insert, left_child, right_child)) [[likely]] {
            std::ignore = pager_.WritePage(parent_id, parent_page);
            return pager_.Sync();
        }
        
        // Parent is full - split it too
        auto internal_split = SplitInternal(parent_page);
        if (!internal_split) [[unlikely]] {
            return internal_split.error();
        }
        
        auto&& [new_internal_id, new_split_key] = *internal_split;
        
        // Insert the key into appropriate internal node
        if (key_to_insert < new_split_key) {
            NodeView orig_internal(parent_page);
            std::ignore = orig_internal.InsertInternalAfterSplit(key_to_insert, left_child, right_child);
            std::ignore = pager_.WritePage(parent_id, parent_page);
        } else {
            storage::Page new_internal_page;
            std::ignore = pager_.ReadPage(new_internal_id, new_internal_page);
            NodeView new_internal(new_internal_page);
            std::ignore = new_internal.InsertInternalAfterSplit(key_to_insert, left_child, right_child);
            std::ignore = pager_.WritePage(new_internal_id, new_internal_page);
        }
        
        // Continue propagating
        left_child = parent_id;
        right_child = new_internal_id;
        key_to_insert = std::move(new_split_key);
    }
    
    // -------------------------------------------------------------------------
    // Root split - create new root
    // -------------------------------------------------------------------------
    
    if (Status new_root_status = CreateNewRoot(key_to_insert, left_child, right_child); 
        !new_root_status.ok()) [[unlikely]] {
        return new_root_status;
    }
    
    return pager_.Sync();
}

// ==============================================================================
// FindLeaf
// ==============================================================================

Result<PageId> BPlusTree::FindLeaf(std::string_view key) {
    if (IsEmpty()) [[unlikely]] {
        return std::unexpected(Status::NotFound("Tree is empty"));
    }
    
    PageId current = pager_.root_page_id();
    
    while (true) {
        storage::Page page;
        if (Status read_status = pager_.ReadPage(current, page); !read_status.ok()) [[unlikely]] {
            return std::unexpected(read_status);
        }
        
        NodeView node(page);
        
        if (node.IsLeaf()) [[likely]] {
            return current;
        }
        
        // Internal node - find which child to descend into
        current = node.FindChild(key);
        if (current == kInvalidPageId) [[unlikely]] {
            return std::unexpected(Status::Internal("Invalid child pointer in internal node"));
        }
    }
    
    // This point is unreachable due to infinite loop with returns
    std::unreachable();
}

// ==============================================================================
// FindLeafPath
// ==============================================================================

Result<std::vector<BPlusTree::PathEntry>> BPlusTree::FindLeafPath(std::string_view key) {
    std::vector<PathEntry> path;
    
    if (IsEmpty()) [[unlikely]] {
        return path;
    }
    
    PageId current = pager_.root_page_id();
    
    while (true) {
        storage::Page page;
        if (Status read_status = pager_.ReadPage(current, page); !read_status.ok()) [[unlikely]] {
            return std::unexpected(read_status);
        }
        
        NodeView node(page);
        
        if (node.IsLeaf()) {
            path.push_back({current, 0});
            return path;
        }
        
        // Find which child to descend into
        bool found;
        uint16_t index = node.FindKeyIndex(key, found);
        
        // The child index is the same as key index for keys < node.GetKey(index)
        // Otherwise it's the rightmost child
        PageId child = [&]() -> PageId {
            if (index < node.NumKeys() && key < node.GetKey(index)) {
                return node.GetChild(index);
            }
            if (index > 0 && index <= node.NumKeys()) {
                if (index < node.NumKeys()) {
                    return node.GetChild(index);
                }
                return node.GetRightmostChild();
            }
            return node.GetRightmostChild();
        }();
        
        path.push_back({current, index});
        current = child;
        
        if (current == kInvalidPageId) [[unlikely]] {
            return std::unexpected(Status::Internal("Invalid child pointer"));
        }
    }
    
    std::unreachable();
}

// ==============================================================================
// SplitLeaf
// ==============================================================================

Result<std::pair<PageId, std::string>> BPlusTree::SplitLeaf(storage::Page& page) {
    NodeView node(page);
    uint16_t num_keys = node.NumKeys();
    uint16_t split_index = num_keys / 2;
    
    // Allocate new page for the right half
    auto alloc_result = pager_.AllocatePage();
    if (!alloc_result) [[unlikely]] {
        return std::unexpected(alloc_result.error());
    }
    
    PageId new_page_id = *alloc_result;
    
    // Collect entries for the new page (upper half)
    std::vector<std::pair<std::string, std::string>> right_entries;
    right_entries.reserve(num_keys - split_index);
    
    for (auto i : std::views::iota(split_index, num_keys)) {
        right_entries.emplace_back(std::string(node.GetKey(i)), 
                                   std::string(node.GetValueAt(i)));
    }
    
    // Get the split key (first key of right page)
    std::string split_key(node.GetKey(split_index));
    
    // Collect entries for the left page (lower half)
    std::vector<std::pair<std::string, std::string>> left_entries;
    left_entries.reserve(split_index);
    
    for (auto i : std::views::iota(uint16_t{0}, split_index)) {
        left_entries.emplace_back(std::string(node.GetKey(i)), 
                                  std::string(node.GetValueAt(i)));
    }
    
    // Setup leaf linking
    PageId old_next = node.GetNextLeaf();
    PageId original_page_id = page.page_id();
    
    // Reinitialize original page with left entries
    page.Init(original_page_id, storage::PageType::kLeaf);
    NodeView left_node(page);
    left_node.InitLeaf();
    left_node.SetNextLeaf(new_page_id);
    
    for (const auto& [k, v] : left_entries) {
        std::ignore = left_node.InsertLeaf(k, v);
    }
    
    std::ignore = pager_.WritePage(original_page_id, page);
    
    // Initialize new page with right entries
    storage::Page new_page;
    new_page.Init(new_page_id, storage::PageType::kLeaf);
    NodeView right_node(new_page);
    right_node.InitLeaf();
    right_node.SetPrevLeaf(original_page_id);
    right_node.SetNextLeaf(old_next);
    
    for (const auto& [k, v] : right_entries) {
        std::ignore = right_node.InsertLeaf(k, v);
    }
    
    std::ignore = pager_.WritePage(new_page_id, new_page);
    
    // Update the old next's prev pointer
    if (old_next != kInvalidPageId) {
        storage::Page old_next_page;
        std::ignore = pager_.ReadPage(old_next, old_next_page);
        NodeView old_next_node(old_next_page);
        old_next_node.SetPrevLeaf(new_page_id);
        std::ignore = pager_.WritePage(old_next, old_next_page);
    }
    
    return std::pair{new_page_id, std::move(split_key)};
}

// ==============================================================================
// SplitInternal
// ==============================================================================

Result<std::pair<PageId, std::string>> BPlusTree::SplitInternal(storage::Page& page) {
    NodeView node(page);
    uint16_t num_keys = node.NumKeys();
    uint16_t split_index = num_keys / 2;
    
    // Allocate new page for the right half
    auto alloc_result = pager_.AllocatePage();
    if (!alloc_result) [[unlikely]] {
        return std::unexpected(alloc_result.error());
    }
    
    PageId new_page_id = *alloc_result;
    
    // The middle key goes up to the parent
    std::string split_key(node.GetKey(split_index));
    
    // Collect entries for the right page (keys after split_index)
    // Internal node has keys K0, K1, K2, K3, K4 and children C0, C1, C2, C3, C4, C5
    // After split at index 2 (K2):
    // Left:  K0, K1 with C0, C1, C2
    // Right: K3, K4 with C3, C4, C5
    // K2 goes up to parent
    
    std::vector<std::pair<std::string, PageId>> right_entries;
    right_entries.reserve(num_keys - split_index - 1);
    
    for (auto i : std::views::iota(static_cast<uint16_t>(split_index + 1), num_keys)) {
        right_entries.emplace_back(std::string(node.GetKey(i)), node.GetChild(i));
    }
    
    // Collect left entries
    std::vector<std::pair<std::string, PageId>> left_entries;
    left_entries.reserve(split_index);
    
    for (auto i : std::views::iota(uint16_t{0}, split_index)) {
        left_entries.emplace_back(std::string(node.GetKey(i)), node.GetChild(i));
    }
    
    // The "rightmost" child of left node is the child at split_index
    PageId left_rightmost = node.GetChild(split_index);
    
    // The rightmost child of right node is the old rightmost
    PageId right_rightmost = node.GetRightmostChild();
    
    PageId original_page_id = page.page_id();
    
    // Reinitialize original page with left entries
    page.Init(original_page_id, storage::PageType::kInternal);
    NodeView left_node(page);
    left_node.InitInternal();
    left_node.SetRightmostChild(left_rightmost);
    
    for (const auto& [k, child] : left_entries) {
        std::ignore = left_node.InsertInternal(k, child);
    }
    
    std::ignore = pager_.WritePage(original_page_id, page);
    
    // Initialize new page with right entries
    storage::Page new_page;
    new_page.Init(new_page_id, storage::PageType::kInternal);
    NodeView right_node(new_page);
    right_node.InitInternal();
    right_node.SetRightmostChild(right_rightmost);
    
    for (const auto& [k, child] : right_entries) {
        std::ignore = right_node.InsertInternal(k, child);
    }
    
    std::ignore = pager_.WritePage(new_page_id, new_page);
    
    return std::pair{new_page_id, std::move(split_key)};
}

// ==============================================================================
// CreateNewRoot
// ==============================================================================

Status BPlusTree::CreateNewRoot(std::string_view split_key,
                                 PageId left_child,
                                 PageId right_child) {
    // Allocate new root page
    auto alloc_result = pager_.AllocatePage();
    if (!alloc_result) [[unlikely]] {
        return alloc_result.error();
    }
    
    PageId new_root_id = *alloc_result;
    
    // Initialize as internal node
    storage::Page root_page;
    root_page.Init(new_root_id, storage::PageType::kInternal);
    
    NodeView root_node(root_page);
    root_node.InitInternal();
    
    // Insert the split key with left_child
    std::ignore = root_node.InsertInternal(split_key, left_child);
    
    // Set right_child as the rightmost child
    root_node.SetRightmostChild(right_child);
    
    // Write the new root
    if (Status write_status = pager_.WritePage(new_root_id, root_page); !write_status.ok()) {
        return write_status;
    }
    
    // Update the root page ID in the pager
    return pager_.SetRootPageId(new_root_id);
}

}  // namespace kvstore::btree
