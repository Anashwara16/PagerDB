// ==============================================================================
// kvstore/storage/pager.cc - Page-Level File I/O Implementation (C++23)
// ==============================================================================
//
// This file implements the Pager class which provides page-level access
// to the database file.
//
// Key Operations:
// ---------------
//   Open()        - Open/create database file, validate/init meta page
//   ReadPage()    - Read a page from disk by PageId
//   WritePage()   - Write a page to disk by PageId
//   AllocatePage() - Extend the file and return new PageId
//
// File Format:
// ------------
//   The database file is an array of fixed-size pages (4KB each).
//   Page 0 is the meta page containing database metadata.
//   Pages 1+ are data pages (B+ tree nodes).
//
//   To read page N: seek to offset (N * 4096), read 4096 bytes
//   To write page N: seek to offset (N * 4096), write 4096 bytes
//
// C++23 Features Used:
// --------------------
//   - [[likely]] / [[unlikely]] for branch prediction hints
//   - std::expected for error handling
//   - std::unexpected for error returns
//
// ==============================================================================

#include "kvstore/storage/pager.h"

#include <cstring>  // memset, memcpy
#include <utility>  // std::move

namespace kvstore::storage {

// ==============================================================================
// Helper: Convert FileError to Status
// ==============================================================================
// The file layer uses FileError, but the pager layer uses Status.
// This helper bridges the two error types.

static Status FileErrorToStatus(const FileError& err) {
    switch (err.kind) {
        case FileError::Kind::NotFound:
            return Status::NotFound(err.message);
        case FileError::Kind::IoError:
            return Status::IoError(err.message);
        case FileError::Kind::InternalError:
            return Status::Internal(err.message);
    }
    return Status::Internal("Unknown file error");
}

// ==============================================================================
// Private Constructor
// ==============================================================================

Pager::Pager(PosixFile file, MetaPage meta)
    : file_(std::move(file))
    , meta_(meta)
{}

// ==============================================================================
// Move Operations
// ==============================================================================

Pager::Pager(Pager&& other) noexcept
    : file_(std::move(other.file_))
    , meta_(other.meta_)
{
    // Invalidate other's meta to catch use-after-move bugs
    other.meta_.magic = 0;
}

Pager& Pager::operator=(Pager&& other) noexcept {
    if (this != &other) [[likely]] {
        file_ = std::move(other.file_);
        meta_ = other.meta_;
        other.meta_.magic = 0;
    }
    return *this;
}

// ==============================================================================
// Open
// ==============================================================================

Result<Pager> Pager::Open(const std::string& path, bool create_if_missing) {
    // -------------------------------------------------------------------------
    // Step 1: Open the file
    // -------------------------------------------------------------------------
    auto file_result = PosixFile::Open(path, create_if_missing);
    if (!file_result) [[unlikely]] {
        return std::unexpected(FileErrorToStatus(file_result.error()));
    }
    
    PosixFile file = std::move(*file_result);
    
    // -------------------------------------------------------------------------
    // Step 2: Check file size to determine if this is a new database
    // -------------------------------------------------------------------------
    auto size_result = file.Size();
    if (!size_result) [[unlikely]] {
        return std::unexpected(FileErrorToStatus(size_result.error()));
    }
    uint64_t file_size = *size_result;
    
    MetaPage meta{};
    
    if (file_size == 0) [[unlikely]] {
        // ---------------------------------------------------------------------
        // New database: Initialize meta page
        // ---------------------------------------------------------------------
        meta.Init();
        
        // Write the meta page to disk
        auto write_result = file.Write(0, &meta, sizeof(MetaPage));
        if (!write_result) [[unlikely]] {
            return std::unexpected(FileErrorToStatus(write_result.error()));
        }
        
        // Sync to ensure the meta page is durable
        auto sync_result = file.Sync();
        if (!sync_result) [[unlikely]] {
            return std::unexpected(FileErrorToStatus(sync_result.error()));
        }
    } else {
        // ---------------------------------------------------------------------
        // Existing database: Read and validate meta page
        // ---------------------------------------------------------------------
        if (file_size < kPageSize) [[unlikely]] {
            return std::unexpected(Status::Corruption(
                "Database file too small: " + std::to_string(file_size) + " bytes"
            ));
        }
        
        // Read the meta page
        auto read_result = file.Read(0, &meta, sizeof(MetaPage));
        if (!read_result) [[unlikely]] {
            return std::unexpected(FileErrorToStatus(read_result.error()));
        }
        
        // Validate magic number
        if (meta.magic != MetaPage::kMagicNumber) [[unlikely]] {
            return std::unexpected(Status::Corruption(
                "Invalid database file: bad magic number"
            ));
        }
        
        // Validate version
        if (meta.version != MetaPage::kFormatVersion) [[unlikely]] {
            return std::unexpected(Status::Corruption(
                "Unsupported database version: " + std::to_string(meta.version)
            ));
        }
        
        // Validate file size matches expected page count
        uint64_t expected_size = static_cast<uint64_t>(meta.num_pages) * kPageSize;
        if (file_size < expected_size) [[unlikely]] {
            return std::unexpected(Status::Corruption(
                "Database file truncated: expected " + std::to_string(expected_size) +
                " bytes, got " + std::to_string(file_size)
            ));
        }
    }
    
    // -------------------------------------------------------------------------
    // Step 3: Create and return the Pager
    // -------------------------------------------------------------------------
    return Pager(std::move(file), meta);
}

// ==============================================================================
// ReadPage
// ==============================================================================

Status Pager::ReadPage(PageId page_id, Page& out) const {
    // -------------------------------------------------------------------------
    // Validate page_id
    // -------------------------------------------------------------------------
    if (page_id >= meta_.num_pages) [[unlikely]] {
        return Status::InvalidArgument(
            "Page " + std::to_string(page_id) + " out of range (max: " +
            std::to_string(meta_.num_pages - 1) + ")"
        );
    }
    
    // -------------------------------------------------------------------------
    // Read the page
    // -------------------------------------------------------------------------
    // Calculate file offset: page_id * page_size
    uint64_t offset = PageOffset(page_id);
    
    // Read directly into the Page's raw data
    auto read_result = file_.Read(offset, out.RawData(), kPageSize);
    if (!read_result) [[unlikely]] {
        return FileErrorToStatus(read_result.error());
    }
    
    return Status::Ok();
}

// ==============================================================================
// WritePage
// ==============================================================================

Status Pager::WritePage(PageId page_id, const Page& page) {
    // -------------------------------------------------------------------------
    // Validate page_id
    // -------------------------------------------------------------------------
    if (page_id >= meta_.num_pages) [[unlikely]] {
        return Status::InvalidArgument(
            "Page " + std::to_string(page_id) + " out of range (max: " +
            std::to_string(meta_.num_pages - 1) + ")"
        );
    }
    
    // -------------------------------------------------------------------------
    // Write the page
    // -------------------------------------------------------------------------
    uint64_t offset = PageOffset(page_id);
    
    auto write_result = file_.Write(offset, page.RawData(), kPageSize);
    if (!write_result) [[unlikely]] {
        return FileErrorToStatus(write_result.error());
    }
    
    return Status::Ok();
}

// ==============================================================================
// AllocatePage
// ==============================================================================

Result<PageId> Pager::AllocatePage() {
    // -------------------------------------------------------------------------
    // Get the new page ID
    // -------------------------------------------------------------------------
    // New page ID is simply the current page count
    // (Pages are numbered 0, 1, 2, ... so if we have N pages, next is N)
    PageId new_page_id = meta_.num_pages;
    
    // -------------------------------------------------------------------------
    // Write a zeroed page to extend the file
    // -------------------------------------------------------------------------
    // We write zeros to ensure the file is actually extended
    // (some filesystems won't extend on Truncate alone)
    Page empty_page;
    std::memset(empty_page.RawData(), 0, kPageSize);
    
    uint64_t offset = PageOffset(new_page_id);
    auto write_result = file_.Write(offset, empty_page.RawData(), kPageSize);
    if (!write_result) [[unlikely]] {
        return std::unexpected(FileErrorToStatus(write_result.error()));
    }
    
    // -------------------------------------------------------------------------
    // Update meta page
    // -------------------------------------------------------------------------
    meta_.num_pages = new_page_id + 1;
    
    Status meta_status = WriteMetaPage();
    if (!meta_status.ok()) [[unlikely]] {
        return std::unexpected(meta_status);
    }
    
    return new_page_id;
}

// ==============================================================================
// SetRootPageId
// ==============================================================================

Status Pager::SetRootPageId(PageId new_root) {
    meta_.root_page_id = new_root;
    return WriteMetaPage();
}

// ==============================================================================
// SetNumKeys
// ==============================================================================

Status Pager::SetNumKeys(uint64_t count) {
    meta_.num_keys = count;
    return WriteMetaPage();
}

// ==============================================================================
// WriteMetaPage
// ==============================================================================

Status Pager::WriteMetaPage() {
    // Write the meta page at offset 0
    auto write_result = file_.Write(0, &meta_, sizeof(MetaPage));
    if (!write_result) {
        return FileErrorToStatus(write_result.error());
    }
    return Status::Ok();
}

// ==============================================================================
// Sync
// ==============================================================================

Status Pager::Sync() {
    auto sync_result = file_.Sync();
    if (!sync_result) {
        return FileErrorToStatus(sync_result.error());
    }
    return Status::Ok();
}

}  // namespace kvstore::storage
