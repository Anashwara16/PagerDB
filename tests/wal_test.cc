// ==============================================================================
// tests/wal_test.cc - WAL record encoding, writer, and reader tests
// ==============================================================================
//
// Phase 4 (tasks 4.1/4.2): record round-trips with CRC, torn/corrupt tail
// handling (a damaged tail is a clean end-of-log, not an error), byte-offset
// LSN assignment, buffered append + FlushTo durability, and truncation with
// a new base LSN (used by checkpoints).
//
// ==============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "kvstore/storage/buffer_pool.h"
#include "kvstore/storage/file.h"
#include "kvstore/storage/pager.h"
#include "kvstore/wal/log_reader.h"
#include "kvstore/wal/log_record.h"
#include "kvstore/wal/log_writer.h"

namespace kvstore::wal {
namespace {

namespace fs = std::filesystem;

class WalTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::path(::testing::TempDir()) / "kvstore_wal_test";
        fs::create_directories(dir_);
        wal_path_ = (dir_ / "wal.log").string();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    fs::path dir_;
    std::string wal_path_;
};

// -----------------------------------------------------------------------------
// Record encoding (task 4.1)
// -----------------------------------------------------------------------------

TEST_F(WalTest, RecordRoundTripsAllFields) {
    LogRecord record;
    record.lsn = 12345;
    record.txid = 0;
    record.type = LogRecordType::kPut;
    record.page_id = 42;
    record.key = "alpha";
    record.value = std::string("bin\0ary", 7);  // Embedded NUL survives

    std::string bytes;
    record.AppendTo(bytes);
    EXPECT_EQ(bytes.size(), record.SerializedSize());

    auto parsed = LogRecord::Parse(
        std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    ASSERT_TRUE(parsed.has_value());
    const auto& [rec, consumed] = *parsed;
    EXPECT_EQ(consumed, bytes.size());
    EXPECT_EQ(rec.lsn, 12345u);
    EXPECT_EQ(rec.type, LogRecordType::kPut);
    EXPECT_EQ(rec.page_id, 42u);
    EXPECT_EQ(rec.key, "alpha");
    EXPECT_EQ(rec.value, std::string("bin\0ary", 7));
}

TEST_F(WalTest, RecordRoundTripsEmptyKeyAndValue) {
    LogRecord record;
    record.lsn = 1;
    record.type = LogRecordType::kCheckpoint;
    record.page_id = kInvalidPageId;

    std::string bytes;
    record.AppendTo(bytes);
    auto parsed = LogRecord::Parse(
        std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->first.type, LogRecordType::kCheckpoint);
    EXPECT_TRUE(parsed->first.key.empty());
    EXPECT_TRUE(parsed->first.value.empty());
}

TEST_F(WalTest, ParseRejectsCorruptedBytes) {
    LogRecord record;
    record.lsn = 7;
    record.type = LogRecordType::kDelete;
    record.page_id = 3;
    record.key = "victim";

    std::string bytes;
    record.AppendTo(bytes);
    bytes[10] ^= 0x40;  // Flip a bit mid-record

    auto parsed = LogRecord::Parse(
        std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    EXPECT_FALSE(parsed.has_value());  // CRC catches it
}

TEST_F(WalTest, ParseRejectsTruncatedBuffers) {
    LogRecord record;
    record.lsn = 7;
    record.type = LogRecordType::kPut;
    record.page_id = 3;
    record.key = "key";
    record.value = "value";

    std::string bytes;
    record.AppendTo(bytes);

    for (size_t len = 0; len < bytes.size(); ++len) {
        auto parsed = LogRecord::Parse(
            std::span(reinterpret_cast<const uint8_t*>(bytes.data()), len));
        EXPECT_FALSE(parsed.has_value()) << "prefix length " << len;
    }
}

// -----------------------------------------------------------------------------
// Writer (task 4.2)
// -----------------------------------------------------------------------------

TEST_F(WalTest, WriterAssignsByteOffsetLsns) {
    auto writer = LogWriter::Open(wal_path_, kFirstLsn).value();

    LogRecord a;
    a.type = LogRecordType::kPut;
    a.page_id = 2;
    a.key = "k1";
    a.value = "v1";
    const Lsn lsn_a = writer.Append(a).value();
    EXPECT_EQ(lsn_a, kFirstLsn);

    LogRecord b;
    b.type = LogRecordType::kDelete;
    b.page_id = 2;
    b.key = "k1";
    const Lsn lsn_b = writer.Append(b).value();
    EXPECT_EQ(lsn_b, kFirstLsn + a.SerializedSize());
    EXPECT_EQ(writer.end_lsn(), lsn_b + b.SerializedSize());
    EXPECT_EQ(writer.SizeBytes(), a.SerializedSize() + b.SerializedSize());
}

TEST_F(WalTest, FlushToIsIdempotentAndSyncs) {
    auto writer = LogWriter::Open(wal_path_, kFirstLsn).value();

    LogRecord rec;
    rec.type = LogRecordType::kPut;
    rec.page_id = 2;
    rec.key = "k";
    rec.value = "v";
    const Lsn lsn = writer.Append(rec).value();
    EXPECT_LT(writer.flushed_lsn(), writer.end_lsn());  // Still buffered

    const uint64_t syncs_before = storage::PosixFile::TotalSyncs();
    ASSERT_TRUE(writer.FlushTo(lsn).ok());
    EXPECT_EQ(storage::PosixFile::TotalSyncs(), syncs_before + 1);
    EXPECT_EQ(writer.flushed_lsn(), writer.end_lsn());

    // Already durable: no further fsync.
    ASSERT_TRUE(writer.FlushTo(lsn).ok());
    EXPECT_EQ(storage::PosixFile::TotalSyncs(), syncs_before + 1);
}

TEST_F(WalTest, ReaderReadsBackWhatWriterWrote) {
    {
        auto writer = LogWriter::Open(wal_path_, kFirstLsn).value();
        for (int i = 0; i < 3; ++i) {
            LogRecord rec;
            rec.type = LogRecordType::kPut;
            rec.page_id = static_cast<PageId>(2 + i);
            rec.key = "key" + std::to_string(i);
            rec.value = "value" + std::to_string(i);
            ASSERT_TRUE(writer.Append(rec).has_value());
        }
        ASSERT_TRUE(writer.Flush().ok());
    }

    auto records = LogReader::ReadAll(wal_path_).value();
    ASSERT_EQ(records.size(), 3u);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(records[static_cast<size_t>(i)].key, "key" + std::to_string(i));
        EXPECT_EQ(records[static_cast<size_t>(i)].value, "value" + std::to_string(i));
        EXPECT_EQ(records[static_cast<size_t>(i)].page_id, static_cast<PageId>(2 + i));
    }
    // LSNs form the byte-offset chain.
    EXPECT_EQ(records[0].lsn, kFirstLsn);
    EXPECT_EQ(records[1].lsn, records[0].lsn + records[0].SerializedSize());
}

TEST_F(WalTest, ReaderStopsCleanlyAtTornTail) {
    {
        auto writer = LogWriter::Open(wal_path_, kFirstLsn).value();
        for (int i = 0; i < 2; ++i) {
            LogRecord rec;
            rec.type = LogRecordType::kPut;
            rec.page_id = 2;
            rec.key = "key" + std::to_string(i);
            rec.value = "value";
            ASSERT_TRUE(writer.Append(rec).has_value());
        }
        ASSERT_TRUE(writer.Flush().ok());
    }
    // Simulate a torn append: half a record header of garbage at the end.
    {
        auto file = storage::PosixFile::Open(wal_path_, false).value();
        const uint64_t size = file.Size().value();
        const char garbage[13] = "torn-record!";
        ASSERT_TRUE(file.Write(size, garbage, sizeof(garbage)).has_value());
    }

    auto records = LogReader::ReadAll(wal_path_).value();
    EXPECT_EQ(records.size(), 2u);  // The torn tail is a clean end-of-log
}

TEST_F(WalTest, ReadAllOnMissingFileIsEmpty) {
    auto records = LogReader::ReadAll((dir_ / "absent.log").string());
    ASSERT_TRUE(records.has_value());
    EXPECT_TRUE(records->empty());
}

TEST_F(WalTest, TruncateRestartsAtNewBase) {
    auto writer = LogWriter::Open(wal_path_, kFirstLsn).value();
    LogRecord rec;
    rec.type = LogRecordType::kPut;
    rec.page_id = 2;
    rec.key = "old";
    rec.value = "data";
    ASSERT_TRUE(writer.Append(rec).has_value());
    ASSERT_TRUE(writer.Flush().ok());

    const Lsn new_base = writer.end_lsn();
    ASSERT_TRUE(writer.Truncate(new_base).ok());
    EXPECT_EQ(writer.SizeBytes(), 0u);
    EXPECT_EQ(writer.end_lsn(), new_base);

    LogRecord fresh;
    fresh.type = LogRecordType::kDelete;
    fresh.page_id = 2;
    fresh.key = "old";
    EXPECT_EQ(writer.Append(fresh).value(), new_base);
    ASSERT_TRUE(writer.Flush().ok());

    auto records = LogReader::ReadAll(wal_path_).value();
    ASSERT_EQ(records.size(), 1u);  // Pre-truncate records are gone
    EXPECT_EQ(records[0].lsn, new_base);
    EXPECT_EQ(records[0].type, LogRecordType::kDelete);
}

TEST_F(WalTest, WriterReopenContinuesAfterExistingRecords) {
    Lsn first_end;
    {
        auto writer = LogWriter::Open(wal_path_, kFirstLsn).value();
        LogRecord rec;
        rec.type = LogRecordType::kPut;
        rec.page_id = 2;
        rec.key = "persisted";
        rec.value = "v";
        ASSERT_TRUE(writer.Append(rec).has_value());
        ASSERT_TRUE(writer.Flush().ok());
        first_end = writer.end_lsn();
    }
    {
        auto writer = LogWriter::Open(wal_path_, kFirstLsn).value();
        EXPECT_EQ(writer.end_lsn(), first_end);  // Positioned after old bytes
        LogRecord rec;
        rec.type = LogRecordType::kPut;
        rec.page_id = 3;
        rec.key = "appended";
        rec.value = "v";
        EXPECT_EQ(writer.Append(rec).value(), first_end);
        ASSERT_TRUE(writer.Flush().ok());
    }
    auto records = LogReader::ReadAll(wal_path_).value();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[1].key, "appended");
}

// -----------------------------------------------------------------------------
// The write-ahead rule (task 4.5): no page reaches the data file before the
// log covers its page_lsn
// -----------------------------------------------------------------------------

class WalRuleTest : public WalTest {
protected:
    void SetUp() override {
        WalTest::SetUp();
        auto pager = storage::Pager::Open((dir_ / "data.db").string(), true);
        ASSERT_TRUE(pager.has_value()) << pager.error().message();
        pager_ = std::move(*pager);
        for (int i = 0; i < 8; ++i) {
            ASSERT_TRUE(pager_.AllocatePage().has_value());  // pages 2..9
        }
    }

    // Dirty page 2 with a logged mutation whose record is still buffered.
    Lsn DirtyPageTwoWithUnflushedRecord(storage::BufferPool& pool,
                                        LogWriter& writer) {
        auto guard = pool.FetchPage(2).value();
        guard.page().Init(2, storage::PageType::kLeaf);

        LogRecord rec;
        rec.type = LogRecordType::kPut;
        rec.page_id = 2;
        rec.key = "k";
        rec.value = "v";
        const Lsn lsn = writer.Append(rec).value();
        guard.page().header()->page_lsn = lsn;
        guard.MarkDirty();
        return lsn;
    }

    storage::Pager pager_;
};

TEST_F(WalRuleTest, DirtyEvictionFlushesLogThroughPageLsnFirst) {
    auto writer = LogWriter::Open(wal_path_, kFirstLsn).value();
    storage::BufferPool pool(pager_, 4, &writer);

    const Lsn lsn = DirtyPageTwoWithUnflushedRecord(pool, writer);
    ASSERT_LT(writer.flushed_lsn(), writer.end_lsn())
        << "record must start out buffered";

    // Evict page 2 by cycling more pages than frames.
    for (PageId id = 3; id <= 6; ++id) {
        ASSERT_TRUE(pool.FetchPage(id).has_value());
    }
    ASSERT_FALSE(pool.IsResident(2));

    // The pool must have flushed the log past the page's LSN before writing.
    EXPECT_GT(writer.flushed_lsn(), lsn);
}

// Regression (op-group atomicity vs the write-ahead rule): recovery drops a
// record group whose op-end record is not durable, on the assumption that
// none of the group's pages reached data.db. Flushing only *through the
// victim's page_lsn* breaks that assumption: if some other committer flushed
// the log mid-group (covering the victim's image but not the later op-end),
// a page_lsn-bounded flush is a no-op and the page lands on disk while its
// group is still incomplete on disk. A crash then leaves a half-applied
// structural change that recovery will not repair. Eviction must therefore
// make EVERYTHING appended so far durable - in particular the op-end record
// of the victim's group (the tree guarantees it is already appended, by
// keeping every page of an in-flight op pinned).
TEST_F(WalRuleTest, DirtyEvictionFlushesTheWholeAppendedLog) {
    auto writer = LogWriter::Open(wal_path_, kFirstLsn).value();
    storage::BufferPool pool(pager_, 4, &writer);

    // A group's non-final record targets page 2 (op_end stays false).
    const Lsn image_lsn = DirtyPageTwoWithUnflushedRecord(pool, writer);

    // Another thread's commit flushes the log mid-group: the durable prefix
    // now covers page 2's LSN, but the group is still open.
    ASSERT_TRUE(writer.Flush().ok());
    ASSERT_GT(writer.flushed_lsn(), image_lsn);

    // The group's op-end record arrives after that flush: buffered only.
    LogRecord end_rec;
    end_rec.type = LogRecordType::kPut;
    end_rec.page_id = 2;
    end_rec.op_end = true;
    end_rec.key = "k2";
    end_rec.value = "v2";
    ASSERT_TRUE(writer.Append(end_rec).has_value());
    ASSERT_LT(writer.flushed_lsn(), writer.end_lsn())
        << "op-end must start buffered";

    // Evict page 2. A page_lsn-bounded flush would skip (already durable
    // past image_lsn) and write the page with its group incomplete on disk.
    for (PageId id = 3; id <= 6; ++id) {
        ASSERT_TRUE(pool.FetchPage(id).has_value());
    }
    ASSERT_FALSE(pool.IsResident(2));

    EXPECT_EQ(writer.flushed_lsn(), writer.end_lsn())
        << "eviction wrote page 2 while its op-end record was not durable";
}

TEST_F(WalRuleTest, DirtyEvictionFailsAndKeepsPageWhenLogFlushFails) {
    auto writer = LogWriter::Open(wal_path_, kFirstLsn).value();
    storage::BufferPool pool(pager_, 4, &writer);

    DirtyPageTwoWithUnflushedRecord(pool, writer);

    // Every write now fails: the WAL flush inside the eviction path fails,
    // so the dirty page must NOT be written or discarded.
    storage::PosixFile::FailAfterNWrites(0);
    for (PageId id = 3; id <= 5; ++id) {
        ASSERT_TRUE(pool.FetchPage(id).has_value());  // Fill remaining frames
    }
    auto blocked = pool.FetchPage(6);  // Needs to evict dirty page 2
    ASSERT_FALSE(blocked.has_value());
    EXPECT_TRUE(pool.IsResident(2)) << "dirty page must survive a failed flush";
    storage::PosixFile::ClearWriteFailure();

    // With writes working again, cycling more pages than frames must
    // eventually evict (and write back) the dirty page. The failed attempt
    // moved it to MRU, so several fetches are needed.
    for (PageId id = 6; id <= 9; ++id) {
        ASSERT_TRUE(pool.FetchPage(id).has_value()) << "page " << id;
    }
    EXPECT_FALSE(pool.IsResident(2));
}

// --------------------------------------------------------------------------
// Phase 5: before-image field + transaction markers
// --------------------------------------------------------------------------

TEST(LogRecord, BeforeImageRoundTrip) {
    using kvstore::wal::LogRecord;
    using kvstore::wal::LogRecordType;

    // kPut carrying a prior value (an update's before-image).
    LogRecord put;
    put.lsn = 100;
    put.txid = 7;
    put.type = LogRecordType::kPut;
    put.page_id = 3;
    put.op_end = true;
    put.key = "k";
    put.value = "new";
    put.before = std::string("old");

    std::string buf;
    put.AppendTo(buf);
    auto parsed = LogRecord::Parse(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(buf.data()), buf.size()));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->first.txid, 7u);
    EXPECT_EQ(parsed->first.type, LogRecordType::kPut);
    EXPECT_TRUE(parsed->first.op_end);
    EXPECT_EQ(parsed->first.key, "k");
    EXPECT_EQ(parsed->first.value, "new");
    ASSERT_TRUE(parsed->first.before.has_value());
    EXPECT_EQ(*parsed->first.before, "old");
    EXPECT_EQ(parsed->second, buf.size());
}

TEST(LogRecord, AbsentBeforeImageAndTxnMarkers) {
    using kvstore::wal::LogRecord;
    using kvstore::wal::LogRecordType;

    // Insert: before is nullopt (key was absent) -> 0xFFFF sentinel on disk.
    LogRecord ins;
    ins.type = LogRecordType::kPut;
    ins.key = "x";
    ins.value = "";      // empty value, still present
    ins.before = std::nullopt;
    std::string b1;
    ins.AppendTo(b1);
    auto p1 = LogRecord::Parse(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(b1.data()), b1.size()));
    ASSERT_TRUE(p1.has_value());
    EXPECT_FALSE(p1->first.before.has_value());
    EXPECT_EQ(p1->first.value, "");

    // Commit marker parses back with its type.
    LogRecord commit;
    commit.txid = 42;
    commit.type = LogRecordType::kCommit;
    commit.op_end = true;
    std::string b2;
    commit.AppendTo(b2);
    auto p2 = LogRecord::Parse(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(b2.data()), b2.size()));
    ASSERT_TRUE(p2.has_value());
    EXPECT_EQ(p2->first.type, LogRecordType::kCommit);
    EXPECT_EQ(p2->first.txid, 42u);
}

}  // namespace
}  // namespace kvstore::wal
