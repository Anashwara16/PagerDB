// ==============================================================================
// kvstore/wal/log_writer.cc - WAL Writer Implementation (Phase 4, 5-safe)
// ==============================================================================

#include "kvstore/wal/log_writer.h"

#include <utility>

#include "kvstore/storage/page_format.h"  // storage::Page, kPageSize

namespace kvstore::wal {

namespace {

Status FileErrorToStatus(const storage::FileError& err) {
    switch (err.kind) {
        case storage::FileError::Kind::NotFound:
            return Status::NotFound(err.message);
        case storage::FileError::Kind::IoError:
            return Status::IoError(err.message);
        case storage::FileError::Kind::InternalError:
            return Status::Internal(err.message);
    }
    return Status::Internal("Unknown file error");
}

}  // namespace

Result<LogWriter> LogWriter::Open(const std::string& path, Lsn base_lsn) {
    auto file_result = storage::PosixFile::Open(path, /*create_if_missing=*/true);
    if (!file_result) [[unlikely]] {
        return std::unexpected(FileErrorToStatus(file_result.error()));
    }
    auto size_result = file_result->Size();
    if (!size_result) [[unlikely]] {
        return std::unexpected(FileErrorToStatus(size_result.error()));
    }
    return LogWriter(std::move(*file_result), base_lsn, *size_result);
}

void LogWriter::BufferRecordLocked(const LogRecord& record) {
    record.AppendTo(buffer_);
    end_lsn_ += record.SerializedSize();
}

Result<Lsn> LogWriter::Append(LogRecord& record) {
    std::lock_guard<std::mutex> lg(*mu_);
    record.lsn = end_lsn_;
    BufferRecordLocked(record);
    return record.lsn;
}

Result<Lsn> LogWriter::AppendPageImage(PageId page_id, storage::Page& page,
                                       TxId txid, bool op_end) {
    std::lock_guard<std::mutex> lg(*mu_);
    // Assign the LSN and stamp it into the page BEFORE serializing, so the
    // image on disk carries page_lsn == its own record LSN. Doing this under
    // the lock is what makes it safe against a concurrent appender.
    const Lsn lsn = end_lsn_;
    page.header()->page_lsn = lsn;

    LogRecord record;
    record.lsn = lsn;
    record.txid = txid;
    record.type = LogRecordType::kPageImage;
    record.page_id = page_id;
    record.op_end = op_end;
    record.value.assign(reinterpret_cast<const char*>(page.RawData()),
                        storage::kPageSize);
    BufferRecordLocked(record);
    imaged_since_truncate_.insert(page_id);
    return lsn;
}

Status LogWriter::FlushLocked() {
    if (buffer_.empty() && flushed_lsn_ == end_lsn_) {
        return Status::Ok();  // Nothing new; skip the fsync
    }

    if (!buffer_.empty()) {
        auto write_result = file_.Write(file_bytes_, buffer_.data(), buffer_.size());
        if (!write_result) [[unlikely]] {
            return FileErrorToStatus(write_result.error());
        }
        file_bytes_ += buffer_.size();
        buffer_.clear();
    }

    auto sync_result = file_.Sync();
    if (!sync_result) [[unlikely]] {
        return FileErrorToStatus(sync_result.error());
    }
    flushed_lsn_ = end_lsn_;
    return Status::Ok();
}

Status LogWriter::Flush() {
    std::lock_guard<std::mutex> lg(*mu_);
    return FlushLocked();
}

Status LogWriter::FlushTo(Lsn lsn) {
    std::lock_guard<std::mutex> lg(*mu_);
    if (lsn == kInvalidLsn || flushed_lsn_ > lsn ||
        (flushed_lsn_ == lsn && lsn == end_lsn_)) {
        return Status::Ok();
    }
    if (flushed_lsn_ >= end_lsn_) {
        return Status::Ok();  // Everything appended is already durable
    }
    // Simplicity over precision: flush the whole buffer. Records are small
    // and partial flushes would complicate the offset bookkeeping.
    return FlushLocked();
}

Status LogWriter::Truncate(Lsn new_base) {
    std::lock_guard<std::mutex> lg(*mu_);
    auto truncate_result = file_.Truncate(0);
    if (!truncate_result) [[unlikely]] {
        return FileErrorToStatus(truncate_result.error());
    }
    auto sync_result = file_.Sync();
    if (!sync_result) [[unlikely]] {
        return FileErrorToStatus(sync_result.error());
    }
    buffer_.clear();
    file_bytes_ = 0;
    base_lsn_ = new_base;
    end_lsn_ = new_base;
    flushed_lsn_ = new_base;
    imaged_since_truncate_.clear();  // New checkpoint epoch: image-first again
    return Status::Ok();
}

}  // namespace kvstore::wal
