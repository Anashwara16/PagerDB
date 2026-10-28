// ==============================================================================
// kvstore/wal/log_record.h - Write-Ahead Log Record Encoding (Phase 4)
// ==============================================================================
//
// On-disk record layout (all integers little-endian):
//
//   [lsn:8][txid:8][type:1][page_id:4][key_len:2][val_len:2][key][value][crc:4]
//
// The CRC-32 covers every byte before it. A record whose bytes are missing
// or whose CRC fails marks the end of the log (torn tail after a crash) -
// the reader stops there without reporting an error.
//
// Deviation from the original task sketch (documented in FORMAT.md): a
// 4-byte page_id field was added so records are physiological - redo applies
// each record to exactly one page, guarded by that page's page_lsn. Without
// it, replaying logical records into a partially-flushed multi-page split
// can duplicate or strand keys.
//
// Record types and payload conventions:
//   kPut        page_id = target leaf; key/value = user pair
//   kDelete     page_id = target leaf; key = user key
//   kAllocPage  page_id = newly allocated page (redo extends the file)
//   kFreePage   page_id = freed page (reserved; nothing emits it yet)
//   kRootChange page_id = new root page id
//   kCheckpoint no payload; checkpoint_lsn in the meta is authoritative
//   kPageImage  page_id = rebuilt page; value = full 4096-byte after-image
//               (used for every page a structural change rewrites)
//
// The type byte's high bit (kOpEndBit) marks the final record of a logical
// operation; see LogRecord::op_end.
//
// txid is written as 0 until transactions arrive in Phase 5.
//
// ==============================================================================

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "kvstore/types.h"

namespace kvstore::wal {

enum class LogRecordType : uint8_t {
    kPut = 1,
    kDelete = 2,
    kAllocPage = 3,
    kFreePage = 4,
    kRootChange = 5,
    kCheckpoint = 6,
    kPageImage = 7,
    kBegin = 8,    // Phase 5: transaction start (lazy, first write)
    kCommit = 9,   // Phase 5: transaction commit (op_end)
    kAbort = 10,   // Phase 5: transaction abort (op_end, after logged undo)
};

struct LogRecord {
    // Fixed prefix: lsn + txid + type + page_id + key_len + val_len + before_len.
    static constexpr size_t kHeaderSize = 8 + 8 + 1 + 4 + 2 + 2 + 2;  // 27
    static constexpr size_t kCrcSize = 4;

    // On-disk before_len sentinel: the key had no prior value (an insert).
    static constexpr uint16_t kBeforeAbsent = 0xFFFF;

    // High bit of the on-disk type byte: this record COMPLETES a logical
    // operation. Recovery applies records in op groups, all-or-nothing: a
    // group whose op-end record is missing (torn tail, or a mid-op WAL-rule
    // flush followed by a crash) is discarded entirely. Without this, a
    // partially logged split replays half a structural change and loses
    // committed keys.
    static constexpr uint8_t kOpEndBit = 0x80;

    Lsn lsn = kInvalidLsn;          // Assigned by LogWriter::Append
    TxId txid = 0;                  // Real, monotone txid from Phase 5 (0 = none)
    LogRecordType type = LogRecordType::kPut;
    PageId page_id = kInvalidPageId;
    bool op_end = false;            // Last record of its logical operation
    std::string key;
    std::string value;
    // Phase 5.4: prior value for kPut/kDelete undo. nullopt => key was absent
    // (insert). Values never reach 0xFFFF bytes (page-bounded), so the sentinel
    // is unambiguous. Empty/unused for all other record types.
    std::optional<std::string> before;

    [[nodiscard("SerializedSize sizes the append")]]
    size_t SerializedSize() const noexcept {
        const size_t before_bytes = before ? before->size() : 0;
        return kHeaderSize + key.size() + value.size() + before_bytes + kCrcSize;
    }

    // Append the serialized record (using the current lsn field) to `out`.
    void AppendTo(std::string& out) const;

    // Parse one record from the front of `buffer`.
    // Returns the record and the bytes consumed, or nullopt if the buffer
    // holds no complete, CRC-valid record (torn tail).
    [[nodiscard("Parse returns the decoded record")]]
    static std::optional<std::pair<LogRecord, size_t>> Parse(
        std::span<const uint8_t> buffer);
};

}  // namespace kvstore::wal
