#include "private_result_queue.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace exchange::private_result {
namespace {

PrivateEvent rejection(const std::uint64_t clientId, const std::uint32_t eventIndex) {
    return domain::CommandRejected{
        .eventId = {domain::CommandSequence{1}, domain::EventIndex{eventIndex}, domain::ExchangeRunId{1}},
        .commandType = domain::CommandType::NEW_ORDER,
        .clientId = domain::ClientId{clientId},
        .clientCommandId = domain::ClientCommandId{"COMMAND"},
        .relevantOrderId = std::nullopt,
        .reason = domain::CommandRejectionReason::BOOK_CAPACITY_EXCEEDED,
    };
}

RecipientResult recipient(const std::uint64_t clientId, const std::size_t eventCount) {
    PrivateResult events;
    events.reserve(eventCount);
    for (std::size_t index = 0; index < eventCount; ++index) {
        events.push_back(rejection(clientId, static_cast<std::uint32_t>(index)));
    }
    return {.recipient = domain::ClientId{clientId}, .correlation = std::nullopt, .privateResult = std::move(events)};
}

TEST(BoundedPrivateResultQueueTest, RejectsZeroCapacityAndAcceptsExactCapacity) {
    EXPECT_THROW((void)BoundedPrivateResultQueue{0}, std::invalid_argument);

    BoundedPrivateResultQueue queue{8};
    RecipientResults batch{recipient(1, 4), recipient(2, 4)};
    EXPECT_TRUE(queue.tryPush(batch));
    EXPECT_TRUE(batch.empty());
    EXPECT_EQ(queue.queuedEventCount(), 8U);
}

TEST(BoundedPrivateResultQueueTest, FailedInsertionIsAtomicAndLeavesSourceUnchanged) {
    BoundedPrivateResultQueue queue{3};
    RecipientResults first{recipient(1, 2)};
    ASSERT_TRUE(queue.tryPush(first));
    RecipientResults rejected{recipient(2, 1), recipient(3, 1)};
    const auto original = rejected;

    EXPECT_FALSE(queue.tryPush(rejected));
    EXPECT_EQ(rejected, original);
    EXPECT_EQ(queue.queuedEventCount(), 2U);
}

TEST(BoundedPrivateResultQueueTest, PreservesBatchRecipientAndEventFifoWithExactAccounting) {
    BoundedPrivateResultQueue queue{4};
    RecipientResults first{recipient(3, 1), recipient(4, 2)};
    RecipientResults second{recipient(5, 1)};
    const auto firstExpected = first;
    const auto secondExpected = second;
    ASSERT_TRUE(queue.tryPush(first));
    ASSERT_TRUE(queue.tryPush(second));
    EXPECT_EQ(queue.queuedEventCount(), 4U);

    RecipientResults popped;
    ASSERT_TRUE(queue.tryPop(popped));
    EXPECT_EQ(popped, firstExpected);
    EXPECT_EQ(queue.queuedEventCount(), 1U);
    ASSERT_TRUE(queue.tryPop(popped));
    EXPECT_EQ(popped, secondExpected);
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.queuedEventCount(), 0U);
    EXPECT_FALSE(queue.tryPop(popped));
}

TEST(BoundedPrivateResultQueueTest, RejectsEmptyAndEventlessBatches) {
    BoundedPrivateResultQueue queue{2};
    RecipientResults empty;
    EXPECT_THROW((void)queue.tryPush(empty), std::invalid_argument);
    RecipientResults eventless{recipient(1, 0)};
    EXPECT_THROW((void)queue.tryPush(eventless), std::invalid_argument);
    EXPECT_TRUE(queue.empty());
}

} // namespace
} // namespace exchange::private_result
