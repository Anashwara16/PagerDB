// ==============================================================================
// tests/types_test.cc - Compile-time and sentinel-value checks for core types
// ==============================================================================
//
// Phase 0 smoke tests: these pin down the fundamental type sizes and sentinel
// values that the on-disk format depends on. If any of these fail, the file
// format has silently changed.
//
// ==============================================================================

#include <gtest/gtest.h>

#include <limits>

#include "kvstore/status.h"
#include "kvstore/types.h"

namespace kvstore {
namespace {

TEST(TypesTest, PageIdIs32BitWithMaxSentinel) {
    static_assert(sizeof(PageId) == 4);
    EXPECT_EQ(kInvalidPageId, std::numeric_limits<PageId>::max());
    EXPECT_EQ(kInvalidPageId, 0xFFFFFFFFu);
}

TEST(TypesTest, LsnIs64BitStartingAtOne) {
    static_assert(sizeof(Lsn) == 8);
    EXPECT_EQ(kInvalidLsn, Lsn{0});
    EXPECT_EQ(kFirstLsn, Lsn{1});
}

TEST(TypesTest, SlotIdIs16BitWithMaxSentinel) {
    static_assert(sizeof(SlotId) == 2);
    EXPECT_EQ(kInvalidSlotId, 0xFFFFu);
}

TEST(TypesTest, TxIdIs64BitWithZeroSentinel) {
    static_assert(sizeof(TxId) == 8);
    EXPECT_EQ(kInvalidTxId, TxId{0});
}

TEST(StatusTest, OkAndErrorFactories) {
    EXPECT_TRUE(Status::Ok().ok());
    EXPECT_FALSE(Status::NotFound("x").ok());
    EXPECT_FALSE(Status::Corruption("x").ok());
    EXPECT_FALSE(Status::IoError("x").ok());
}

TEST(StatusTest, ResultCarriesValueOrStatus) {
    Result<int> good = 42;
    ASSERT_TRUE(good.has_value());
    EXPECT_EQ(*good, 42);

    Result<int> bad = std::unexpected(Status::NotFound("missing"));
    ASSERT_FALSE(bad.has_value());
    EXPECT_FALSE(bad.error().ok());
}

}  // namespace
}  // namespace kvstore
