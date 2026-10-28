// ==============================================================================
// tools/kvstore_inspect.cc - On-disk database inspector (Phase 1 task 1.15)
// ==============================================================================
//
// Usage:
//   kvstore_inspect <db-dir | data.db path>
//
// Prints the meta page, the B+ tree structure (recursively, with indentation),
// and verifies the leaf sibling chain (forward order + prev back-links).
// Read-only; checksums are verified on every page read.
//
// ==============================================================================

#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "kvstore/btree/node.h"
#include "kvstore/storage/pager.h"

namespace {

namespace fs = std::filesystem;
using kvstore::PageId;
using kvstore::kInvalidPageId;
using kvstore::btree::NodeView;
using kvstore::storage::Page;
using kvstore::storage::Pager;

// Recursively print a subtree. Returns false if any page fails to load or
// fails validation. Every visited page ID is recorded in `tree_pages` so the
// free-list check can verify disjointness.
bool DumpSubtree(Pager& pager, PageId page_id, int depth,
                 std::vector<PageId>& leaves_in_tree_order,
                 std::set<PageId>& tree_pages) {
    Page page;
    if (auto s = pager.ReadPage(page_id, page); !s.ok()) {
        std::cerr << std::string(static_cast<size_t>(depth) * 2, ' ')
                  << "!! page " << page_id << ": " << s.message() << "\n";
        return false;
    }

    const std::string indent(static_cast<size_t>(depth) * 2, ' ');
    NodeView node(page);

    if (!tree_pages.insert(page_id).second) {
        std::cerr << indent << "!! page " << page_id
                  << " reachable twice from the root (cycle?)\n";
        return false;
    }

    if (!page.Validate()) {
        std::cerr << indent << "!! page " << page_id
                  << " fails structural validation\n";
        page.Dump(std::cerr);
        return false;
    }

    if (node.IsLeaf()) {
        std::cout << indent << "leaf " << page_id
                  << ": " << node.NumKeys() << " keys";
        if (node.NumKeys() > 0) {
            std::cout << " [" << node.GetKey(0) << " .. "
                      << node.GetKey(static_cast<uint16_t>(node.NumKeys() - 1))
                      << "]";
        }
        std::cout << " next=" << node.GetNextLeaf()
                  << " prev=" << node.GetPrevLeaf() << "\n";
        leaves_in_tree_order.push_back(page_id);
        return true;
    }

    std::cout << indent << "internal " << page_id
              << ": " << node.NumKeys() << " separators"
              << " rightmost=" << node.GetRightmostChild() << "\n";

    bool ok = true;
    for (uint16_t i = 0; i < node.NumKeys(); ++i) {
        std::cout << indent << "  < " << node.GetKey(i) << ":\n";
        ok = DumpSubtree(pager, node.GetChild(i), depth + 2,
                         leaves_in_tree_order, tree_pages) && ok;
    }
    std::cout << indent << "  >= "
              << (node.NumKeys() > 0
                      ? node.GetKey(static_cast<uint16_t>(node.NumKeys() - 1))
                      : std::string_view("(none)"))
              << ":\n";
    ok = DumpSubtree(pager, node.GetRightmostChild(), depth + 2,
                     leaves_in_tree_order, tree_pages) && ok;
    return ok;
}

// Walk the free list (Phase 2): every page must be typed kFree, the list
// must be acyclic, and no free page may be reachable from the root.
bool CheckFreeList(Pager& pager, const std::set<PageId>& tree_pages) {
    std::set<PageId> seen;
    for (PageId current = pager.free_list_head(); current != kInvalidPageId;) {
        if (!seen.insert(current).second) {
            std::cerr << "!! free list cycle at page " << current << "\n";
            return false;
        }
        if (tree_pages.contains(current)) {
            std::cerr << "!! free page " << current
                      << " is reachable from the root\n";
            return false;
        }

        Page page;
        if (auto s = pager.ReadPage(current, page); !s.ok()) {
            std::cerr << "!! free list read " << current << ": "
                      << s.message() << "\n";
            return false;
        }
        if (page.type() != kvstore::storage::PageType::kFree) {
            std::cerr << "!! free list page " << current
                      << " is not typed kFree\n";
            return false;
        }

        // Next pointer lives in the first 4 payload bytes.
        std::memcpy(&current,
                    page.RawData() + sizeof(kvstore::storage::PageHeader),
                    sizeof(current));
    }

    std::cout << "free list: " << seen.size()
              << " pages, acyclic, disjoint from tree\n";
    return true;
}

// Walk the leaf chain via next_page/prev_page and compare with the leaves
// found by tree traversal (which visits them in key order).
bool CheckLeafChain(Pager& pager, const std::vector<PageId>& tree_leaves) {
    if (tree_leaves.empty()) {
        std::cout << "leaf chain: (no leaves)\n";
        return true;
    }

    std::vector<PageId> forward;
    PageId prev_expected = kInvalidPageId;
    for (PageId current = tree_leaves.front(); current != kInvalidPageId;) {
        Page page;
        if (auto s = pager.ReadPage(current, page); !s.ok()) {
            std::cerr << "!! leaf chain read " << current << ": "
                      << s.message() << "\n";
            return false;
        }
        NodeView node(page);
        if (node.GetPrevLeaf() != prev_expected) {
            std::cerr << "!! leaf " << current << " prev=" << node.GetPrevLeaf()
                      << " expected " << prev_expected << "\n";
            return false;
        }
        forward.push_back(current);
        prev_expected = current;
        current = node.GetNextLeaf();
        if (forward.size() > tree_leaves.size() + 1) {
            std::cerr << "!! leaf chain longer than tree traversal (cycle?)\n";
            return false;
        }
    }

    if (forward != tree_leaves) {
        std::cerr << "!! leaf chain order differs from tree key order\n";
        return false;
    }

    std::cout << "leaf chain: " << forward.size()
              << " leaves, forward/backward links consistent\n";
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: kvstore_inspect <db-dir | data.db path>\n";
        return 2;
    }

    // Accept either the database directory or the data file itself.
    fs::path path(argv[1]);
    if (fs::is_directory(path)) {
        path /= "data.db";
    }

    auto pager_result = Pager::Open(path.string(), /*create_if_missing=*/false);
    if (!pager_result) {
        std::cerr << "open failed: " << pager_result.error().message() << "\n";
        return 1;
    }
    Pager pager = std::move(*pager_result);

    std::cout << "file: " << path.string() << "\n"
              << "format version: "
              << kvstore::storage::MetaPage::kFormatVersion << "\n"
              << "meta generation: " << pager.generation()
              << " (v3 dual meta: active copy = page "
              << (pager.generation() % 2) << ")\n"
              << "checkpoint_lsn: " << pager.checkpoint_lsn() << "\n"
              << "num_pages: " << pager.num_pages() << "\n"
              << "num_keys: " << pager.num_keys() << "\n"
              << "root_page_id: " << pager.root_page_id() << "\n"
              << "free_list_head: " << pager.free_list_head() << "\n\n";

    std::set<PageId> tree_pages;
    bool ok = true;

    if (pager.root_page_id() == kInvalidPageId) {
        std::cout << "(empty tree)\n";
    } else {
        std::vector<PageId> leaves;
        ok = DumpSubtree(pager, pager.root_page_id(), 0, leaves, tree_pages);
        std::cout << "\n";
        ok = CheckLeafChain(pager, leaves) && ok;
    }

    ok = CheckFreeList(pager, tree_pages) && ok;

    return ok ? 0 : 1;
}
