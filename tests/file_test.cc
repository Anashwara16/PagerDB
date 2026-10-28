// ==============================================================================
// tests/file_test.cc - PosixFile smoke tests
// ==============================================================================
//
// Phase 0 smoke tests for the lowest storage layer: open/create semantics,
// positioned read/write round-trip, Size, and move-only ownership of the fd.
// Phase 1 (task 1.16) extends this file with short-read, truncate, and
// double-move coverage.
//
// ==============================================================================

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "kvstore/storage/file.h"

namespace kvstore::storage {
namespace {

namespace fs = std::filesystem;

class FileTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::path(::testing::TempDir()) / "kvstore_file_test";
        fs::create_directories(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    [[nodiscard]] std::string FilePath(std::string_view name) const {
        return (dir_ / name).string();
    }

    fs::path dir_;
};

TEST_F(FileTest, OpenCreatesFileWhenAllowed) {
    auto result = PosixFile::Open(FilePath("new.db"), /*create_if_missing=*/true);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->IsOpen());
    EXPECT_TRUE(fs::exists(FilePath("new.db")));
}

TEST_F(FileTest, OpenMissingFileFailsWithNotFound) {
    auto result = PosixFile::Open(FilePath("absent.db"), /*create_if_missing=*/false);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, FileError::Kind::NotFound);
}

TEST_F(FileTest, WriteReadRoundTrip) {
    auto file = PosixFile::Open(FilePath("rt.db"), true).value();

    const std::string payload = "hello, pager";
    ASSERT_TRUE(file.Write(0, payload.data(), payload.size()).has_value());

    std::string buffer(payload.size(), '\0');
    ASSERT_TRUE(file.Read(0, buffer.data(), buffer.size()).has_value());
    EXPECT_EQ(buffer, payload);
}

TEST_F(FileTest, WriteAtOffsetExtendsFileAndSizeReflectsIt) {
    auto file = PosixFile::Open(FilePath("sz.db"), true).value();

    const char byte = 'x';
    ASSERT_TRUE(file.Write(4096, &byte, 1).has_value());

    auto size = file.Size();
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(*size, 4097u);
}

TEST_F(FileTest, MoveTransfersOwnership) {
    auto a = PosixFile::Open(FilePath("mv.db"), true).value();
    ASSERT_TRUE(a.IsOpen());

    PosixFile b = std::move(a);
    EXPECT_TRUE(b.IsOpen());
    EXPECT_FALSE(a.IsOpen());  // NOLINT(bugprone-use-after-move): moved-from state is spec'd

    // The moved-to object still works.
    const char byte = 'y';
    EXPECT_TRUE(b.Write(0, &byte, 1).has_value());
}

TEST_F(FileTest, ShortReadAtEofIsIoError) {
    auto file = PosixFile::Open(FilePath("short.db"), true).value();
    const std::string payload = "abc";
    ASSERT_TRUE(file.Write(0, payload.data(), payload.size()).has_value());

    char buffer[16];
    auto result = file.Read(0, buffer, sizeof(buffer));  // Only 3 bytes exist
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, FileError::Kind::IoError);
}

TEST_F(FileTest, TruncateShrinksAndExtends) {
    auto file = PosixFile::Open(FilePath("tr.db"), true).value();
    const std::string payload = "0123456789";
    ASSERT_TRUE(file.Write(0, payload.data(), payload.size()).has_value());

    ASSERT_TRUE(file.Truncate(4).has_value());
    EXPECT_EQ(file.Size().value(), 4u);

    // Extending zero-fills the new bytes.
    ASSERT_TRUE(file.Truncate(8).has_value());
    EXPECT_EQ(file.Size().value(), 8u);
    char buffer[8];
    ASSERT_TRUE(file.Read(0, buffer, sizeof(buffer)).has_value());
    EXPECT_EQ(std::string_view(buffer, 4), "0123");
    EXPECT_EQ(std::string_view(buffer + 4, 4), std::string_view("\0\0\0\0", 4));
}

TEST_F(FileTest, MoveAssignmentTransfersOwnership) {
    auto a = PosixFile::Open(FilePath("mva.db"), true).value();
    auto b = PosixFile::Open(FilePath("mvb.db"), true).value();

    b = std::move(a);  // b's original fd is closed, b now owns a's fd
    EXPECT_TRUE(b.IsOpen());
    EXPECT_FALSE(a.IsOpen());  // NOLINT(bugprone-use-after-move): moved-from state is spec'd

    const char byte = 'z';
    EXPECT_TRUE(b.Write(0, &byte, 1).has_value());
}

// Phase 4 (task 4.9): fault injection for crash tests. The (n+1)th write
// after arming persists only half its bytes and reports IoError - a torn
// write, exactly what a power cut mid-pwrite leaves behind.
TEST_F(FileTest, FailAfterNWritesTearsTheFailingWrite) {
    auto file = PosixFile::Open(FilePath("fault.db"), true).value();

    PosixFile::FailAfterNWrites(1);
    const std::string first = "first-write";
    ASSERT_TRUE(file.Write(0, first.data(), first.size()).has_value());

    const std::string second = "second-write";
    auto result = file.Write(100, second.data(), second.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, FileError::Kind::IoError);

    // Further writes keep failing until the hook is cleared.
    auto still_failing = file.Write(0, first.data(), first.size());
    EXPECT_FALSE(still_failing.has_value());

    PosixFile::ClearWriteFailure();

    // The failing write tore: exactly half its bytes reached the file.
    EXPECT_EQ(file.Size().value(), 100 + second.size() / 2);

    // Disarmed: writes work again.
    EXPECT_TRUE(file.Write(0, first.data(), first.size()).has_value());
}

TEST_F(FileTest, SyncIncrementsTotalSyncsCounter) {
    auto file = PosixFile::Open(FilePath("sync.db"), true).value();
    const uint64_t before = PosixFile::TotalSyncs();
    ASSERT_TRUE(file.Sync().has_value());
    EXPECT_EQ(PosixFile::TotalSyncs(), before + 1);
}

}  // namespace
}  // namespace kvstore::storage
