#include "public_trade_queue.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace exchange::market_data {
namespace {

PublicTrade trade(const std::uint64_t sequence, const std::uint32_t index) {
    return {.eventId = {domain::CommandSequence{sequence}, domain::EventIndex{index}, domain::ExchangeRunId{1}},
            .instrumentId = domain::InstrumentId{1},
            .executionPrice = domain::Price{100},
            .executionQuantity = domain::Quantity{1},
            .aggressorSide = domain::Side::BUY};
}

TEST(BoundedPublicTradeQueueTest, RejectsInvalidCapacityAndAcceptsOneExactMaximumBatch) {
    EXPECT_THROW((void)BoundedPublicTradeQueue{0}, std::invalid_argument);

    BoundedPublicTradeQueue queue{4};
    std::vector<PublicTrade> batch{trade(1, 0), trade(1, 1), trade(1, 2), trade(1, 3)};
    EXPECT_TRUE(queue.tryPush(batch));
    EXPECT_TRUE(batch.empty());
    EXPECT_EQ(queue.queuedRecordCount(), 4U);
}

TEST(BoundedPublicTradeQueueTest, ExactFitPreservesFifoAndRecordAccounting) {
    BoundedPublicTradeQueue queue{3};
    std::vector<PublicTrade> first{trade(1, 0)};
    std::vector<PublicTrade> second{trade(2, 0), trade(2, 1)};
    ASSERT_TRUE(queue.tryPush(first));
    ASSERT_TRUE(queue.tryPush(second));
    EXPECT_EQ(queue.queuedRecordCount(), 3U);

    std::vector<PublicTrade> popped;
    ASSERT_TRUE(queue.tryPop(popped));
    EXPECT_EQ(popped, (std::vector<PublicTrade>{trade(1, 0)}));
    EXPECT_EQ(queue.queuedRecordCount(), 2U);
    ASSERT_TRUE(queue.tryPop(popped));
    EXPECT_EQ(popped, (std::vector<PublicTrade>{trade(2, 0), trade(2, 1)}));
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.queuedRecordCount(), 0U);
    EXPECT_FALSE(queue.tryPop(popped));
}

TEST(BoundedPublicTradeQueueTest, InsufficientCapacityLeavesSourceBatchUnchanged) {
    BoundedPublicTradeQueue queue{2};
    std::vector<PublicTrade> accepted{trade(1, 0)};
    ASSERT_TRUE(queue.tryPush(accepted));
    std::vector<PublicTrade> rejected{trade(2, 0), trade(2, 1)};
    const auto original = rejected;

    EXPECT_FALSE(queue.tryPush(rejected));
    EXPECT_EQ(rejected, original);
    EXPECT_EQ(queue.queuedRecordCount(), 1U);
}

TEST(BoundedPublicTradeQueueTest, RejectsEmptyBatchWithoutChangingTheQueue) {
    BoundedPublicTradeQueue queue{1};
    std::vector<PublicTrade> empty;
    EXPECT_THROW((void)queue.tryPush(empty), std::invalid_argument);
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.queuedRecordCount(), 0U);
}

} // namespace
} // namespace exchange::market_data
