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

}  // namespace
}  // namespace kvstore::storage
