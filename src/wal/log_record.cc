// ==============================================================================
// kvstore/wal/log_record.cc - WAL Record Serialization (Phase 4)
// ==============================================================================

#include "kvstore/wal/log_record.h"

#include <cstring>

#include "kvstore/storage/page_format.h"  // Crc32

namespace kvstore::wal {

namespace {

template <typename T>
void PutLE(std::string& out, T value) {
    // All supported targets are little-endian (see node.cc helpers); memcpy
    // keeps this alignment-safe.
    char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    out.append(bytes, sizeof(T));
}

template <typename T>
T GetLE(const uint8_t* ptr) {
    T value;
    std::memcpy(&value, ptr, sizeof(T));
    return value;
}

}  // namespace

void LogRecord::AppendTo(std::string& out) const {
    // No reserve() here (Phase 6 optimization #3): reserve(size + delta) on
    // every append requests an exact capacity, defeating std::string's
    // geometric growth - each record could reallocate and copy the whole
    // accumulated WAL buffer (measured: a 64 MB max_wal_bytes turned a 2 s
    // load into minutes of memmove). append() grows the buffer geometrically.
    const size_t start = out.size();

    PutLE(out, lsn);
    PutLE(out, txid);
    out.push_back(static_cast<char>(
        static_cast<uint8_t>(type) | (op_end ? kOpEndBit : 0)));
    PutLE(out, page_id);
    PutLE(out, static_cast<uint16_t>(key.size()));
    PutLE(out, static_cast<uint16_t>(value.size()));
    PutLE(out, before ? static_cast<uint16_t>(before->size()) : kBeforeAbsent);
    out.append(key);
    out.append(value);
    if (before) out.append(*before);

    const uint32_t crc =
        storage::Crc32(out.data() + start, out.size() - start);
    PutLE(out, crc);
}

std::optional<std::pair<LogRecord, size_t>> LogRecord::Parse(
    std::span<const uint8_t> buffer) {
    if (buffer.size() < kHeaderSize + kCrcSize) {
        return std::nullopt;  // Not even a full header: torn tail
    }

    LogRecord record;
    const uint8_t* p = buffer.data();
    record.lsn = GetLE<Lsn>(p);
    record.txid = GetLE<TxId>(p + 8);
    const uint8_t raw_type_byte = p[16];
    const uint8_t raw_type = raw_type_byte & static_cast<uint8_t>(~kOpEndBit);
    record.op_end = (raw_type_byte & kOpEndBit) != 0;
    record.page_id = GetLE<PageId>(p + 17);
    const uint16_t key_len = GetLE<uint16_t>(p + 21);
    const uint16_t val_len = GetLE<uint16_t>(p + 23);
    const uint16_t before_len = GetLE<uint16_t>(p + 25);

    if (raw_type < static_cast<uint8_t>(LogRecordType::kPut) ||
        raw_type > static_cast<uint8_t>(LogRecordType::kAbort)) {
        return std::nullopt;  // Garbage type byte
    }
    record.type = static_cast<LogRecordType>(raw_type);

    const size_t before_bytes = (before_len == kBeforeAbsent) ? 0 : before_len;
    const size_t total =
        kHeaderSize + key_len + val_len + before_bytes + kCrcSize;
    if (buffer.size() < total) {
        return std::nullopt;  // Payload cut short: torn tail
    }

    const uint32_t stored_crc = GetLE<uint32_t>(p + total - kCrcSize);
    if (storage::Crc32(p, total - kCrcSize) != stored_crc) {
        return std::nullopt;  // Bit rot or a torn write inside the record
    }

    const uint8_t* cur = p + kHeaderSize;
    record.key.assign(reinterpret_cast<const char*>(cur), key_len);
    cur += key_len;
    record.value.assign(reinterpret_cast<const char*>(cur), val_len);
    cur += val_len;
    if (before_len != kBeforeAbsent) {
        record.before = std::string(reinterpret_cast<const char*>(cur), before_len);
    }
    return std::pair{std::move(record), total};
}

}  // namespace kvstore::wal
