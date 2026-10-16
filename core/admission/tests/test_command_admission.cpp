#include <gtest/gtest.h>

#include "command_admission.hpp"
#include "admission_test_access.hpp"
#include "command_result_queue.hpp"
#include "shared_queue.hpp"

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

TEST(CommandAdmissionIndexTest, StagingSaturationRefusesWithoutAcceptingARecordAndIdenticalRetryStagesOnce) {
    CommandAdmissionIndex index(8);
    core::SharedQueue<sequencer::sequenceMessage> staging(1);
    const auto first = makeNewOrder(100, "FIRST");
    const auto retry = makeNewOrder(200, "RETRY");
    ASSERT_EQ(index.reserveAndStage(first, staging).status, AdmissionStatus::FIRST_SUBMISSION);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto refusal = index.reserveAndStage(retry, staging);
        EXPECT_EQ(refusal.status, AdmissionStatus::ADMISSION_UNAVAILABLE);
        EXPECT_EQ(refusal.rejectionReason, domain::AdmissionRejectionReason::GATEWAY_BUSY);
        EXPECT_EQ(refusal.originalResult, nullptr);
        EXPECT_EQ(index.size(), 1U);
        EXPECT_EQ(index.statistics().firstSubmissions, 1U);
    }
    sequencer::sequenceMessage staged{};
    ASSERT_TRUE(staging.pop(staged));
    EXPECT_EQ(staged.clientCommandId, first.clientCommandId);
    EXPECT_EQ(staged.clientId, first.clientId);
    EXPECT_TRUE(staging.empty());
    ASSERT_EQ(index.reserveAndStage(retry, staging).status, AdmissionStatus::FIRST_SUBMISSION);
    EXPECT_EQ(index.size(), 2U);
    EXPECT_EQ(index.statistics().firstSubmissions, 2U);
    EXPECT_EQ(index.statistics().admissionUnavailable, 2U);
    ASSERT_TRUE(staging.pop(staged));
    EXPECT_EQ(staged.clientCommandId, retry.clientCommandId);
    EXPECT_EQ(staged.clientId, retry.clientId);
    EXPECT_EQ(staged.quantity, retry.quantity);
    EXPECT_EQ(staged.price, retry.price);
    EXPECT_FALSE(staging.pop(staged));
}

TEST(CommandAdmissionIndexTest, StagingLookupSuppressesDuplicateAndConflictingWorkEvenWhenClosedOrFull) {
    CommandAdmissionIndex index(8);
    core::SharedQueue<sequencer::sequenceMessage> staging(1);
    auto original = makeNewOrder();
    ASSERT_EQ(index.reserveAndStage(original, staging).status, AdmissionStatus::FIRST_SUBMISSION);
    EXPECT_EQ(index.reserveAndStage(original, staging).status, AdmissionStatus::IDENTICAL_IN_FLIGHT);
    auto conflict = original;
    conflict.quantity = domain::Quantity{11};
    expectConflict(index.reserveAndStage(conflict, staging));
    sequencer::sequenceMessage staged{};
    ASSERT_TRUE(staging.pop(staged));
    EXPECT_EQ(staged.quantity, original.quantity);
    EXPECT_FALSE(staging.pop(staged));
    original.globalSequenceNumber = domain::CommandSequence{77};
    ASSERT_EQ(index.markSequenced(original), MarkSequencedStatus::SEQUENCED);
    const auto completed = makeCompletedResult();
    ASSERT_EQ(index.complete(completed), CompletionStatus::COMPLETED);
    CommandAdmissionIndexTestAccess::setAdmissionOpen(index, false);
    const auto blocker = makeCancel(200, "BLOCKER");
    ASSERT_TRUE(staging.push(blocker));

    const auto retransmission = index.reserveAndStage(original, staging);
    EXPECT_EQ(retransmission.status, AdmissionStatus::IDENTICAL_COMPLETED);
    EXPECT_EQ(retransmission.rejectionReason, std::nullopt);
    EXPECT_EQ(retransmission.originalResult, completed);
    expectConflict(index.reserveAndStage(conflict, staging));
    const auto unavailable = index.reserveAndStage(makeNewOrder(200, "NEW"), staging);
    EXPECT_EQ(unavailable.status, AdmissionStatus::ADMISSION_UNAVAILABLE);
    EXPECT_EQ(unavailable.rejectionReason, domain::AdmissionRejectionReason::EXCHANGE_RUN_UNAVAILABLE);
    EXPECT_EQ(index.size(), 1U);
    EXPECT_EQ(index.statistics().firstSubmissions, 1U);
    ASSERT_TRUE(staging.pop(staged));
    EXPECT_EQ(staged.clientCommandId, blocker.clientCommandId);
    EXPECT_FALSE(staging.pop(staged));
}

TEST(CommandAdmissionIndexTest, TableCapacityAndValidationFailurePublishNoStagedWork) {
    CommandAdmissionIndex index(1);
    core::SharedQueue<sequencer::sequenceMessage> staging(1);
    const auto first = makeNewOrder();
    ASSERT_EQ(index.reserveAndStage(first, staging).status, AdmissionStatus::FIRST_SUBMISSION);
    sequencer::sequenceMessage staged{};
    ASSERT_TRUE(staging.pop(staged));
    auto next = makeNewOrder(200, "NEXT");
    const auto capacity = index.reserveAndStage(next, staging);
    EXPECT_EQ(capacity.status, AdmissionStatus::ADMISSION_UNAVAILABLE);
    EXPECT_EQ(capacity.rejectionReason, std::nullopt);
    EXPECT_TRUE(staging.empty());
    next.quantity = domain::Quantity{};
    EXPECT_THROW((void)index.reserveAndStage(next, staging), std::invalid_argument);
    EXPECT_TRUE(staging.empty());
    EXPECT_EQ(index.size(), 1U);
    EXPECT_EQ(index.statistics().firstSubmissions, 1U);
    EXPECT_EQ(index.statistics().admissionUnavailable, 1U);
    EXPECT_EQ(index.reserveAndStage(first, staging).status, AdmissionStatus::IDENTICAL_IN_FLIGHT);
    EXPECT_TRUE(staging.empty());
}

TEST(CommandAdmissionIndexTest, ConcurrentDistinctProducersAcceptExactlyTheStagingCapacityAndRetryRefusals) {
    constexpr std::size_t PRODUCERS = 16;
    constexpr std::size_t CAPACITY = 8;
    CommandAdmissionIndex index(PRODUCERS);
    core::SharedQueue<sequencer::sequenceMessage> staging(CAPACITY);
    std::array<sequencer::sequenceMessage, PRODUCERS> commands{};
    std::array<AdmissionDecision, PRODUCERS> decisions{};
    std::barrier start(static_cast<std::ptrdiff_t>(PRODUCERS));
    std::vector<std::thread> producers;
    for (std::size_t i = 0; i < PRODUCERS; ++i) {
        commands[i] = makeNewOrder(100 + i, "PRODUCER-" + std::to_string(i));
        producers.emplace_back([&, i] {
            start.arrive_and_wait();
            decisions[i] = index.reserveAndStage(commands[i], staging);
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    std::array<bool, PRODUCERS> seen{};
    sequencer::sequenceMessage staged{};
    std::size_t stagedCount = 0;
    while (staging.pop(staged)) {
        std::size_t i = 0;
        while (i < PRODUCERS && commands[i].clientCommandId != staged.clientCommandId) {
            ++i;
        }
        ASSERT_LT(i, PRODUCERS);
        EXPECT_FALSE(seen[i]);
        seen[i] = true;
        EXPECT_EQ(staged.clientId, commands[i].clientId);
        ++stagedCount;
    }
    EXPECT_EQ(stagedCount, CAPACITY);
    EXPECT_EQ(index.size(), CAPACITY);
    EXPECT_EQ(index.statistics().firstSubmissions, CAPACITY);
    EXPECT_EQ(index.statistics().admissionUnavailable, PRODUCERS - CAPACITY);
    for (std::size_t i = 0; i < PRODUCERS; ++i) {
        if (seen[i]) {
            EXPECT_EQ(decisions[i].status, AdmissionStatus::FIRST_SUBMISSION);
            EXPECT_EQ(decisions[i].rejectionReason, std::nullopt);
            EXPECT_EQ(index.reserveAndStage(commands[i], staging).status, AdmissionStatus::IDENTICAL_IN_FLIGHT);
        } else {
            EXPECT_EQ(decisions[i].status, AdmissionStatus::ADMISSION_UNAVAILABLE);
            EXPECT_EQ(decisions[i].rejectionReason, domain::AdmissionRejectionReason::GATEWAY_BUSY);
            ASSERT_EQ(index.reserveAndStage(commands[i], staging).status, AdmissionStatus::FIRST_SUBMISSION);
            ASSERT_TRUE(staging.pop(staged));
            EXPECT_EQ(staged.clientCommandId, commands[i].clientCommandId);
        }
        EXPECT_TRUE(staging.empty());
    }
    EXPECT_EQ(index.size(), PRODUCERS);
    EXPECT_EQ(index.statistics().firstSubmissions, PRODUCERS);
}

TEST(CommandAdmissionIndexTest, ConcurrentIdenticalProducersReserveAndStageExactlyOnce) {
    constexpr std::size_t PRODUCERS = 8;
    CommandAdmissionIndex index(PRODUCERS);
    core::SharedQueue<sequencer::sequenceMessage> staging(1);
    const auto command = makeNewOrder();
    std::array<AdmissionStatus, PRODUCERS> statuses{};
    std::barrier start(static_cast<std::ptrdiff_t>(PRODUCERS));
    std::vector<std::thread> producers;
    for (std::size_t i = 0; i < PRODUCERS; ++i) {
        producers.emplace_back([&, i] {
            start.arrive_and_wait();
            statuses[i] = index.reserveAndStage(command, staging).status;
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    EXPECT_EQ(std::count(statuses.begin(), statuses.end(), AdmissionStatus::FIRST_SUBMISSION), 1);
    EXPECT_EQ(std::count(statuses.begin(), statuses.end(), AdmissionStatus::IDENTICAL_IN_FLIGHT), PRODUCERS - 1);
    EXPECT_EQ(index.size(), 1U);
    EXPECT_EQ(index.statistics().firstSubmissions, 1U);
    EXPECT_EQ(index.statistics().admissionUnavailable, 0U);
    sequencer::sequenceMessage staged{};
    ASSERT_TRUE(staging.pop(staged));
    EXPECT_EQ(staged.clientCommandId, command.clientCommandId);
    EXPECT_FALSE(staging.pop(staged));
}

TEST(CommandAdmissionIndexTest, RejectsZeroCapacity) {
    EXPECT_THROW(CommandAdmissionIndex(0), std::invalid_argument);
    EXPECT_THROW(CommandAdmissionIndex(1, domain::ExchangeRunId{}), std::invalid_argument);
}

TEST(CommandAdmissionIndexTest, RunBoundAdmissionStartsClosedWithoutCreatingRecords) {
    CommandAdmissionIndex index(8, domain::ExchangeRunId{42});
    const auto decision = index.reserve(makeNewOrder());
    EXPECT_EQ(decision.status, AdmissionStatus::ADMISSION_UNAVAILABLE);
    EXPECT_EQ(decision.rejectionReason, domain::AdmissionRejectionReason::EXCHANGE_RUN_UNAVAILABLE);
    EXPECT_EQ(decision.originalResult, nullptr);
    EXPECT_EQ(index.size(), 0U);
    EXPECT_EQ(index.statistics().firstSubmissions, 0U);
    EXPECT_EQ(index.statistics().admissionUnavailable, 1U);
}

TEST(CommandAdmissionIndexTest, ClosedGateReasonPrecedesFullCapacityButNotRetransmissionsOrConflicts) {
    CommandAdmissionIndex index(2);
    const auto reserved = makeNewOrder(100, "RESERVED");
    auto completed = makeNewOrder(100, "COMPLETED");
    ASSERT_EQ(index.reserve(reserved).status, AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_EQ(index.reserve(completed).status, AdmissionStatus::FIRST_SUBMISSION);
    completed.globalSequenceNumber = domain::CommandSequence{77};
    ASSERT_EQ(index.markSequenced(completed), MarkSequencedStatus::SEQUENCED);
    const auto original = makeCompletedResult(100, "COMPLETED");
    ASSERT_EQ(index.complete(original), CompletionStatus::COMPLETED);
    ASSERT_EQ(index.size(), index.capacity());
    CommandAdmissionIndexTestAccess::setAdmissionOpen(index, false);

    const auto fresh = makeNewOrder(200, "NEW");
    const auto closed = index.reserve(fresh);
    EXPECT_EQ(closed.status, AdmissionStatus::ADMISSION_UNAVAILABLE);
    EXPECT_EQ(closed.rejectionReason, domain::AdmissionRejectionReason::EXCHANGE_RUN_UNAVAILABLE);
    EXPECT_EQ(closed.originalResult, nullptr);
    const auto inFlight = index.reserve(reserved);
    EXPECT_EQ(inFlight.status, AdmissionStatus::IDENTICAL_IN_FLIGHT);
    EXPECT_EQ(inFlight.rejectionReason, std::nullopt);
    const auto retransmission = index.reserve(completed);
    EXPECT_EQ(retransmission.status, AdmissionStatus::IDENTICAL_COMPLETED);
    EXPECT_EQ(retransmission.rejectionReason, std::nullopt);
    EXPECT_EQ(retransmission.originalResult, original);
    for (auto conflict : {reserved, completed}) {
        conflict.quantity = domain::Quantity{11};
        expectConflict(index.reserve(conflict));
    }

    CommandAdmissionIndexTestAccess::setAdmissionOpen(index, true);
    const auto capacityOnly = index.reserve(fresh);
    EXPECT_EQ(capacityOnly.status, AdmissionStatus::ADMISSION_UNAVAILABLE);
    EXPECT_EQ(capacityOnly.rejectionReason, std::nullopt);
    EXPECT_EQ(capacityOnly.originalResult, nullptr);
    EXPECT_EQ(index.size(), 2U);
    EXPECT_EQ(index.completedResult(completed), original);
}

TEST(CommandAdmissionIndexTest, ClosingPreservesReservedSequencedAndCompletedRetransmissionsAndConflicts) {
    CommandAdmissionIndex index(8);
    const auto reserved = makeNewOrder(100, "RESERVED");
    auto sequenced = makeCancel(100, "SEQUENCED");
    auto completed = makeNewOrder(100, "COMPLETED");
    ASSERT_EQ(index.reserve(reserved).status, AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_EQ(index.reserve(sequenced).status, AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_EQ(index.reserve(completed).status, AdmissionStatus::FIRST_SUBMISSION);
    sequenced.globalSequenceNumber = domain::CommandSequence{76};
    completed.globalSequenceNumber = domain::CommandSequence{77};
    ASSERT_EQ(index.markSequenced(sequenced), MarkSequencedStatus::SEQUENCED);
    ASSERT_EQ(index.markSequenced(completed), MarkSequencedStatus::SEQUENCED);
    CommandAdmissionIndexTestAccess::setAdmissionOpen(index, false);
    const auto original = makeCompletedResult(100, "COMPLETED");
    ASSERT_EQ(index.complete(original), CompletionStatus::COMPLETED);

    EXPECT_EQ(index.reserve(reserved).status, AdmissionStatus::IDENTICAL_IN_FLIGHT);
    EXPECT_EQ(index.reserve(sequenced).status, AdmissionStatus::IDENTICAL_IN_FLIGHT);
    const auto retransmission = index.reserve(completed);
    EXPECT_EQ(retransmission.status, AdmissionStatus::IDENTICAL_COMPLETED);
    EXPECT_EQ(retransmission.originalResult, original);
    EXPECT_EQ(index.completedResult(completed), original);
    for (auto conflict : {reserved, completed}) {
        conflict.quantity = domain::Quantity{11};
        expectConflict(index.reserve(conflict));
    }
    auto cancelConflict = sequenced;
    cancelConflict.targetOrderId = domain::TargetOrderId{43};
    expectConflict(index.reserve(cancelConflict));
    EXPECT_EQ(index.reserve(makeNewOrder(200, "NEW")).status, AdmissionStatus::ADMISSION_UNAVAILABLE);
    EXPECT_EQ(index.size(), 3U);

    CommandAdmissionIndexTestAccess::setAdmissionOpen(index, true);
    EXPECT_EQ(index.reserve(makeNewOrder(200, "NEW")).status, AdmissionStatus::FIRST_SUBMISSION);
    EXPECT_EQ(index.completedResult(completed), original);
}

} // namespace exchange::core::admission
