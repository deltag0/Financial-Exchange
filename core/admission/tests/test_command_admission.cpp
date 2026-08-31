#include <gtest/gtest.h>

#include "command_admission.hpp"
#include "command_result_queue.hpp"

#include <algorithm>
#include <array>
#include <barrier>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace exchange::core::admission {
namespace {

sequencer::sequenceMessage makeNewOrder(const std::uint64_t clientId = 100,
                                        const std::string& clientCommandId = "NEW-1") {
    sequencer::sequenceMessage command{};
    command.clientId = domain::ClientId{clientId};
    command.clientCommandId.emplace(clientCommandId);
    command.instrumentId = domain::InstrumentId{1};
    command.configurationVersion = 1;
    command.type = sequencer::orderType::BUY;
    command.price = domain::Price{100};
    command.quantity = domain::Quantity{10};
    command.tif = task::TimeInForce::GTC;
    std::strcpy(command.symbol, "SPY");
    return command;
}

sequencer::sequenceMessage makeCancel(const std::uint64_t clientId = 100,
                                      const std::string& clientCommandId = "CANCEL-1") {
    sequencer::sequenceMessage command{};
    command.clientId = domain::ClientId{clientId};
    command.clientCommandId.emplace(clientCommandId);
    command.instrumentId = domain::InstrumentId{1};
    command.configurationVersion = 1;
    command.type = sequencer::orderType::CANCEL;
    command.targetOrderId.emplace(domain::TargetOrderId{42});
    std::strcpy(command.symbol, "SPY");
    return command;
}

matching_engine::ImmutableCommandResultBatch makeCompletedResult(const std::uint64_t clientId = 100,
                                                                 const std::string& clientCommandId = "NEW-1",
                                                                 const std::uint64_t commandSequence = 77) {
    std::vector<domain::BusinessEvent> events;
    events.emplace_back(domain::OrderRested{
        .eventId =
            {
                .commandSequence = domain::CommandSequence{commandSequence},
                .eventIndex = domain::EventIndex{0},
            },
        .orderId = domain::OrderId{commandSequence},
        .clientId = domain::ClientId{clientId},
        .instrumentId = domain::InstrumentId{1},
        .side = domain::Side::BUY,
        .price = domain::Price{100},
        .remainingQuantity = domain::Quantity{10},
    });
    return std::make_shared<const matching_engine::CommandResultBatch>(
        domain::CommandResultCorrelation{
            .clientId = domain::ClientId{clientId},
            .clientCommandId = domain::ClientCommandId{clientCommandId},
            .commandSequence = domain::CommandSequence{commandSequence},
        },
        matching_engine::ProcessingResult::APPLIED, std::move(events));
}

void expectConflict(const AdmissionDecision& decision) {
    EXPECT_EQ(decision.status, AdmissionStatus::CONFLICTING_REUSE);
    EXPECT_EQ(decision.rejectionReason, std::optional<domain::AdmissionRejectionReason>{
                                            domain::AdmissionRejectionReason::DUPLICATE_COMMAND_CONFLICT});
    EXPECT_EQ(decision.originalResult, nullptr);
}

} // namespace

TEST(CommandAdmissionIndexTest, IdenticalNewOrderRetransmissionIsInFlight) {
    CommandAdmissionIndex index(8);
    const sequencer::sequenceMessage command = makeNewOrder();

    EXPECT_EQ(index.reserve(command).status, AdmissionStatus::FIRST_SUBMISSION);
    const AdmissionDecision retransmission = index.reserve(command);

    EXPECT_EQ(retransmission.status, AdmissionStatus::IDENTICAL_IN_FLIGHT);
    EXPECT_EQ(retransmission.rejectionReason, std::nullopt);
    EXPECT_EQ(retransmission.originalResult, nullptr);
    EXPECT_EQ(index.size(), 1u);
}

TEST(CommandAdmissionIndexTest, IdenticalCancelRetransmissionIsInFlight) {
    CommandAdmissionIndex index(8);
    const sequencer::sequenceMessage command = makeCancel();

    EXPECT_EQ(index.reserve(command).status, AdmissionStatus::FIRST_SUBMISSION);
    EXPECT_EQ(index.reserve(command).status, AdmissionStatus::IDENTICAL_IN_FLIGHT);
    EXPECT_EQ(index.size(), 1u);
}

TEST(CommandAdmissionIndexTest, CompletedRetransmissionReturnsExactOriginalImmutableBatchAndEventIds) {
    CommandAdmissionIndex index(8);
    sequencer::sequenceMessage command = makeNewOrder();
    command.globalSequenceNumber = domain::CommandSequence{77};
    const matching_engine::ImmutableCommandResultBatch original = makeCompletedResult();
    ASSERT_EQ(index.reserve(command).status, AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_EQ(index.markSequenced(command), MarkSequencedStatus::SEQUENCED);
    ASSERT_EQ(index.complete(original), CompletionStatus::COMPLETED);

    sequencer::sequenceMessage retransmission = command;
    retransmission.globalSequenceNumber = domain::CommandSequence{123};
    const AdmissionDecision decision = index.reserve(retransmission);

    ASSERT_EQ(decision.status, AdmissionStatus::IDENTICAL_COMPLETED);
    EXPECT_EQ(decision.originalResult, original);
    EXPECT_EQ(index.completedResult(retransmission), original);
    ASSERT_EQ(decision.originalResult->events().size(), 1u);
    const auto* rested = std::get_if<domain::OrderRested>(&decision.originalResult->events().front());
    ASSERT_NE(rested, nullptr);
    EXPECT_EQ(rested->eventId.commandSequence, domain::CommandSequence{77});
    EXPECT_EQ(rested->eventId.eventIndex, domain::EventIndex{0});
}

TEST(CommandAdmissionIndexTest, EveryNewOrderBusinessFieldParticipatesInConflictDetection) {
    CommandAdmissionIndex index(8);
    const sequencer::sequenceMessage original = makeNewOrder();
    ASSERT_EQ(index.reserve(original).status, AdmissionStatus::FIRST_SUBMISSION);

    std::array<sequencer::sequenceMessage, 6> conflicts{
        original, original, original, original, original, original,
    };
    conflicts[0].instrumentId = domain::InstrumentId{2};
    conflicts[1].configurationVersion = 2;
    conflicts[2].type = sequencer::orderType::SELL;
    conflicts[3].price = domain::Price{101};
    conflicts[4].quantity = domain::Quantity{11};
    conflicts[5].tif = task::TimeInForce::IOC;

    for (const sequencer::sequenceMessage& conflict : conflicts) {
        expectConflict(index.reserve(conflict));
    }
    EXPECT_EQ(index.size(), 1u);
}

TEST(CommandAdmissionIndexTest, CancelTargetInstrumentAndCommandKindParticipateInConflictDetection) {
    CommandAdmissionIndex index(8);
    const sequencer::sequenceMessage original = makeCancel();
    ASSERT_EQ(index.reserve(original).status, AdmissionStatus::FIRST_SUBMISSION);

    sequencer::sequenceMessage differentTarget = original;
    differentTarget.targetOrderId = domain::TargetOrderId{43};
    expectConflict(index.reserve(differentTarget));

    sequencer::sequenceMessage differentInstrument = original;
    differentInstrument.instrumentId = domain::InstrumentId{2};
    expectConflict(index.reserve(differentInstrument));

    sequencer::sequenceMessage newOrderReuse = makeNewOrder(100, "CANCEL-1");
    expectConflict(index.reserve(newOrderReuse));
    EXPECT_EQ(index.size(), 1u);
}

TEST(CommandAdmissionIndexTest, DifferentClientsMayReuseSameClientCommandId) {
    CommandAdmissionIndex index(8);

    EXPECT_EQ(index.reserve(makeNewOrder(100, "SHARED-ID")).status, AdmissionStatus::FIRST_SUBMISSION);
    EXPECT_EQ(index.reserve(makeNewOrder(200, "SHARED-ID")).status, AdmissionStatus::FIRST_SUBMISSION);
    EXPECT_EQ(index.size(), 2u);
}

TEST(CommandAdmissionIndexTest, GeneratedAndPartitionMetadataDoNotAffectEquality) {
    CommandAdmissionIndex index(8);
    const sequencer::sequenceMessage original = makeNewOrder();
    ASSERT_EQ(index.reserve(original).status, AdmissionStatus::FIRST_SUBMISSION);

    sequencer::sequenceMessage retransmission = original;
    retransmission.shard_id = 3;
    retransmission.globalSequenceNumber = domain::CommandSequence{333};
    retransmission.orderId = domain::OrderId{333};
    std::strcpy(retransmission.symbol, "ALT");

    EXPECT_EQ(index.reserve(retransmission).status, AdmissionStatus::IDENTICAL_IN_FLIGHT);
}

TEST(CommandAdmissionIndexTest, ConcurrentIdenticalSubmissionsProduceExactlyOneReservation) {
    constexpr std::size_t THREAD_COUNT = 16;
    CommandAdmissionIndex index(8);
    const sequencer::sequenceMessage command = makeNewOrder();
    std::barrier start(static_cast<std::ptrdiff_t>(THREAD_COUNT));
    std::array<AdmissionStatus, THREAD_COUNT> statuses{};
    std::vector<std::thread> threads;
    threads.reserve(THREAD_COUNT);

    for (std::size_t threadIndex = 0; threadIndex < THREAD_COUNT; ++threadIndex) {
        threads.emplace_back([&, threadIndex]() {
            start.arrive_and_wait();
            statuses[threadIndex] = index.reserve(command).status;
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(std::count(statuses.begin(), statuses.end(), AdmissionStatus::FIRST_SUBMISSION), 1);
    EXPECT_EQ(std::count(statuses.begin(), statuses.end(), AdmissionStatus::IDENTICAL_IN_FLIGHT), 15);
    EXPECT_EQ(index.size(), 1u);
}

TEST(CommandAdmissionIndexTest, AbandonBeforeCommitAllowsRetry) {
    CommandAdmissionIndex index(1);
    const sequencer::sequenceMessage command = makeNewOrder();
    ASSERT_EQ(index.reserve(command).status, AdmissionStatus::FIRST_SUBMISSION);

    EXPECT_TRUE(index.abandonReservation(command));
    EXPECT_EQ(index.size(), 0u);
    EXPECT_EQ(index.reserve(command).status, AdmissionStatus::FIRST_SUBMISSION);
    EXPECT_EQ(index.size(), 1u);
}

TEST(CommandAdmissionLifecycleTest, ValidReservedSequencedCompletedTransitionAndIdempotentBinding) {
    CommandAdmissionIndex index(4);
    sequencer::sequenceMessage command = makeNewOrder();
    ASSERT_EQ(index.reserve(command).status, AdmissionStatus::FIRST_SUBMISSION);

    command.globalSequenceNumber = domain::CommandSequence{77};
    EXPECT_EQ(index.markSequenced(command), MarkSequencedStatus::SEQUENCED);
    EXPECT_EQ(index.markSequenced(command), MarkSequencedStatus::IDEMPOTENT);

    const matching_engine::ImmutableCommandResultBatch result = makeCompletedResult();
    EXPECT_EQ(index.complete(result), CompletionStatus::COMPLETED);
    EXPECT_EQ(index.completedResult(command), result);
}

TEST(CommandAdmissionLifecycleTest, SequencingRejectsZeroUnknownMismatchAndDifferentRebinding) {
    CommandAdmissionIndex index(4);
    sequencer::sequenceMessage original = makeNewOrder();
    ASSERT_EQ(index.reserve(original).status, AdmissionStatus::FIRST_SUBMISSION);

    EXPECT_EQ(index.markSequenced(original), MarkSequencedStatus::INVALID_SEQUENCE);

    sequencer::sequenceMessage unknown = makeNewOrder(100, "UNKNOWN");
    unknown.globalSequenceNumber = domain::CommandSequence{1};
    EXPECT_EQ(index.markSequenced(unknown), MarkSequencedStatus::UNKNOWN_RESERVATION);

    sequencer::sequenceMessage mismatch = original;
    mismatch.price = domain::Price{101};
    mismatch.globalSequenceNumber = domain::CommandSequence{1};
    EXPECT_EQ(index.markSequenced(mismatch), MarkSequencedStatus::COMMAND_MISMATCH);

    original.globalSequenceNumber = domain::CommandSequence{1};
    ASSERT_EQ(index.markSequenced(original), MarkSequencedStatus::SEQUENCED);
    original.globalSequenceNumber = domain::CommandSequence{2};
    EXPECT_EQ(index.markSequenced(original), MarkSequencedStatus::SEQUENCE_MISMATCH);
}

TEST(CommandAdmissionLifecycleTest, AbandonOnlyRemovesReservedRecords) {
    CommandAdmissionIndex index(4);
    sequencer::sequenceMessage command = makeNewOrder();
    ASSERT_EQ(index.reserve(command).status, AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(index.abandonReservation(command));
    ASSERT_EQ(index.size(), 0u);

    ASSERT_EQ(index.reserve(command).status, AdmissionStatus::FIRST_SUBMISSION);
    command.globalSequenceNumber = domain::CommandSequence{77};
    ASSERT_EQ(index.markSequenced(command), MarkSequencedStatus::SEQUENCED);
    EXPECT_FALSE(index.abandonReservation(command));
    EXPECT_EQ(index.reserve(command).status, AdmissionStatus::IDENTICAL_IN_FLIGHT);

    const matching_engine::ImmutableCommandResultBatch result = makeCompletedResult();
    ASSERT_EQ(index.complete(result), CompletionStatus::COMPLETED);
    EXPECT_FALSE(index.abandonReservation(command));
    const AdmissionDecision retransmission = index.reserve(command);
    EXPECT_EQ(retransmission.status, AdmissionStatus::IDENTICAL_COMPLETED);
    EXPECT_EQ(retransmission.originalResult, result);
}

TEST(CommandAdmissionLifecycleTest, CompletionRejectsInvalidUnknownWrongStateAndMismatchedCorrelation) {
    CommandAdmissionIndex index(8);
    sequencer::sequenceMessage command = makeNewOrder();
    ASSERT_EQ(index.reserve(command).status, AdmissionStatus::FIRST_SUBMISSION);

    EXPECT_EQ(index.complete({}), CompletionStatus::INVALID_BATCH);
    EXPECT_EQ(index.complete(makeCompletedResult(0, "NEW-1", 77)), CompletionStatus::INVALID_BATCH);
    EXPECT_EQ(index.complete(makeCompletedResult(100, "NEW-1", 0)), CompletionStatus::INVALID_BATCH);
    EXPECT_EQ(index.complete(makeCompletedResult()), CompletionStatus::WRONG_STATE);
    EXPECT_EQ(index.complete(makeCompletedResult(999, "NEW-1", 77)), CompletionStatus::UNKNOWN_RESERVATION);
    EXPECT_EQ(index.complete(makeCompletedResult(100, "OTHER", 77)), CompletionStatus::UNKNOWN_RESERVATION);

    command.globalSequenceNumber = domain::CommandSequence{77};
    ASSERT_EQ(index.markSequenced(command), MarkSequencedStatus::SEQUENCED);
    EXPECT_EQ(index.complete(makeCompletedResult(100, "NEW-1", 78)), CompletionStatus::CORRELATION_MISMATCH);

    const matching_engine::ImmutableCommandResultBatch original = makeCompletedResult();
    ASSERT_EQ(index.complete(original), CompletionStatus::COMPLETED);
    const matching_engine::ImmutableCommandResultBatch replacement = makeCompletedResult(100, "NEW-1", 77);
    EXPECT_EQ(index.complete(replacement), CompletionStatus::WRONG_STATE);
    EXPECT_EQ(index.completedResult(command), original);
    EXPECT_NE(index.completedResult(command), replacement);
}

TEST(CommandAdmissionIndexTest, CapacityFailsClosedWithoutEvictingCompletedRecordOrCreatingBusinessResult) {
    CommandAdmissionIndex index(1);
    sequencer::sequenceMessage retained = makeNewOrder(100, "RETAINED");
    retained.globalSequenceNumber = domain::CommandSequence{77};
    const matching_engine::ImmutableCommandResultBatch original = makeCompletedResult(100, "RETAINED", 77);
    ASSERT_EQ(index.reserve(retained).status, AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_EQ(index.markSequenced(retained), MarkSequencedStatus::SEQUENCED);
    ASSERT_EQ(index.complete(original), CompletionStatus::COMPLETED);

    const AdmissionDecision unavailable = index.reserve(makeNewOrder(200, "NEW"));

    EXPECT_EQ(unavailable.status, AdmissionStatus::ADMISSION_UNAVAILABLE);
    EXPECT_EQ(unavailable.rejectionReason, std::nullopt);
    EXPECT_EQ(unavailable.originalResult, nullptr);
    EXPECT_EQ(index.size(), 1u);
    EXPECT_EQ(index.completedResult(retained), original);
    EXPECT_EQ(index.statistics().admissionUnavailable, 1u);
}

TEST(CommandAdmissionIndexTest, RejectsZeroCapacity) {
    EXPECT_THROW(CommandAdmissionIndex(0), std::invalid_argument);
}

} // namespace exchange::core::admission
