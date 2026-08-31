#include <atomic>
#include <array>
#include <barrier>
#include <chrono>
#include <gtest/gtest.h>
#include <iostream>
#include <limits>
#include <thread>

#include "../../core/admission/include/admission_completion_consumer.hpp"
#include "../../bus/include/bus.hpp"
#include "../../core/fix/include/fix_task.hpp"
#include "../../core/shared_queue/include/shared_queue.hpp"
#include "../../matching_engine/include/matching_engine.hpp"
#include "../../sequencer/include/sequencer.hpp"

// Pre-include STL headers so the throw(...) macro hack doesn't break them
#include <cstring>
#include <memory>
#include <vector>

#if __cplusplus >= 201703L
#define throw(...)
#endif

#include <quickfix/Fields.h>
#include <quickfix/Message.h>
#include <quickfix/Session.h>
#include <quickfix/fix44/NewOrderSingle.h>

#if __cplusplus >= 201703L
#undef throw
#endif

using namespace exchange;

namespace {
// Testable sequencer subclass to call run without infinite loop
class TestableSequencer : public sequencer::Sequencer {
public:
    using sequencer::Sequencer::Sequencer;
    bool processOnce() {
        return processNext();
    }
    bool drainActive() {
        return drainAvailable();
    }
};

// Testable matching engine subclass
class TestableMatchingEngine : public matching_engine::MatchingEngine {
public:
    using matching_engine::MatchingEngine::MatchingEngine;
    void invokeDrain() {
        drainQueue(sequencerQueue, "Sequencer");
    }
    std::size_t activeOrderCount() const {
        return activeOrders.size();
    }
};

FIX::Message makeFixNewOrder(const std::string& clientCommandId, const double price = 100.0) {
    FIX::Message message;
    message.getHeader().setField(FIX::MsgType("D"));
    message.setField(FIX::ClOrdID(clientCommandId));
    message.setField(FIX::Symbol("SPY"));
    message.setField(FIX::Side(FIX::Side_BUY));
    message.setField(FIX::OrderQty(10));
    message.setField(FIX::Price(price));
    message.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    message.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));
    return message;
}

FIX::Message makeFixCancel(const std::string& clientCommandId, const std::string& orderId) {
    FIX::Message message;
    message.getHeader().setField(FIX::MsgType("F"));
    message.setField(FIX::ClOrdID(clientCommandId));
    message.setField(FIX::StringField(FIX::FIELD::OrderID, orderId));
    message.setField(FIX::OrigClOrdID("UNRELATED-PROTOCOL-CORRELATION"));
    message.setField(FIX::Symbol("SPY"));
    return message;
}
} // namespace

TEST(SequencerCancelNormalizationTest, ValidFixCancelGetsDistinctSequenceAndReachesMatching) {
    core::SharedQueue<sequencer::sequenceMessage> sequencingIngressQueue(16);
    core::SharedQueue<sequencer::sequenceMessage> matchingQueue(16);
    core::Bus bus(16);
    const FIX::SessionID session("FIX.4.4", "CANCEL-CLIENT", "EXCHANGE");
    const core::fix::ClientIdentityResolver clientIdentityResolver({{session, domain::ClientId{500}}});
    core::admission::CommandAdmissionIndex admissionIndex(16);
    core::task::FixTask fixTask(sequencingIngressQueue, bus, clientIdentityResolver, admissionIndex);
    TestableSequencer sequencer(sequencingIngressQueue, matchingQueue, admissionIndex);
    matching_engine::BoundedCommandResultQueue resultQueue(4);
    TestableMatchingEngine matchingEngine(&matchingQueue, bus, resultQueue);

    fixTask.fromApp(makeFixNewOrder("NEW-1"), session);
    ASSERT_TRUE(fixTask.processNextStagedCommand());
    ASSERT_TRUE(sequencer.processOnce());

    sequencer::sequenceMessage sequencedNew{};
    ASSERT_TRUE(matchingQueue.pop(sequencedNew));
    EXPECT_EQ(sequencedNew.globalSequenceNumber, domain::CommandSequence{1});
    EXPECT_EQ(sequencedNew.orderId, domain::OrderId{1});
    ASSERT_TRUE(matchingQueue.push(sequencedNew));
    matchingEngine.invokeDrain();
    matching_engine::ImmutableCommandResultBatch newResult;
    ASSERT_TRUE(resultQueue.tryPop(newResult));
    ASSERT_EQ(newResult->events().size(), 1u);
    ASSERT_NE(std::get_if<domain::OrderRested>(&newResult->events().front()), nullptr);

    fixTask.fromApp(makeFixCancel("INVALID-CANCEL", "0"), session);
    sequencer::sequenceMessage rejectedCancel{};
    EXPECT_FALSE(fixTask.processNextStagedCommand());
    EXPECT_FALSE(sequencingIngressQueue.pop(rejectedCancel));
    EXPECT_FALSE(sequencer.processOnce());

    fixTask.fromApp(makeFixCancel("CANCEL-2", "1"), session);
    sequencer::sequenceMessage normalizedCancel{};
    ASSERT_TRUE(fixTask.processNextStagedCommand());
    ASSERT_TRUE(sequencingIngressQueue.pop(normalizedCancel));
    EXPECT_EQ(normalizedCancel.globalSequenceNumber, domain::CommandSequence{});
    EXPECT_EQ(normalizedCancel.orderId, domain::OrderId{});
    EXPECT_EQ(normalizedCancel.targetOrderId, std::optional<domain::TargetOrderId>{domain::TargetOrderId{1}});
    ASSERT_TRUE(sequencingIngressQueue.push(normalizedCancel));
    ASSERT_TRUE(sequencer.processOnce());

    sequencer::sequenceMessage sequencedCancel{};
    ASSERT_TRUE(matchingQueue.pop(sequencedCancel));
    EXPECT_EQ(sequencedCancel.globalSequenceNumber, domain::CommandSequence{2});
    EXPECT_NE(sequencedCancel.globalSequenceNumber, sequencedNew.globalSequenceNumber);
    EXPECT_EQ(sequencedCancel.orderId, domain::OrderId{});
    EXPECT_EQ(sequencedCancel.targetOrderId, normalizedCancel.targetOrderId);
    ASSERT_TRUE(matchingQueue.push(sequencedCancel));
    matchingEngine.invokeDrain();

    matching_engine::ImmutableCommandResultBatch cancelResult;
    ASSERT_TRUE(resultQueue.tryPop(cancelResult));
    EXPECT_EQ(cancelResult->commandSequence(), domain::CommandSequence{2});
    ASSERT_EQ(cancelResult->events().size(), 1u);
    const auto* cancelled = std::get_if<domain::OrderCancelled>(&cancelResult->events().front());
    ASSERT_NE(cancelled, nullptr);
    EXPECT_EQ(cancelled->orderId, domain::OrderId{1});
    EXPECT_EQ(cancelled->cancelledQuantity, domain::Quantity{10});
    EXPECT_EQ(cancelled->reason, domain::CancelReason::CLIENT_REQUESTED);
}

TEST(SequencerClientIdentityTest, OwnershipUsesConfiguredClientIdAcrossSessions) {
    core::SharedQueue<sequencer::sequenceMessage> sequencingIngressQueue(16);
    core::SharedQueue<sequencer::sequenceMessage> matchingQueue(16);
    core::Bus bus(16);
    const FIX::SessionID primarySession("FIX.4.4", "EXCHANGE", "CLIENT-A");
    const FIX::SessionID alternateSession("FIX.4.4", "EXCHANGE", "CLIENT-A-ALT");
    const FIX::SessionID otherClientSession("FIX.4.4", "EXCHANGE", "CLIENT-B");
    const FIX::SessionID unknownSession("FIX.4.4", "EXCHANGE", "UNKNOWN");
    const core::fix::ClientIdentityResolver clientIdentityResolver({
        {primarySession, domain::ClientId{7001}},
        {alternateSession, domain::ClientId{7001}},
        {otherClientSession, domain::ClientId{8002}},
    });
    core::admission::CommandAdmissionIndex admissionIndex(16);
    core::task::FixTask fixTask(sequencingIngressQueue, bus, clientIdentityResolver, admissionIndex);
    TestableSequencer sequencer(sequencingIngressQueue, matchingQueue, admissionIndex);
    matching_engine::BoundedCommandResultQueue resultQueue(8);
    TestableMatchingEngine matchingEngine(&matchingQueue, bus, resultQueue);

    const auto normalizeAndSequence = [&](const FIX::Message& message,
                                          const FIX::SessionID& session) -> std::optional<sequencer::sequenceMessage> {
        fixTask.fromApp(message, session);
        sequencer::sequenceMessage normalized{};
        if (!fixTask.processNextStagedCommand() || !sequencingIngressQueue.pop(normalized)) {
            return std::nullopt;
        }
        if (!sequencingIngressQueue.push(normalized)) {
            return std::nullopt;
        }
        if (!sequencer.processOnce()) {
            return std::nullopt;
        }
        sequencer::sequenceMessage sequenced{};
        if (!matchingQueue.pop(sequenced)) {
            return std::nullopt;
        }
        return sequenced;
    };
    const auto match = [&](const sequencer::sequenceMessage& command) -> matching_engine::ImmutableCommandResultBatch {
        if (!matchingQueue.push(command)) {
            return {};
        }
        matchingEngine.invokeDrain();
        matching_engine::ImmutableCommandResultBatch result;
        if (!resultQueue.tryPop(result)) {
            return {};
        }
        return result;
    };

    const auto firstNew = normalizeAndSequence(makeFixNewOrder("NEW-1"), primarySession);
    ASSERT_TRUE(firstNew.has_value());
    EXPECT_EQ(firstNew->globalSequenceNumber, domain::CommandSequence{1});
    EXPECT_EQ(firstNew->orderId, domain::OrderId{1});
    EXPECT_EQ(firstNew->clientId, domain::ClientId{7001});
    ASSERT_TRUE(match(*firstNew));

    const auto secondNew = normalizeAndSequence(makeFixNewOrder("NEW-2"), primarySession);
    ASSERT_TRUE(secondNew.has_value());
    EXPECT_EQ(secondNew->globalSequenceNumber, domain::CommandSequence{2});
    EXPECT_EQ(secondNew->orderId, domain::OrderId{2});
    EXPECT_EQ(secondNew->clientId, domain::ClientId{7001});
    ASSERT_TRUE(match(*secondNew));

    fixTask.fromApp(makeFixCancel("UNKNOWN", "1"), unknownSession);
    sequencer::sequenceMessage rejectedIdentity{};
    EXPECT_FALSE(fixTask.processNextStagedCommand());
    EXPECT_FALSE(sequencingIngressQueue.pop(rejectedIdentity));
    EXPECT_FALSE(sequencer.processOnce());
    EXPECT_TRUE(matchingQueue.empty());

    const auto alternateCancel = normalizeAndSequence(makeFixCancel("ALT-CANCEL", "1"), alternateSession);
    ASSERT_TRUE(alternateCancel.has_value());
    EXPECT_EQ(alternateCancel->globalSequenceNumber, domain::CommandSequence{3});
    EXPECT_EQ(alternateCancel->orderId, domain::OrderId{});
    EXPECT_EQ(alternateCancel->targetOrderId, std::optional<domain::TargetOrderId>{domain::TargetOrderId{1}});
    EXPECT_EQ(alternateCancel->clientId, domain::ClientId{7001});
    const matching_engine::ImmutableCommandResultBatch alternateResult = match(*alternateCancel);
    ASSERT_TRUE(alternateResult);
    ASSERT_EQ(alternateResult->events().size(), 1u);
    const auto* cancelled = std::get_if<domain::OrderCancelled>(&alternateResult->events().front());
    ASSERT_NE(cancelled, nullptr);
    EXPECT_EQ(cancelled->reason, domain::CancelReason::CLIENT_REQUESTED);

    const auto otherClientCancel = normalizeAndSequence(makeFixCancel("OTHER-CANCEL", "2"), otherClientSession);
    ASSERT_TRUE(otherClientCancel.has_value());
    EXPECT_EQ(otherClientCancel->globalSequenceNumber, domain::CommandSequence{4});
    EXPECT_EQ(otherClientCancel->clientId, domain::ClientId{8002});
    const matching_engine::ImmutableCommandResultBatch otherResult = match(*otherClientCancel);
    ASSERT_TRUE(otherResult);
    ASSERT_EQ(otherResult->events().size(), 1u);
    const auto* rejected = std::get_if<domain::CommandRejected>(&otherResult->events().front());
    ASSERT_NE(rejected, nullptr);
    EXPECT_EQ(rejected->reason, domain::CommandRejectionReason::NOT_OWNER);
}

TEST(SequencerCommandAdmissionTest, DuplicateConflictAndCapacityNeverEnterSequencing) {
    core::SharedQueue<sequencer::sequenceMessage> sequencingIngressQueue(16);
    core::SharedQueue<sequencer::sequenceMessage> matchingQueue(16);
    core::Bus bus(16);
    const FIX::SessionID session("FIX.4.4", "EXCHANGE", "ADMISSION-CLIENT");
    const core::fix::ClientIdentityResolver clientIdentityResolver({{session, domain::ClientId{9001}}});
    core::admission::CommandAdmissionIndex admissionIndex(2);
    core::task::FixTask fixTask(sequencingIngressQueue, bus, clientIdentityResolver, admissionIndex);
    TestableSequencer sequencer(sequencingIngressQueue, matchingQueue, admissionIndex);

    const auto submitAndSequence = [&](const FIX::Message& message) -> std::optional<sequencer::sequenceMessage> {
        fixTask.fromApp(message, session);
        if (!fixTask.processNextStagedCommand() || !sequencer.processOnce()) {
            return std::nullopt;
        }
        sequencer::sequenceMessage sequenced{};
        if (!matchingQueue.pop(sequenced)) {
            return std::nullopt;
        }
        return sequenced;
    };

    const auto first = submitAndSequence(makeFixNewOrder("COMMAND-1"));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->globalSequenceNumber, domain::CommandSequence{1});

    fixTask.fromApp(makeFixNewOrder("COMMAND-1"), session);
    sequencer::sequenceMessage duplicate{};
    EXPECT_FALSE(fixTask.processNextStagedCommand());
    EXPECT_FALSE(sequencingIngressQueue.pop(duplicate));
    EXPECT_FALSE(sequencer.processOnce());
    EXPECT_TRUE(matchingQueue.empty());

    fixTask.fromApp(makeFixNewOrder("COMMAND-1", 101.0), session);
    sequencer::sequenceMessage conflict{};
    EXPECT_FALSE(fixTask.processNextStagedCommand());
    EXPECT_FALSE(sequencingIngressQueue.pop(conflict));
    EXPECT_FALSE(sequencer.processOnce());
    EXPECT_TRUE(matchingQueue.empty());

    const auto second = submitAndSequence(makeFixNewOrder("COMMAND-2"));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->globalSequenceNumber, domain::CommandSequence{2});

    fixTask.fromApp(makeFixNewOrder("COMMAND-3"), session);
    sequencer::sequenceMessage unavailable{};
    EXPECT_FALSE(fixTask.processNextStagedCommand());
    EXPECT_FALSE(sequencingIngressQueue.pop(unavailable));
    EXPECT_FALSE(sequencer.processOnce());
    EXPECT_TRUE(matchingQueue.empty());

    const core::admission::AdmissionStatistics statistics = admissionIndex.statistics();
    EXPECT_EQ(statistics.firstSubmissions, 2u);
    EXPECT_EQ(statistics.identicalInFlight, 1u);
    EXPECT_EQ(statistics.conflictingReuse, 1u);
    EXPECT_EQ(statistics.admissionUnavailable, 1u);
    EXPECT_EQ(admissionIndex.size(), 2u);
}

TEST(SequencerHandoffTest, FullMatchingQueueRetainsSequencedCommandWithoutOvertakingOrResequencing) {
    core::SharedQueue<sequencer::sequenceMessage> sequencingIngressQueue(4);
    core::SharedQueue<sequencer::sequenceMessage> matchingQueue(1);
    core::Bus bus(8);
    const FIX::SessionID session("FIX.4.4", "EXCHANGE", "SEQUENCER-SATURATION");
    const core::fix::ClientIdentityResolver resolver({{session, domain::ClientId{9100}}});
    core::admission::CommandAdmissionIndex admissionIndex(4);
    core::task::FixTask fixTask(sequencingIngressQueue, bus, resolver, admissionIndex);
    TestableSequencer sequencer(sequencingIngressQueue, matchingQueue, admissionIndex);

    sequencer::sequenceMessage blocker{};
    blocker.globalSequenceNumber = domain::CommandSequence{999};
    ASSERT_TRUE(matchingQueue.push(blocker));
    fixTask.fromApp(makeFixNewOrder("FIRST"), session);
    fixTask.fromApp(makeFixNewOrder("SECOND"), session);
    ASSERT_TRUE(fixTask.processNextStagedCommand());
    ASSERT_TRUE(fixTask.processNextStagedCommand());

    EXPECT_FALSE(sequencer.drainActive());
    ASSERT_TRUE(sequencer.hasPendingSequencedCommand());
    ASSERT_TRUE(sequencer.pendingCommand().has_value());
    EXPECT_EQ(sequencer.pendingCommand()->globalSequenceNumber, domain::CommandSequence{1});
    EXPECT_FALSE(sequencingIngressQueue.empty());

    EXPECT_FALSE(sequencer.drainActive());
    EXPECT_FALSE(sequencer.drainActive());
    ASSERT_TRUE(sequencer.pendingCommand().has_value());
    EXPECT_EQ(sequencer.pendingCommand()->globalSequenceNumber, domain::CommandSequence{1});
    EXPECT_FALSE(sequencingIngressQueue.empty());

    sequencer::sequenceMessage removedBlocker{};
    ASSERT_TRUE(matchingQueue.pop(removedBlocker));
    ASSERT_EQ(removedBlocker.globalSequenceNumber, domain::CommandSequence{999});
    ASSERT_TRUE(sequencer.drainActive());
    ASSERT_TRUE(sequencer.hasPendingSequencedCommand());
    ASSERT_TRUE(sequencer.pendingCommand().has_value());
    EXPECT_EQ(sequencer.pendingCommand()->globalSequenceNumber, domain::CommandSequence{2});
    EXPECT_TRUE(sequencingIngressQueue.empty());

    sequencer::sequenceMessage first{};
    ASSERT_TRUE(matchingQueue.pop(first));
    ASSERT_TRUE(first.clientCommandId.has_value());
    EXPECT_EQ(first.clientCommandId->value(), "FIRST");
    EXPECT_EQ(first.globalSequenceNumber, domain::CommandSequence{1});

    ASSERT_TRUE(sequencer.drainActive());
    EXPECT_FALSE(sequencer.hasPendingSequencedCommand());
    sequencer::sequenceMessage second{};
    ASSERT_TRUE(matchingQueue.pop(second));
    ASSERT_TRUE(second.clientCommandId.has_value());
    EXPECT_EQ(second.clientCommandId->value(), "SECOND");
    EXPECT_EQ(second.globalSequenceNumber, domain::CommandSequence{2});
    EXPECT_FALSE(sequencer.drainActive());
}

TEST(SequencerRunLoopTest, ActiveCycleDrainsAvailableCommandsInFifoOrder) {
    core::SharedQueue<sequencer::sequenceMessage> sequencingIngressQueue(4);
    core::SharedQueue<sequencer::sequenceMessage> matchingQueue(4);
    TestableSequencer sequencer(sequencingIngressQueue, matchingQueue, sequencer::Sequencer::INTERNAL_ADMISSION_BYPASS);

    for (std::uint64_t inputOrder = 1; inputOrder <= 3; ++inputOrder) {
        sequencer::sequenceMessage command{};
        command.type = sequencer::orderType::BUY;
        command.configurationVersion = inputOrder;
        ASSERT_TRUE(sequencingIngressQueue.push(command));
    }

    ASSERT_TRUE(sequencer.drainActive());
    EXPECT_FALSE(sequencer.drainActive());
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
        sequencer::sequenceMessage command{};
        ASSERT_TRUE(matchingQueue.pop(command));
        EXPECT_EQ(command.configurationVersion, sequence);
        EXPECT_EQ(command.globalSequenceNumber, domain::CommandSequence{sequence});
        EXPECT_EQ(command.orderId, domain::OrderId{sequence});
    }
}

TEST(SequencerAdmissionBindingTest, MissingProductionReservationIsAnInvariantFailure) {
    core::SharedQueue<sequencer::sequenceMessage> sequencingIngressQueue(2);
    core::SharedQueue<sequencer::sequenceMessage> matchingQueue(2);
    const FIX::SessionID session("FIX.4.4", "EXCHANGE", "UNRESERVED-CLIENT");
    const core::fix::ClientIdentityResolver resolver({{session, domain::ClientId{9150}}});
    core::admission::CommandAdmissionIndex admissionIndex(2);
    TestableSequencer sequencer(sequencingIngressQueue, matchingQueue, admissionIndex);
    const sequencer::sequenceMessage unreserved =
        core::fix::parseFixMessage(makeFixNewOrder("UNRESERVED"), session, resolver);
    ASSERT_TRUE(sequencingIngressQueue.push(unreserved));

    EXPECT_THROW(static_cast<void>(sequencer.processOnce()), std::logic_error);
    EXPECT_TRUE(matchingQueue.empty());
    EXPECT_FALSE(sequencer.hasPendingSequencedCommand());
}

TEST(CommandAdmissionCompletionIntegrationTest, CompletedFixRetransmissionReturnsOriginalWithoutNewWork) {
    core::SharedQueue<sequencer::sequenceMessage> sequencingIngressQueue(8);
    core::SharedQueue<sequencer::sequenceMessage> matchingQueue(8);
    core::Bus bus(16);
    const FIX::SessionID session("FIX.4.4", "EXCHANGE", "COMPLETION-CLIENT");
    const core::fix::ClientIdentityResolver resolver({{session, domain::ClientId{9200}}});
    core::admission::CommandAdmissionIndex admissionIndex(8);
    core::task::FixTask fixTask(sequencingIngressQueue, bus, resolver, admissionIndex);
    TestableSequencer sequencer(sequencingIngressQueue, matchingQueue, admissionIndex);
    matching_engine::BoundedCommandResultQueue resultQueue(4);
    TestableMatchingEngine matchingEngine(&matchingQueue, bus, resultQueue);
    core::admission::AdmissionCompletionConsumer completionConsumer(resultQueue, admissionIndex);
    const FIX::Message message = makeFixNewOrder("COMPLETE-1");

    fixTask.fromApp(message, session);
    ASSERT_TRUE(fixTask.processNextStagedCommand());
    sequencer::sequenceMessage normalized{};
    ASSERT_TRUE(sequencingIngressQueue.pop(normalized));
    ASSERT_TRUE(sequencingIngressQueue.push(normalized));
    ASSERT_TRUE(sequencer.processOnce());
    matchingEngine.invokeDrain();
    ASSERT_EQ(matchingEngine.activeOrderCount(), 1u);

    matching_engine::ImmutableCommandResultBatch original;
    ASSERT_TRUE(resultQueue.tryPop(original));
    ASSERT_NE(original, nullptr);
    ASSERT_TRUE(resultQueue.tryPush(original));
    ASSERT_TRUE(completionConsumer.processNext());
    EXPECT_EQ(admissionIndex.completedResult(normalized), original);

    fixTask.fromApp(message, session);
    EXPECT_FALSE(fixTask.processNextStagedCommand());
    EXPECT_TRUE(sequencingIngressQueue.empty());
    EXPECT_FALSE(sequencer.processOnce());
    EXPECT_TRUE(matchingQueue.empty());
    matchingEngine.invokeDrain();
    EXPECT_EQ(matchingEngine.activeOrderCount(), 1u);
    EXPECT_TRUE(resultQueue.empty());
    EXPECT_FALSE(completionConsumer.processNext());

    const core::admission::AdmissionStatistics statistics = admissionIndex.statistics();
    EXPECT_EQ(statistics.firstSubmissions, 1u);
    EXPECT_EQ(statistics.identicalCompleted, 1u);
    EXPECT_EQ(admissionIndex.completedResult(normalized), original);
    EXPECT_EQ(original->commandSequence(), domain::CommandSequence{1});
    ASSERT_EQ(original->events().size(), 1u);
    EXPECT_EQ(std::visit([](const auto& event) { return event.eventId.commandSequence; }, original->events().front()),
              domain::CommandSequence{1});

    fixTask.fromApp(makeFixNewOrder("COMPLETE-2"), session);
    ASSERT_TRUE(fixTask.processNextStagedCommand());
    ASSERT_TRUE(sequencer.processOnce());
    sequencer::sequenceMessage next{};
    ASSERT_TRUE(matchingQueue.pop(next));
    EXPECT_EQ(next.globalSequenceNumber, domain::CommandSequence{2});
}

TEST(SequencingIngressTest, AlternatingClientsShareOneProcessLocalSequenceDomainWithCorrectIdentifiers) {
    core::SharedQueue<sequencer::sequenceMessage> sequencingIngressQueue(8);
    core::SharedQueue<sequencer::sequenceMessage> matchingQueue(8);
    core::Bus bus(8);
    const FIX::SessionID firstSession("FIX.4.4", "EXCHANGE", "CLIENT-ONE");
    const FIX::SessionID secondSession("FIX.4.4", "EXCHANGE", "CLIENT-TWO");
    const core::fix::ClientIdentityResolver resolver({
        {firstSession, domain::ClientId{1001}},
        {secondSession, domain::ClientId{2002}},
    });
    core::admission::CommandAdmissionIndex admissionIndex(8);
    core::task::FixTask fixTask(sequencingIngressQueue, bus, resolver, admissionIndex);
    TestableSequencer sequencer(sequencingIngressQueue, matchingQueue, admissionIndex);

    fixTask.fromApp(makeFixNewOrder("ONE-NEW"), firstSession);
    fixTask.fromApp(makeFixNewOrder("TWO-NEW"), secondSession);
    fixTask.fromApp(makeFixCancel("ONE-CANCEL", "1"), firstSession);
    fixTask.fromApp(makeFixCancel("TWO-CANCEL", "2"), secondSession);

    std::vector<sequencer::sequenceMessage> sequenced;
    for (std::size_t index = 0; index < 4; ++index) {
        ASSERT_TRUE(fixTask.processNextStagedCommand());
        ASSERT_TRUE(sequencer.processOnce());
        sequencer::sequenceMessage command{};
        ASSERT_TRUE(matchingQueue.pop(command));
        sequenced.push_back(command);
    }

    EXPECT_EQ(sequenced[0].clientId, domain::ClientId{1001});
    EXPECT_EQ(sequenced[1].clientId, domain::ClientId{2002});
    EXPECT_EQ(sequenced[2].clientId, domain::ClientId{1001});
    EXPECT_EQ(sequenced[3].clientId, domain::ClientId{2002});
    for (std::size_t index = 0; index < sequenced.size(); ++index) {
        EXPECT_EQ(sequenced[index].globalSequenceNumber, domain::CommandSequence{index + 1});
    }
    EXPECT_EQ(sequenced[0].orderId, domain::OrderId{1});
    EXPECT_EQ(sequenced[1].orderId, domain::OrderId{2});
    EXPECT_EQ(sequenced[2].orderId, domain::OrderId{});
    EXPECT_EQ(sequenced[3].orderId, domain::OrderId{});
    EXPECT_EQ(sequenced[2].targetOrderId, std::optional<domain::TargetOrderId>{domain::TargetOrderId{1}});
    EXPECT_EQ(sequenced[3].targetOrderId, std::optional<domain::TargetOrderId>{domain::TargetOrderId{2}});
}

TEST(SequencingIngressTest, ConcurrentFixProducersReceiveUniqueGapFreeSequencesWithinRun) {
    constexpr std::size_t PRODUCER_COUNT = 4;
    constexpr std::size_t COMMANDS_PER_PRODUCER = 32;
    constexpr std::size_t COMMAND_COUNT = PRODUCER_COUNT * COMMANDS_PER_PRODUCER;

    core::SharedQueue<sequencer::sequenceMessage> sequencingIngressQueue(COMMAND_COUNT);
    core::SharedQueue<sequencer::sequenceMessage> matchingQueue(COMMAND_COUNT);
    core::Bus bus(8);
    const std::array<FIX::SessionID, PRODUCER_COUNT> sessions{
        FIX::SessionID("FIX.4.4", "EXCHANGE", "PRODUCER-0"),
        FIX::SessionID("FIX.4.4", "EXCHANGE", "PRODUCER-1"),
        FIX::SessionID("FIX.4.4", "EXCHANGE", "PRODUCER-2"),
        FIX::SessionID("FIX.4.4", "EXCHANGE", "PRODUCER-3"),
    };
    const core::fix::ClientIdentityResolver resolver({
        {sessions[0], domain::ClientId{3000}},
        {sessions[1], domain::ClientId{3001}},
        {sessions[2], domain::ClientId{3002}},
        {sessions[3], domain::ClientId{3003}},
    });
    core::admission::CommandAdmissionIndex admissionIndex(COMMAND_COUNT);
    core::task::FixTask fixTask(sequencingIngressQueue, bus, resolver, admissionIndex);
    TestableSequencer sequencer(sequencingIngressQueue, matchingQueue, admissionIndex);
    std::barrier producerStart(static_cast<std::ptrdiff_t>(PRODUCER_COUNT));
    std::vector<std::thread> producers;

    for (std::size_t producer = 0; producer < PRODUCER_COUNT; ++producer) {
        producers.emplace_back([&, producer]() {
            producerStart.arrive_and_wait();
            for (std::size_t command = 0; command < COMMANDS_PER_PRODUCER; ++command) {
                fixTask.fromApp(makeFixNewOrder("P" + std::to_string(producer) + "-" + std::to_string(command)),
                                sessions[producer]);
            }
        });
    }
    for (std::thread& producer : producers) {
        producer.join();
    }

    ASSERT_EQ(admissionIndex.size(), COMMAND_COUNT);
    for (std::size_t index = 0; index < COMMAND_COUNT; ++index) {
        ASSERT_TRUE(fixTask.processNextStagedCommand());
        ASSERT_TRUE(sequencer.processOnce());
        sequencer::sequenceMessage command{};
        ASSERT_TRUE(matchingQueue.pop(command));
        EXPECT_EQ(command.globalSequenceNumber, domain::CommandSequence{index + 1});
        EXPECT_EQ(command.orderId, domain::OrderId{index + 1});
    }
    EXPECT_FALSE(sequencer.processOnce());
}

TEST(SequencerSequenceExhaustionTest, LastValueIsAssignedAndNextCommandCannotWrapToZero) {
    core::SharedQueue<sequencer::sequenceMessage> sequencingIngressQueue(4);
    core::SharedQueue<sequencer::sequenceMessage> matchingQueue(4);
    core::Bus bus(8);
    const FIX::SessionID session("FIX.4.4", "EXCHANGE", "EXHAUSTION-CLIENT");
    const core::fix::ClientIdentityResolver resolver({{session, domain::ClientId{4004}}});
    core::admission::CommandAdmissionIndex admissionIndex(4);
    core::task::FixTask fixTask(sequencingIngressQueue, bus, resolver, admissionIndex);
    const auto maximum = std::numeric_limits<domain::CommandSequence::Underlying>::max();
    TestableSequencer sequencer(sequencingIngressQueue, matchingQueue, admissionIndex,
                                domain::CommandSequence{maximum - 1});

    fixTask.fromApp(makeFixNewOrder("LAST"), session);
    fixTask.fromApp(makeFixNewOrder("REMAINS-UNSEQUENCED"), session);
    ASSERT_TRUE(fixTask.processNextStagedCommand());
    ASSERT_TRUE(fixTask.processNextStagedCommand());
    EXPECT_THROW(static_cast<void>(sequencer.drainActive()), std::overflow_error);

    sequencer::sequenceMessage last{};
    ASSERT_TRUE(matchingQueue.pop(last));
    EXPECT_EQ(last.globalSequenceNumber, domain::CommandSequence{maximum});
    EXPECT_EQ(last.orderId, domain::OrderId{maximum});

    sequencer::sequenceMessage unsequenced{};
    ASSERT_TRUE(sequencingIngressQueue.pop(unsequenced));
    EXPECT_EQ(unsequenced.globalSequenceNumber, domain::CommandSequence{});
    EXPECT_EQ(unsequenced.orderId, domain::OrderId{});
    EXPECT_EQ(admissionIndex.reserve(unsequenced).status, core::admission::AdmissionStatus::IDENTICAL_IN_FLIGHT);
    EXPECT_TRUE(matchingQueue.empty());
}

TEST(E2EThroughputTest, SequencerToMatchingEngineThroughput) {
    // Test: Measure throughput from Sequencer → MatchingEngine (excluding FIX parsing)
    const int queue_size = 50000; // Must be < 65535 for boost lockfree

    core::SharedQueue<sequencer::sequenceMessage> sequencing_ingress_queue(queue_size);
    core::SharedQueue<sequencer::sequenceMessage> matching_engine_queue(queue_size);
    core::Bus multicast_bus(131072);
    matching_engine::BoundedCommandResultQueue command_result_queue(queue_size);

    // Create pipeline components
    auto sequencer = std::make_unique<TestableSequencer>(sequencing_ingress_queue, matching_engine_queue,
                                                         sequencer::Sequencer::INTERNAL_ADMISSION_BYPASS);
    auto matching_engine =
        std::make_unique<matching_engine::MatchingEngine>(&matching_engine_queue, multicast_bus, command_result_queue);

    std::atomic<bool> stop_sequencer{false};
    std::atomic<bool> stop_matching{false};
    std::atomic<int> messages_sequenced{0};
    std::atomic<int> messages_matched{0};

    // Sequencer thread: drains the single ingress and forwards to matching engine
    std::thread sequencer_thread([&]() {
        while (!stop_sequencer || !sequencing_ingress_queue.empty() || sequencer->hasPendingSequencedCommand()) {
            if (sequencer->processOnce()) {
                messages_sequenced.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    // Matching engine thread: drains sequencer queue
    std::thread matching_thread([&]() {
        while (!stop_matching || !matching_engine_queue.empty()) {
            sequencer::sequenceMessage msg{};
            if (matching_engine_queue.pop(msg)) {
                matching_engine->send(msg);
                messages_matched.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    // Client simulation: spam messages for 1 second
    auto start = std::chrono::high_resolution_clock::now();
    auto end_time = start + std::chrono::seconds(1);
    int client_messages_sent = 0;

    while (std::chrono::high_resolution_clock::now() < end_time) {
        sequencer::sequenceMessage msg{};
        msg.shard_id = static_cast<std::uint8_t>(client_messages_sent % 4);
        msg.price = domain::Price{static_cast<std::uint64_t>((client_messages_sent % 100) + 1)};
        msg.quantity = domain::Quantity{10};
        strcpy(msg.symbol, "SPY");
        msg.type = sequencer::orderType::BUY;

        if (sequencing_ingress_queue.push(msg)) {
            client_messages_sent++;
        } else {
            // Queue full, back off briefly
            std::this_thread::yield();
        }
    }

    auto send_end = std::chrono::high_resolution_clock::now();
    auto send_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(send_end - start).count();

    // Wait for pipeline to drain (up to 500ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop_sequencer = true;
    sequencer_thread.join();

    // Give matching engine time to finish
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop_matching = true;
    matching_thread.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto total_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    int seq_count = messages_sequenced.load();
    int match_count = messages_matched.load();
    double client_throughput = (client_messages_sent * 1000.0) / send_duration_ms;
    double total_throughput = (match_count * 1000.0) / total_duration_ms;

    std::cout << "\n========== E2E Throughput Test Results ==========" << std::endl;
    std::cout << "Client send phase: " << send_duration_ms << " ms" << std::endl;
    std::cout << "  Messages sent by clients: " << client_messages_sent << std::endl;
    std::cout << "  Client throughput: " << client_throughput << " msg/sec" << std::endl;
    std::cout << "\nPipeline processing:" << std::endl;
    std::cout << "  Messages sequenced: " << seq_count << std::endl;
    std::cout << "  Messages matched: " << match_count << std::endl;
    std::cout << "  Total time: " << total_duration_ms << " ms" << std::endl;
    std::cout << "  End-to-end throughput: " << total_throughput << " msg/sec" << std::endl;
    std::cout << "  Avg time per message: " << (total_duration_ms * 1000.0 / match_count) << " us" << std::endl;
    std::cout << "===============================================\n" << std::endl;

    // Sanity checks
    EXPECT_GT(client_messages_sent, 0);
    EXPECT_GT(seq_count, 0);
    EXPECT_GT(match_count, 0);
    EXPECT_EQ(seq_count, match_count);
}

TEST(E2EThroughputTest, FullPipelineWithFixParsing) {
    // Test: Measure throughput from FixTask (FIX parsing) → Sequencer → MatchingEngine
    const int queue_size = 50000;

    core::SharedQueue<sequencer::sequenceMessage> sequencing_ingress_queue(queue_size);
    core::SharedQueue<sequencer::sequenceMessage> matching_engine_queue(queue_size);
    core::Bus multicast_bus(131072);
    matching_engine::BoundedCommandResultQueue command_result_queue(queue_size);

    // Create FixTask
    const FIX::SessionID sessionID("FIX.4.4", "SENDER", "TARGET");
    const core::fix::ClientIdentityResolver clientIdentityResolver({{sessionID, domain::ClientId{600}}});
    core::admission::CommandAdmissionIndex admissionIndex(queue_size);
    core::task::FixTask fix_task(sequencing_ingress_queue, multicast_bus, clientIdentityResolver, admissionIndex);

    // Create sequencer and matching engine
    auto sequencer =
        std::make_unique<TestableSequencer>(sequencing_ingress_queue, matching_engine_queue, admissionIndex);
    auto matching_engine =
        std::make_unique<matching_engine::MatchingEngine>(&matching_engine_queue, multicast_bus, command_result_queue);

    std::atomic<bool> stop_fix{false};
    std::atomic<bool> stop_sequencer{false};
    std::atomic<bool> stop_matching{false};
    std::atomic<int> messages_forwarded{0};
    std::atomic<int> messages_sequenced{0};
    std::atomic<int> messages_matched{0};

    // Single FixTask staging consumer and sequencing-ingress producer.
    std::thread fix_thread([&]() {
        while (!stop_fix || !fix_task.stagingQueueEmpty() || fix_task.hasPendingStagedCommand()) {
            if (fix_task.processNextStagedCommand()) {
                messages_forwarded.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    // Sequencer thread
    std::thread sequencer_thread([&]() {
        while (!stop_sequencer || !sequencing_ingress_queue.empty() || sequencer->hasPendingSequencedCommand()) {
            if (sequencer->processOnce()) {
                messages_sequenced.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    // Matching engine thread
    std::thread matching_thread([&]() {
        while (!stop_matching || !matching_engine_queue.empty()) {
            sequencer::sequenceMessage msg{};
            if (matching_engine_queue.pop(msg)) {
                matching_engine->send(msg);
                messages_matched.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    // Simulate FIX clients sending actual FIX messages for 1 second
    auto start = std::chrono::high_resolution_clock::now();
    auto end_time = start + std::chrono::seconds(1);
    int fix_messages_injected = 0;

    while (std::chrono::high_resolution_clock::now() < end_time) {
        // Create a FIX NewOrderSingle message
        FIX44::NewOrderSingle order;

        // Required fields for order
        order.set(FIX::ClOrdID(std::to_string(fix_messages_injected)));
        order.set(FIX::Symbol("SPY"));
        order.set(FIX::Side(FIX::Side_BUY)); // Buy order
        order.set(FIX::TransactTime());
        order.set(FIX::OrderQty(10.0));
        order.set(FIX::Price((fix_messages_injected % 100) + 1.0));
        order.set(FIX::OrdType(FIX::OrdType_LIMIT));
        order.set(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));

        // Send through FixTask's fromApp() to parse into sequenceMessage
        try {
            fix_task.fromApp(order, sessionID);
            fix_messages_injected++;
        } catch (...) {
            std::this_thread::yield();
        }
    }

    auto send_end = std::chrono::high_resolution_clock::now();
    auto send_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(send_end - start).count();

    // Wait for pipeline to drain
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop_fix = true;
    fix_thread.join();

    stop_sequencer = true;
    sequencer_thread.join();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop_matching = true;
    matching_thread.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto total_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    const std::size_t ingress_count = admissionIndex.size();
    int forwarded_count = messages_forwarded.load();
    int seq_count = messages_sequenced.load();
    int match_count = messages_matched.load();

    std::cout << "\n========== Full Pipeline (FIX + Sequencer + MatchingEngine) ==========" << std::endl;
    std::cout << "FIX message injection: " << send_duration_ms << " ms" << std::endl;
    std::cout << "  Messages injected: " << fix_messages_injected << std::endl;
    std::cout << "  Injection throughput: " << (fix_messages_injected * 1000.0 / send_duration_ms) << " msg/sec"
              << std::endl;
    std::cout << "\nFull pipeline processing:" << std::endl;
    std::cout << "  Commands retained by admission: " << ingress_count << std::endl;
    std::cout << "  Commands forwarded from gateway staging: " << forwarded_count << std::endl;
    std::cout << "  Messages sequenced: " << seq_count << std::endl;
    std::cout << "  Messages matched: " << match_count << std::endl;
    std::cout << "  Total time: " << total_duration_ms << " ms" << std::endl;
    std::cout << "  End-to-end throughput: " << (match_count * 1000.0 / total_duration_ms) << " msg/sec" << std::endl;
    std::cout << "  Avg time per message: " << (total_duration_ms * 1000.0 / match_count) << " us" << std::endl;
    std::cout << "====================================================================\n" << std::endl;

    // Sanity checks
    EXPECT_GT(fix_messages_injected, 0);
    EXPECT_GT(ingress_count, 0u);
    EXPECT_GT(forwarded_count, 0);
    EXPECT_GT(seq_count, 0);
    EXPECT_GT(match_count, 0);
}
