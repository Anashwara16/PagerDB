// ==============================================================================
// kvstore/btree/btree.h - B+ Tree Implementation (C++23)
// ==============================================================================
//
// This file implements a B+ tree for the key-value store. The B+ tree provides:
//   - O(log n) search, insert, and delete
//   - Sorted key order (enables range scans)
//   - All values stored in leaf nodes (efficient for range queries)
//
// B+ Tree Structure:
// ------------------
//
//                    [Internal Node]
//                    keys: [M]
//                   /          \
//          [Leaf A]            [Leaf B]
//        keys: [A,B,C]       keys: [M,N,O]
//        values: [...]       values: [...]
//              |                   |
//              +--->---next--->----+  (leaf linked list)
//
// Key Properties:
//   1. All values are in leaf nodes
//   2. Internal nodes only store keys and child pointers
//   3. Leaf nodes are linked for efficient range scans
//   4. Keys are sorted within each node
//   5. For internal node key K: left child has keys < K, right child has keys >= K
//
// MVP Simplifications:
// --------------------
//   - No delete operation (keys can only be added)
//   - No concurrent access (single-threaded)
//   - No transactions (operations are immediate)
//
// C++23 Features Used:
// --------------------
//   - [[nodiscard("reason")]] with explanatory messages
//   - std::unreachable() for impossible code paths
//   - Deducing this for cleaner accessor methods
//   - std::ranges algorithms
//
// ==============================================================================

#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <utility>

#include "kvstore/types.h"
#include "kvstore/status.h"
#include "kvstore/storage/pager.h"
#include "kvstore/btree/node.h"

namespace kvstore::btree {

// ==============================================================================
// BPlusTree Class
// ==============================================================================
//
// Usage:
//   Pager pager = Pager::Open("db.dat", true).value();
//   BPlusTree tree(pager);
//
//   // Insert
//   tree.Put("name", "Alice");
//   tree.Put("age", "30");
//
//   // Lookup
//   auto result = tree.Get("name");
//   if (result.ok()) {
//       std::cout << result.value() << std::endl;  // "Alice"
//   }

class BPlusTree {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------
    
    // Create a B+ tree using the given pager
    // The pager must already be opened
    explicit BPlusTree(storage::Pager& pager);
    
    // -------------------------------------------------------------------------
    // Core Operations
    // -------------------------------------------------------------------------
    
    // Get the value for a key
    //
    // Returns:
    //   On success: The value associated with the key
    //   On not found: Status::NotFound
    //   On error: Status with error details
    [[nodiscard("Get returns the value - ignoring it wastes the lookup")]]
    Result<std::string> Get(std::string_view key);
    
    // Insert or update a key-value pair
    //
    // If key exists: value is updated
    // If key doesn't exist: key-value pair is inserted
    //
    // Returns:
    //   Status::Ok() on success
    //   Status::IoError() on disk error
    [[nodiscard("Put returns status - check for errors")]]
    Status Put(std::string_view key, std::string_view value);
    
    // Check if a key exists
    //
    // Returns:
    //   true if key exists
    //   false if key doesn't exist
    [[nodiscard("Contains result should be used")]]
    bool Contains(std::string_view key);
    
    // -------------------------------------------------------------------------
    // Tree Information (using deducing this for const-correctness)
    // -------------------------------------------------------------------------
    
    // Get the root page ID
    [[nodiscard("root_page_id returns a value")]]
    constexpr auto root_page_id(this auto&& self) {
        return self.pager_.root_page_id();
    }
    
    // Check if the tree is empty
    [[nodiscard("IsEmpty returns a boolean condition")]]
    constexpr auto IsEmpty(this auto&& self) {
        return self.pager_.root_page_id() == kInvalidPageId;
    }

private:
    storage::Pager& pager_;
    
    // -------------------------------------------------------------------------
    // Internal Operations
    // -------------------------------------------------------------------------
    
    // Find the leaf page that should contain a key
    // Returns the PageId of the leaf, or kInvalidPageId if tree is empty
    [[nodiscard("FindLeaf returns the target page")]]
    Result<PageId> FindLeaf(std::string_view key);
    
    // Insert into a leaf node, handling splits if necessary
    // Returns the new root page ID if root changed, otherwise kInvalidPageId
    struct InsertResult {
        bool split_occurred;
        std::string split_key;      // Key to insert into parent
        PageId new_page_id;         // New page created by split
    };
    
    [[nodiscard]]
    Result<InsertResult> InsertIntoLeaf(PageId leaf_id, 
                                         std::string_view key, 
                                         std::string_view value);
    
    // Insert into an internal node after a child split
    [[nodiscard]]
    Result<InsertResult> InsertIntoInternal(PageId internal_id,
                                             std::string_view key,
                                             PageId left_child,
                                             PageId right_child);
    
    // Split a leaf node
    // Returns: (new_page_id, split_key)
    // The split_key should be inserted into the parent
    // new_page gets the upper half of keys
    [[nodiscard("Split returns the new page info")]]
    Result<std::pair<PageId, std::string>> SplitLeaf(storage::Page& page);
    
    // Split an internal node
    [[nodiscard("Split returns the new page info")]]
    Result<std::pair<PageId, std::string>> SplitInternal(storage::Page& page);
    
    // Create a new root after the old root splits
    [[nodiscard("CreateNewRoot returns status")]]
    Status CreateNewRoot(std::string_view split_key, 
                         PageId left_child, 
                         PageId right_child);
    
    // Traverse from root to leaf, recording the path
    // path[0] is root, path[n-1] is the leaf
    struct PathEntry {
        PageId page_id;
        uint16_t child_index;  // Which child we descended into
    };
    
    [[nodiscard("FindLeafPath returns the traversal path")]]
    Result<std::vector<PathEntry>> FindLeafPath(std::string_view key);
};

}  // namespace kvstore::btree
