// ==============================================================================
// kvstore/wal/log_reader.cc - WAL Scan Implementation (Phase 4)
// ==============================================================================

#include "kvstore/wal/log_reader.h"

#include <filesystem>
#include <span>
#include <utility>

#include "kvstore/storage/file.h"

namespace kvstore::wal {

Result<std::vector<LogRecord>> LogReader::ReadAll(const std::string& path) {
    std::vector<LogRecord> records;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return records;  // No log yet: empty
    }

    auto file_result = storage::PosixFile::Open(path, /*create_if_missing=*/false);
    if (!file_result) [[unlikely]] {
        return std::unexpected(Status::IoError(file_result.error().message));
    }
    auto size_result = file_result->Size();
    if (!size_result) [[unlikely]] {
        return std::unexpected(Status::IoError(size_result.error().message));
    }

    // WALs are truncated at every checkpoint, so reading the whole file into
    // memory is fine at this scale (bounded by Options::max_wal_bytes plus
    // one burst).
    std::vector<uint8_t> bytes(*size_result);
    if (!bytes.empty()) {
        auto read_result = file_result->Read(0, bytes.data(), bytes.size());
        if (!read_result) [[unlikely]] {
            return std::unexpected(Status::IoError(read_result.error().message));
        }
    }

    size_t offset = 0;
    Lsn expected_lsn = kInvalidLsn;  // Derived from the first record
    while (offset < bytes.size()) {
        auto parsed = LogRecord::Parse(
            std::span(bytes.data() + offset, bytes.size() - offset));
        if (!parsed) {
            break;  // Torn/corrupt tail: clean end-of-log
        }
        auto& [record, consumed] = *parsed;

        // Records must form a contiguous LSN chain; a break means the tail
        // was overwritten or mis-appended - stop trusting it.
        if (expected_lsn != kInvalidLsn && record.lsn != expected_lsn) {
            break;
        }
        expected_lsn = record.lsn + consumed;

        records.push_back(std::move(record));
        offset += consumed;
    }

    return records;
}

}  // namespace kvstore::wal
