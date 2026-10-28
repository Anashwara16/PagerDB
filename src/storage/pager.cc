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

#include <cstddef>  // offsetof
#include <cstring>  // memset, memcpy
#include <utility>  // std::move

namespace kvstore::storage {

// CRC32 of the meta page with its checksum field treated as zero.
static uint32_t ComputeMetaChecksum(const MetaPage& meta) noexcept {
    return Crc32WithZeroedField(&meta, sizeof(MetaPage),
                                offsetof(MetaPage, checksum));
}

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

Pager::Pager(PosixFile file, MetaPage meta, bool verify_checksums)
    : file_(std::move(file))
    , meta_(meta)
    , verify_checksums_(verify_checksums)
{}

// ==============================================================================
// Move Operations
// ==============================================================================

Pager::Pager(Pager&& other) noexcept
    : file_(std::move(other.file_))
    , meta_(other.meta_)
    , verify_checksums_(other.verify_checksums_)
{
    // Invalidate other's meta to catch use-after-move bugs
    other.meta_.magic = 0;
}

Pager& Pager::operator=(Pager&& other) noexcept {
    if (this != &other) [[likely]] {
        file_ = std::move(other.file_);
        meta_ = other.meta_;
        verify_checksums_ = other.verify_checksums_;
        other.meta_.magic = 0;
    }
    return *this;
}

// ==============================================================================
// Open
// ==============================================================================

Result<Pager> Pager::Open(const std::string& path, bool create_if_missing,
                          bool verify_checksums) {
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
        // New database: initialize BOTH meta copies (v3 dual meta)
        // ---------------------------------------------------------------------
        meta.Init();
        meta.checksum = ComputeMetaChecksum(meta);

        for (PageId slot = 0; slot < Pager::kFirstDataPage; ++slot) {
            auto write_result = file.Write(
                static_cast<uint64_t>(slot) * kPageSize, &meta, sizeof(MetaPage));
            if (!write_result) [[unlikely]] {
                return std::unexpected(FileErrorToStatus(write_result.error()));
            }
        }
        
        // Sync to ensure the meta pages are durable
        auto sync_result = file.Sync();
        if (!sync_result) [[unlikely]] {
            return std::unexpected(FileErrorToStatus(sync_result.error()));
        }
    } else {
        // ---------------------------------------------------------------------
        // Existing database: read both meta copies, pick the valid one with
        // the higher generation (a torn write can kill at most one copy)
        // ---------------------------------------------------------------------
        if (file_size < Pager::kFirstDataPage * kPageSize) [[unlikely]] {
            return std::unexpected(Status::Corruption(
                "Database file too small: " + std::to_string(file_size) + " bytes"
            ));
        }
        
        bool have_valid = false;
        for (PageId slot = 0; slot < Pager::kFirstDataPage; ++slot) {
            MetaPage candidate{};
            auto read_result = file.Read(
                static_cast<uint64_t>(slot) * kPageSize, &candidate, sizeof(MetaPage));
            if (!read_result) [[unlikely]] {
                return std::unexpected(FileErrorToStatus(read_result.error()));
            }
            if (candidate.magic != MetaPage::kMagicNumber ||
                candidate.version != MetaPage::kFormatVersion) {
                continue;  // Copy invalid (torn, stale format, or garbage)
            }
            if (verify_checksums &&
                candidate.checksum != ComputeMetaChecksum(candidate)) {
                continue;  // Copy corrupt
            }
            if (!have_valid || candidate.generation > meta.generation) {
                meta = candidate;
                have_valid = true;
            }
        }
        
        if (!have_valid) [[unlikely]] {
            return std::unexpected(Status::Corruption(
                "No valid meta page (bad magic, version, or checksum in both copies)"
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
    return Pager(std::move(file), meta, verify_checksums);
}

// ==============================================================================
// ReadPage
// ==============================================================================

Status Pager::ReadPage(PageId page_id, Page& out) const {
    // -------------------------------------------------------------------------
    // Validate page_id (v3: pages 0 and 1 are the meta copies)
    // -------------------------------------------------------------------------
    if (page_id < kFirstDataPage) [[unlikely]] {
        return Status::InvalidArgument(
            "Page " + std::to_string(page_id) + " is a meta page");
    }
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

    // -------------------------------------------------------------------------
    // Verify checksum (all-zero pages are exempt: allocated, never written)
    // -------------------------------------------------------------------------
    if (verify_checksums_ &&
        out.header()->checksum != out.ComputeChecksum() &&
        !out.IsZeroed()) [[unlikely]] {
        return Status::Corruption(
            "Checksum mismatch on page " + std::to_string(page_id)
        );
    }

    return Status::Ok();
}

// ==============================================================================
// WritePage
// ==============================================================================

Status Pager::WritePage(PageId page_id, Page& page) {
    // -------------------------------------------------------------------------
    // Validate page_id (v3: pages 0 and 1 are the meta copies)
    // -------------------------------------------------------------------------
    if (page_id < kFirstDataPage) [[unlikely]] {
        return Status::InvalidArgument(
            "Page " + std::to_string(page_id) + " is a meta page");
    }
    if (page_id >= meta_.num_pages) [[unlikely]] {
        return Status::InvalidArgument(
            "Page " + std::to_string(page_id) + " out of range (max: " +
            std::to_string(meta_.num_pages - 1) + ")"
        );
    }

    // -------------------------------------------------------------------------
    // Stamp the checksum, then write the page
    // -------------------------------------------------------------------------
    page.header()->checksum = page.ComputeChecksum();

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
    // Fast path: recycle the most recently freed page (LIFO)
    // -------------------------------------------------------------------------
    if (meta_.free_list_head != kInvalidPageId) {
        PageId recycled = meta_.free_list_head;

        Page free_page;
        if (Status s = ReadPage(recycled, free_page); !s.ok()) [[unlikely]] {
            return std::unexpected(s);
        }
        if (free_page.type() != PageType::kFree) [[unlikely]] {
            return std::unexpected(Status::Corruption(
                "Free list head " + std::to_string(recycled) +
                " is not a free page"));
        }

        // The next free page is stored in the first 4 payload bytes.
        uint32_t next;
        std::memcpy(&next, free_page.RawData() + sizeof(PageHeader), sizeof(next));

        // Re-zero the page on disk so it reads back like a fresh allocation
        // (all-zero pages are exempt from checksum verification).
        Page empty_page;
        std::memset(empty_page.RawData(), 0, kPageSize);
        auto write_result =
            file_.Write(PageOffset(recycled), empty_page.RawData(), kPageSize);
        if (!write_result) [[unlikely]] {
            return std::unexpected(FileErrorToStatus(write_result.error()));
        }

        meta_.free_list_head = next;  // In memory; persisted on Sync/checkpoint
        return recycled;
    }

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
    // Update meta (in memory only - v3 defers persistence to Sync/checkpoint;
    // recovery replays kAllocPage records to re-extend after a crash)
    // -------------------------------------------------------------------------
    meta_.num_pages = new_page_id + 1;
    
    return new_page_id;
}

// ==============================================================================
// FreePage
// ==============================================================================

Status Pager::FreePage(PageId page_id) {
    // The meta pages can never be freed; out-of-range ids are caller bugs.
    if (page_id < kFirstDataPage || page_id >= meta_.num_pages) [[unlikely]] {
        return Status::InvalidArgument(
            "Cannot free page " + std::to_string(page_id) + " (num_pages: " +
            std::to_string(meta_.num_pages) + ")");
    }

    // Rewrite the page as a free-list node: type kFree, with the previous
    // head in the first 4 payload bytes (little-endian).
    Page page;
    page.Init(page_id, PageType::kFree);
    uint32_t next = meta_.free_list_head;
    std::memcpy(page.RawData() + sizeof(PageHeader), &next, sizeof(next));

    if (Status s = WritePage(page_id, page); !s.ok()) [[unlikely]] {
        return s;
    }

    meta_.free_list_head = page_id;  // In memory; persisted on Sync/checkpoint
    return Status::Ok();
}

// ==============================================================================
// SetRootPageId
// ==============================================================================

Status Pager::SetRootPageId(PageId new_root) {
    meta_.root_page_id = new_root;  // In memory; persisted on Sync/checkpoint
    return Status::Ok();
}

// ==============================================================================
// SetNumKeys
// ==============================================================================

Status Pager::SetNumKeys(uint64_t count) {
    std::lock_guard<std::mutex> lg(*meta_mu_);
    meta_.num_keys = count;  // In memory; persisted on Sync/checkpoint
    return Status::Ok();
}

Status Pager::AddToNumKeys(int64_t delta) {
    std::lock_guard<std::mutex> lg(*meta_mu_);
    meta_.num_keys = static_cast<uint64_t>(
        static_cast<int64_t>(meta_.num_keys) + delta);
    return Status::Ok();
}

uint64_t Pager::KeyCount() const {
    std::lock_guard<std::mutex> lg(*meta_mu_);
    return meta_.num_keys;
}

Status Pager::SetCheckpointLsn(Lsn lsn) {
    meta_.checkpoint_lsn = lsn;  // In memory; persisted on Sync/checkpoint
    return Status::Ok();
}

// ==============================================================================
// PersistMeta (v3 dual meta)
// ==============================================================================

Status Pager::PersistMeta() {
    // Bump the generation and write to the alternate slot. The previous
    // generation's copy stays intact, so a torn write here can only lose the
    // newest update, never the last good state.
    //
    // Snapshot meta_ under meta_mu_ so a concurrent KeyCount() read (the only
    // reader that may run alongside a checkpoint) never sees a torn field.
    // Structural writers (SetRootPageId, AllocatePage) are excluded by the
    // DB-level checkpoint latch while this runs.
    MetaPage snapshot;
    {
        std::lock_guard<std::mutex> lg(*meta_mu_);
        ++meta_.generation;
        meta_.checksum = ComputeMetaChecksum(meta_);
        snapshot = meta_;
    }

    const uint64_t offset =
        static_cast<uint64_t>(snapshot.generation % 2) * kPageSize;
    auto write_result = file_.Write(offset, &snapshot, sizeof(MetaPage));
    if (!write_result) {
        // Roll the bump back so a retry does not skip a slot.
        std::lock_guard<std::mutex> lg(*meta_mu_);
        --meta_.generation;
        return FileErrorToStatus(write_result.error());
    }
    return Status::Ok();
}

// ==============================================================================
// Sync
// ==============================================================================

Status Pager::Sync() {
    if (Status s = PersistMeta(); !s.ok()) [[unlikely]] {
        return s;
    }
    return SyncData();
}

Status Pager::SyncData() {
    auto sync_result = file_.Sync();
    if (!sync_result) {
        return FileErrorToStatus(sync_result.error());
    }
    return Status::Ok();
}

}  // namespace kvstore::storage
