#include <gtest/gtest.h>

#include "admission_completion_consumer.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace exchange::core::admission {
namespace {

sequencer::sequenceMessage makeSequencedCommand(const std::string& clientCommandId,
                                                const std::uint64_t commandSequence) {
    sequencer::sequenceMessage command{};
    command.clientId = domain::ClientId{100};
    command.clientCommandId.emplace(clientCommandId);
    command.instrumentId = domain::InstrumentId{1};
    command.configurationVersion = 1;
    command.type = sequencer::orderType::BUY;
    command.price = domain::Price{100};
    command.quantity = domain::Quantity{10};
    command.tif = task::TimeInForce::GTC;
    command.globalSequenceNumber = domain::CommandSequence{commandSequence};
    command.orderId = domain::orderIdFrom(command.globalSequenceNumber);
    return command;
}

matching_engine::ImmutableCommandResultBatch makeResult(const std::string& clientCommandId,
                                                        const std::uint64_t commandSequence) {
    std::vector<domain::BusinessEvent> events;
    events.emplace_back(domain::OrderRested{
        .eventId =
            {
                .commandSequence = domain::CommandSequence{commandSequence},
                .eventIndex = domain::EventIndex{0},
            },
        .orderId = domain::OrderId{commandSequence},
        .clientId = domain::ClientId{100},
        .instrumentId = domain::InstrumentId{1},
        .side = domain::Side::BUY,
        .price = domain::Price{100},
        .remainingQuantity = domain::Quantity{10},
    });
    return std::make_shared<const matching_engine::CommandResultBatch>(
        domain::CommandResultCorrelation{
            .clientId = domain::ClientId{100},
            .clientCommandId = domain::ClientCommandId{clientCommandId},
            .commandSequence = domain::CommandSequence{commandSequence},
        },
        matching_engine::ProcessingResult::APPLIED, std::move(events));
}

} // namespace

TEST(AdmissionCompletionConsumerTest, CompletesInQueueOrderAndPreservesExactSharedBatches) {
    CommandAdmissionIndex index(4);
    matching_engine::BoundedCommandResultQueue queue(2);
    AdmissionCompletionConsumer consumer(queue, index);
    const sequencer::sequenceMessage first = makeSequencedCommand("FIRST", 1);
    const sequencer::sequenceMessage second = makeSequencedCommand("SECOND", 2);
    const matching_engine::ImmutableCommandResultBatch firstResult = makeResult("FIRST", 1);
    const matching_engine::ImmutableCommandResultBatch secondResult = makeResult("SECOND", 2);

    ASSERT_EQ(index.reserve(first).status, AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_EQ(index.markSequenced(first), MarkSequencedStatus::SEQUENCED);
    ASSERT_EQ(index.reserve(second).status, AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_EQ(index.markSequenced(second), MarkSequencedStatus::SEQUENCED);
    ASSERT_TRUE(queue.tryPush(firstResult));
    ASSERT_TRUE(queue.tryPush(secondResult));

    EXPECT_TRUE(consumer.processNext());
    EXPECT_EQ(index.completedResult(first), firstResult);
    EXPECT_EQ(index.reserve(second).status, AdmissionStatus::IDENTICAL_IN_FLIGHT);
    EXPECT_TRUE(consumer.processNext());
    EXPECT_EQ(index.completedResult(second), secondResult);
    EXPECT_FALSE(consumer.processNext());
    EXPECT_FALSE(consumer.hasPendingBatch());
    EXPECT_FALSE(consumer.failed());
}

TEST(AdmissionCompletionConsumerTest, UnknownBatchFailStopsAndRetainsItWithoutDiscardingLaterBatch) {
    CommandAdmissionIndex index(4);
    matching_engine::BoundedCommandResultQueue queue(2);
    AdmissionCompletionConsumer consumer(queue, index);
    const matching_engine::ImmutableCommandResultBatch unknown = makeResult("UNKNOWN", 1);
    const matching_engine::ImmutableCommandResultBatch later = makeResult("LATER", 2);
    ASSERT_TRUE(queue.tryPush(unknown));
    ASSERT_TRUE(queue.tryPush(later));

    EXPECT_THROW(static_cast<void>(consumer.processNext()), std::logic_error);
    EXPECT_TRUE(consumer.failed());
    EXPECT_TRUE(consumer.hasPendingBatch());
    EXPECT_EQ(consumer.pendingBatch(), unknown);
    EXPECT_EQ(queue.size(), 1u);
    EXPECT_THROW(static_cast<void>(consumer.processNext()), std::logic_error);
    EXPECT_EQ(queue.size(), 1u);
}

} // namespace exchange::core::admission
