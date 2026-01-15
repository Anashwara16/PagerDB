// ==============================================================================
// kvstore/db.cc - Database Implementation (C++23)
// ==============================================================================
//
// This file implements the DB interface using:
//   - Pager for page-level I/O
//   - BPlusTree for key-value storage
//
// File Layout:
// ------------
// The database is stored in a single file:
//   <path>/data.db
//
// Future versions might add:
//   <path>/wal.log    - Write-ahead log for crash recovery
//   <path>/lock       - File lock for single-writer guarantee
//
// C++23 Features Used:
// --------------------
//   - std::expected (via Result type pattern)
//   - [[likely]] / [[unlikely]] attributes
//   - Improved designated initializers
//   - std::unreachable() for impossible code paths
//
// ==============================================================================

#include "kvstore/db.h"

#include <expected>
#include <filesystem>
#include <utility>

#include "kvstore/storage/pager.h"
#include "kvstore/btree/btree.h"

namespace kvstore {

// ==============================================================================
// DbImpl Class
// ==============================================================================
//
// The concrete implementation of the DB interface.

class DbImpl final : public DB {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------
    
    DbImpl(std::string path, Options options, storage::Pager pager)
        : path_(std::move(path))
        , options_(options)
        , pager_(std::move(pager))
        , tree_(pager_)
    {}
    
    // -------------------------------------------------------------------------
    // DB Interface Implementation
    // -------------------------------------------------------------------------
    
    [[nodiscard]]
    Result<std::string> Get(std::string_view key) override {
        return tree_.Get(key);
    }
    
    [[nodiscard]]
    Status Put(std::string_view key, std::string_view value) override {
        if (Status s = tree_.Put(key, value); !s.ok()) [[unlikely]] {
            return s;
        }
        
        // Sync if configured to do so
        if (options_.sync_on_write) [[likely]] {
            return pager_.Sync();
        }
        
        return Status::Ok();
    }
    
    [[nodiscard]]
    Status Delete([[maybe_unused]] std::string_view key) override {
        // MVP: Delete is not implemented
        // A full implementation would:
        //   1. Find the key in the B+ tree
        //   2. Mark it as deleted (tombstone)
        //   3. Optionally compact the tree later
        return Status::Internal("Delete not implemented in MVP");
    }
    
    [[nodiscard]]
    bool Contains(std::string_view key) override {
        return tree_.Contains(key);
    }
    
    [[nodiscard]]
    Status Sync() override {
        return pager_.Sync();
    }
    
    [[nodiscard]]
    uint64_t ApproximateKeyCount() const override {
        return pager_.num_keys();
    }
    
    [[nodiscard]]
    const std::string& GetPath() const override {
        return path_;
    }

private:
    std::string path_;
    Options options_;
    storage::Pager pager_;
    btree::BPlusTree tree_;
};

// ==============================================================================
// DB::Open Implementation
// ==============================================================================

Result<std::unique_ptr<DB>> DB::Open(const std::string& path, const Options& options) {
    namespace fs = std::filesystem;
    
    // -------------------------------------------------------------------------
    // Step 1: Ensure the directory exists
    // -------------------------------------------------------------------------
    fs::path dir_path(path);
    
    if (fs::exists(dir_path)) {
        // Path exists - make sure it's a directory
        if (!fs::is_directory(dir_path)) [[unlikely]] {
            return std::unexpected(Status::InvalidArgument("Path exists but is not a directory: " + path));
        }
    } else {
        // Path doesn't exist
        if (!options.create_if_missing) [[unlikely]] {
            return std::unexpected(Status::NotFound("Database directory not found: " + path));
        }
        
        // Create the directory
        std::error_code ec;
        if (!fs::create_directories(dir_path, ec)) [[unlikely]] {
            return std::unexpected(Status::IoError("Failed to create directory: " + path + " - " + ec.message()));
        }
    }
    
    // -------------------------------------------------------------------------
    // Step 2: Open the data file
    // -------------------------------------------------------------------------
    auto data_file_path = (dir_path / "data.db").string();
    
    auto pager_result = storage::Pager::Open(data_file_path, options.create_if_missing);
    if (!pager_result) [[unlikely]] {
        return std::unexpected(pager_result.error());
    }
    
    auto pager = std::move(*pager_result);
    
    // -------------------------------------------------------------------------
    // Step 3: Create the DbImpl instance
    // -------------------------------------------------------------------------
    std::unique_ptr<DB> db = std::make_unique<DbImpl>(
        path, 
        options, 
        std::move(pager)
    );
    
    return db;
}

}  // namespace kvstore
