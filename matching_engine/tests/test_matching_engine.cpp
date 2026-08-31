#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <type_traits>

#include "../../bus/include/bus.hpp"
#include "../../core/shared_queue/include/shared_queue.hpp"
#include "../../sequencer/include/sequencer.hpp"
#include "../include/matching_engine.hpp"

using namespace exchange;

namespace {
class TestableMatchingEngine : public matching_engine::MatchingEngine {
public:
    using Result = matching_engine::MatchingEngine::ProcessingResult;
    using Outcome = matching_engine::MatchingEngine::ProcessingOutcome;
    TestableMatchingEngine(core::SharedQueue<sequencer::sequenceMessage>* sequencerQueue, core::Bus& multicastBus)
        : MatchingEngine(sequencerQueue, multicastBus, 16) {}
    TestableMatchingEngine(core::SharedQueue<sequencer::sequenceMessage>* sequencerQueue, core::Bus& multicastBus,
                           matching_engine::BoundedCommandResultQueue& commandResultQueue)
        : MatchingEngine(sequencerQueue, multicastBus, commandResultQueue) {}
    void invokeDrain() {
        drainQueue(sequencerQueue, "Sequencer");
    }
    bool hasPendingCommandResult() const {
        return pendingCommandResult != nullptr;
    }
    Result invokeAddOrder(const sequencer::sequenceMessage& message) {
        return addOrder(message, message.quantity);
    }
    Outcome invokeProcessBuyOrder(const sequencer::sequenceMessage& message) {
        return processBuyOrder(message);
    }
    Outcome invokeProcessSellOrder(const sequencer::sequenceMessage& message) {
        return processSellOrder(message);
    }
    Outcome invokeProcessCancel(const sequencer::sequenceMessage& message) {
        return processCancel(message);
    }
    Outcome invokeProcessMessage(const sequencer::sequenceMessage& message) {
        return processMessage(message);
    }
    uint64_t bestBuyLevelQuantity(domain::InstrumentId instrumentId) const {
        return orderBooks.at(instrumentId).bids.begin()->second.totalQuantity.value();
    }
    uint64_t bestSellQuantity(domain::InstrumentId instrumentId) const {
        return orderBooks.at(instrumentId).asks.begin()->second.orders.front().remainingQuantity.value();
    }
    uint64_t bestSellLevelQuantity(domain::InstrumentId instrumentId) const {
        return orderBooks.at(instrumentId).asks.begin()->second.totalQuantity.value();
    }
    uint64_t sellLevelQuantity(domain::InstrumentId instrumentId, domain::Price price) const {
        return orderBooks.at(instrumentId).asks.at(price).totalQuantity.value();
    }
    domain::OrderId bestSellOrderId(domain::InstrumentId instrumentId) const {
        return orderBooks.at(instrumentId).asks.begin()->second.orders.front().orderId;
    }
    domain::OrderId bestBuyOrderId(domain::InstrumentId instrumentId) const {
        return orderBooks.at(instrumentId).bids.begin()->second.orders.front().orderId;
    }
    uint64_t buyLevelQuantity(domain::InstrumentId instrumentId, domain::Price price) const {
        return orderBooks.at(instrumentId).bids.at(price).totalQuantity.value();
    }
    std::size_t sellLevelOrderCount(domain::InstrumentId instrumentId, domain::Price price) const {
        return orderBooks.at(instrumentId).asks.at(price).orders.size();
    }
    std::size_t buyLevelOrderCount(domain::InstrumentId instrumentId, domain::Price price) const {
        return orderBooks.at(instrumentId).bids.at(price).orders.size();
    }
    std::size_t activeOrderCount() const {
        return activeOrders.size();
    }
    bool hasBuyLevel(domain::InstrumentId instrumentId, domain::Price price) const {
        const auto instrument = orderBooks.find(instrumentId);
        return instrument != orderBooks.end() && instrument->second.bids.contains(price);
    }
    bool hasSellLevel(domain::InstrumentId instrumentId, domain::Price price) const {
        const auto instrument = orderBooks.find(instrumentId);
        return instrument != orderBooks.end() && instrument->second.asks.contains(price);
    }
    bool hasInstrument(domain::InstrumentId instrumentId) const {
        return orderBooks.contains(instrumentId);
    }
    bool isActive(domain::OrderId orderId) const {
        return activeOrders.contains(orderId);
    }
    uint64_t activeRemainingQuantity(domain::OrderId orderId) const {
        return activeOrders.at(orderId).remainingQuantity.value();
    }
    const OrderNode* activeOrderLocation(domain::OrderId orderId) const {
        return &*activeOrders.at(orderId).orderLocation;
    }
    bool stateIsConsistent() const {
        std::size_t nodeCount = 0;
        for (const auto& [instrumentId, instrumentBook] : orderBooks) {
            const auto checkSide = [&](const auto& sideBook, sequencer::orderType side) {
                for (const auto& [price, priceLevel] : sideBook) {
                    if (priceLevel.orders.empty()) {
                        return false;
                    }
                    uint64_t totalQuantity = 0;
                    for (auto order = priceLevel.orders.cbegin(); order != priceLevel.orders.cend(); ++order) {
                        ++nodeCount;
                        totalQuantity += order->remainingQuantity.value();
                        const auto activeOrder = activeOrders.find(order->orderId);
                        if (activeOrder == activeOrders.end() || activeOrder->second.owner != order->owner ||
                            activeOrder->second.instrumentId != instrumentId || activeOrder->second.side != side ||
                            activeOrder->second.price != price ||
                            activeOrder->second.remainingQuantity != order->remainingQuantity ||
                            &*activeOrder->second.orderLocation != &*order) {
                            return false;
                        }
                    }
                    if (totalQuantity != priceLevel.totalQuantity.value()) {
                        return false;
                    }
                }
                return true;
            };

            if (!checkSide(instrumentBook.bids, sequencer::orderType::BUY) ||
                !checkSide(instrumentBook.asks, sequencer::orderType::SELL)) {
                return false;
            }
        }
        return nodeCount == activeOrders.size();
    }
};

sequencer::sequenceMessage makeOrder(uint64_t id, sequencer::orderType type, const char* symbol, uint64_t price,
                                     uint64_t quantity, core::task::TimeInForce tif,
                                     domain::InstrumentId instrumentId = domain::InstrumentId{1}) {
    sequencer::sequenceMessage message{};
    message.globalSequenceNumber = domain::CommandSequence{id};
    message.orderId = domain::orderIdFrom(message.globalSequenceNumber);
    message.clientId = domain::ClientId{1000 + id};
    message.clientCommandId.emplace("COMMAND-" + std::to_string(id));
    message.type = type;
    message.instrumentId = instrumentId;
    message.configurationVersion = 1;
    message.price = domain::Price{price};
    message.quantity = domain::Quantity{quantity};
    message.tif = tif;
    strcpy(message.symbol, symbol);
    return message;
}

sequencer::sequenceMessage makeCancel(uint64_t commandSequence, uint64_t clientId, uint64_t targetOrderId,
                                      domain::InstrumentId instrumentId = domain::InstrumentId{1}) {
    sequencer::sequenceMessage message{};
    message.globalSequenceNumber = domain::CommandSequence{commandSequence};
    message.clientId = domain::ClientId{clientId};
    message.clientCommandId.emplace("CANCEL-" + std::to_string(commandSequence));
    message.instrumentId = instrumentId;
    message.targetOrderId.emplace(domain::TargetOrderId{targetOrderId});
    message.type = sequencer::orderType::CANCEL;
    return message;
}

static_assert(!std::is_same_v<domain::Price, domain::Quantity>);
static_assert(!std::is_same_v<domain::OrderId, domain::TargetOrderId>);
static_assert(!std::is_convertible_v<domain::TargetOrderId, domain::OrderId>);
static_assert(!std::is_same_v<domain::AdmissionRejectionReason, domain::CommandRejectionReason>);
static_assert(!std::is_convertible_v<domain::AdmissionRejectionReason, domain::CommandRejectionReason>);
static_assert(std::is_same_v<decltype(domain::CommandRejected::reason), const domain::CommandRejectionReason>);
static_assert(!std::is_assignable_v<domain::CommandRejected&, domain::CommandRejected>);
static_assert(!std::is_assignable_v<domain::Trade&, domain::Trade>);
static_assert(!std::is_assignable_v<domain::OrderRested&, domain::OrderRested>);
static_assert(!std::is_assignable_v<domain::OrderCancelled&, domain::OrderCancelled>);

struct EventLimitCase {
    sequencer::orderType takerSide;
    std::size_t plannedEventCount;
};

class MatchingEngineEventLimitTest : public ::testing::TestWithParam<EventLimitCase> {};

std::string eventLimitCaseName(const ::testing::TestParamInfo<EventLimitCase>& info) {
    const char* side = info.param.takerSide == sequencer::orderType::BUY ? "Buy" : "Sell";
    if (info.param.plannedEventCount < 4'096) {
        return std::string{side} + "BelowLimit";
    }
    if (info.param.plannedEventCount == 4'096) {
        return std::string{side} + "ExactlyAtLimit";
    }
    return std::string{side} + "AboveLimit";
}
} // namespace

TEST(DomainTypesTest, ClientCommandIdEnforcesAdoptedBoundsAndExactComparison) {
    const domain::ClientCommandId lower{"abc"};
    const domain::ClientCommandId upper{"ABC"};

    EXPECT_EQ(lower.value(), "abc");
    EXPECT_NE(lower, upper);
    EXPECT_THROW(domain::ClientCommandId{""}, std::invalid_argument);
    EXPECT_THROW(domain::ClientCommandId{std::string(65, 'x')}, std::invalid_argument);
    EXPECT_THROW(domain::ClientCommandId{std::string("x\x80", 2)}, std::invalid_argument);
}

TEST(MatchingEngineTest, ProcessOnceConsumesQueue) {
    // Arrange: create a shared queue and a bus
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);

    // Construct the testable matching engine
    TestableMatchingEngine engine(&seq_q, bus);

    // Push a few messages
    auto m1 = makeOrder(1, sequencer::orderType::BUY, "SPY", 100, 1, core::task::TimeInForce::IOC);
    auto m2 = makeOrder(2, sequencer::orderType::BUY, "SPY", 100, 1, core::task::TimeInForce::IOC);

    EXPECT_TRUE(seq_q.push(m1));
    EXPECT_TRUE(seq_q.push(m2));
    EXPECT_FALSE(seq_q.empty());

    // Act: process once via test subclass
    engine.invokeDrain();

    // Assert: queue should be empty (messages consumed)
    EXPECT_TRUE(seq_q.empty());
}

TEST(MatchingEngineTest, ProcessOnceEmptyDoesNothing) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    // Ensure empty initially
    EXPECT_TRUE(seq_q.empty());

    // Should not throw or block
    engine.invokeDrain();

    EXPECT_TRUE(seq_q.empty());
}

TEST(MatchingEngineTest, MulticastSaturationCountsOneFailureWithoutChangingMatchingResults) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(2);
    core::Bus bus(1);
    std::atomic<core::Bus::cursor_type> stalledCursor{0};
    bus.registerCursor(stalledCursor);
    matching_engine::BoundedCommandResultQueue resultQueue(2);
    TestableMatchingEngine engine(&seq_q, bus, resultQueue);

    const auto first = makeOrder(1, sequencer::orderType::BUY, "SPY", 100, 10, core::task::TimeInForce::GTC);
    const auto second = makeOrder(2, sequencer::orderType::BUY, "SPY", 99, 20, core::task::TimeInForce::GTC);
    ASSERT_TRUE(seq_q.push(first));
    ASSERT_TRUE(seq_q.push(second));

    engine.invokeDrain();

    EXPECT_EQ(engine.multicastWriteFailures(), 1u);
    EXPECT_TRUE(seq_q.empty());
    EXPECT_EQ(engine.activeOrderCount(), 2u);
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{1}), 10u);
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{2}), 20u);
    EXPECT_TRUE(engine.stateIsConsistent());

    matching_engine::ImmutableCommandResultBatch firstResult;
    matching_engine::ImmutableCommandResultBatch secondResult;
    ASSERT_TRUE(resultQueue.tryPop(firstResult));
    ASSERT_TRUE(resultQueue.tryPop(secondResult));
    ASSERT_NE(firstResult, nullptr);
    ASSERT_NE(secondResult, nullptr);
    EXPECT_EQ(firstResult->commandSequence(), domain::CommandSequence{1});
    EXPECT_EQ(secondResult->commandSequence(), domain::CommandSequence{2});
    EXPECT_EQ(firstResult->result(), matching_engine::ProcessingResult::APPLIED);
    EXPECT_EQ(secondResult->result(), matching_engine::ProcessingResult::APPLIED);
    ASSERT_EQ(firstResult->events().size(), 1u);
    ASSERT_EQ(secondResult->events().size(), 1u);
    EXPECT_NE(std::get_if<domain::OrderRested>(&firstResult->events().front()), nullptr);
    EXPECT_NE(std::get_if<domain::OrderRested>(&secondResult->events().front()), nullptr);
}

TEST(MatchingEngineTest, MulticastFailureCounterSupportsConcurrentDiagnosticSnapshots) {
    constexpr std::size_t failedPublications = 1'000;
    core::SharedQueue<sequencer::sequenceMessage> seq_q(1);
    core::Bus bus(1);
    std::atomic<core::Bus::cursor_type> stalledCursor{0};
    bus.registerCursor(stalledCursor);
    TestableMatchingEngine engine(&seq_q, bus);
    auto message = makeOrder(1, sequencer::orderType::BUY, "SPY", 100, 1, core::task::TimeInForce::GTC);
    ASSERT_TRUE(bus.write(message));
    std::atomic<bool> writerDone{false};
    std::thread writer([&]() {
        for (std::size_t publication = 0; publication < failedPublications; ++publication) {
            engine.send(message);
        }
        writerDone.store(true, std::memory_order_release);
    });

    std::uint64_t previous = 0;
    bool monotonic = true;
    while (!writerDone.load(std::memory_order_acquire)) {
        const std::uint64_t current = engine.multicastWriteFailures();
        monotonic = monotonic && current >= previous;
        previous = current;
    }
    writer.join();

    EXPECT_TRUE(monotonic);
    EXPECT_EQ(engine.multicastWriteFailures(), failedPublications);
}

TEST(MatchingEngineEventHandoffTest, DeliversOneCompleteImmutableBatchWithEventsInOrder) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    matching_engine::BoundedCommandResultQueue resultQueue(2);
    TestableMatchingEngine engine(&seq_q, bus, resultQueue);

    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(1, sequencer::orderType::SELL, "SPY", 99, 20, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(2, sequencer::orderType::SELL, "SPY", 99, 30, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    auto incoming = makeOrder(3, sequencer::orderType::BUY, "SPY", 100, 100, core::task::TimeInForce::IOC);
    ASSERT_TRUE(seq_q.push(incoming));

    engine.invokeDrain();

    EXPECT_TRUE(seq_q.empty());
    EXPECT_FALSE(engine.hasPendingCommandResult());
    ASSERT_EQ(resultQueue.size(), 1u);

    matching_engine::ImmutableCommandResultBatch batch;
    ASSERT_TRUE(resultQueue.tryPop(batch));
    ASSERT_NE(batch, nullptr);
    EXPECT_EQ(batch->correlation().clientId, incoming.clientId);
    EXPECT_EQ(batch->correlation().clientCommandId, *incoming.clientCommandId);
    EXPECT_EQ(batch->correlation().commandSequence, incoming.globalSequenceNumber);
    EXPECT_EQ(batch->commandSequence(), domain::CommandSequence{3});
    EXPECT_EQ(batch->result(), matching_engine::ProcessingResult::APPLIED);

    const auto& events = batch->events();
    ASSERT_EQ(events.size(), 3u);
    const auto* firstTrade = std::get_if<domain::Trade>(&events[0]);
    const auto* secondTrade = std::get_if<domain::Trade>(&events[1]);
    const auto* terminal = std::get_if<domain::OrderCancelled>(&events[2]);
    ASSERT_NE(firstTrade, nullptr);
    ASSERT_NE(secondTrade, nullptr);
    ASSERT_NE(terminal, nullptr);
    EXPECT_EQ(firstTrade->eventId, (domain::EventId{domain::CommandSequence{3}, domain::EventIndex{0}}));
    EXPECT_EQ(firstTrade->makerOrderId, domain::OrderId{1});
    EXPECT_EQ(secondTrade->eventId, (domain::EventId{domain::CommandSequence{3}, domain::EventIndex{1}}));
    EXPECT_EQ(secondTrade->makerOrderId, domain::OrderId{2});
    EXPECT_EQ(terminal->eventId, (domain::EventId{domain::CommandSequence{3}, domain::EventIndex{2}}));
    EXPECT_EQ(terminal->reason, domain::CancelReason::IOC_REMAINDER);
    EXPECT_EQ(terminal->cancelledQuantity, domain::Quantity{50});
    EXPECT_TRUE(resultQueue.empty());
    EXPECT_FALSE(resultQueue.tryPop(batch));
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineEventHandoffTest, NewOrderAndCancelBatchesUseExactIncomingCorrelationOnly) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(4);
    core::Bus bus(8);
    matching_engine::BoundedCommandResultQueue resultQueue(2);
    TestableMatchingEngine engine(&seq_q, bus, resultQueue);

    sequencer::sequenceMessage order =
        makeOrder(11, sequencer::orderType::BUY, "LEGACY", 100, 10, core::task::TimeInForce::GTC);
    order.shard_id = 3;
    ASSERT_TRUE(seq_q.push(order));
    engine.invokeDrain();

    matching_engine::ImmutableCommandResultBatch orderResult;
    ASSERT_TRUE(resultQueue.tryPop(orderResult));
    ASSERT_NE(orderResult, nullptr);
    EXPECT_EQ(orderResult->correlation().clientId, order.clientId);
    EXPECT_EQ(orderResult->correlation().clientCommandId, *order.clientCommandId);
    EXPECT_EQ(orderResult->correlation().commandSequence, order.globalSequenceNumber);
    for (const domain::BusinessEvent& event : orderResult->events()) {
        EXPECT_EQ(std::visit([](const auto& typedEvent) { return typedEvent.eventId.commandSequence; }, event),
                  orderResult->commandSequence());
    }

    sequencer::sequenceMessage cancel = makeCancel(12, 7012, 999);
    std::strcpy(cancel.symbol, "OTHER");
    cancel.shard_id = order.shard_id;
    ASSERT_TRUE(seq_q.push(cancel));
    engine.invokeDrain();

    matching_engine::ImmutableCommandResultBatch cancelResult;
    ASSERT_TRUE(resultQueue.tryPop(cancelResult));
    ASSERT_NE(cancelResult, nullptr);
    EXPECT_EQ(cancelResult->correlation().clientId, cancel.clientId);
    EXPECT_EQ(cancelResult->correlation().clientCommandId, *cancel.clientCommandId);
    EXPECT_EQ(cancelResult->correlation().commandSequence, cancel.globalSequenceNumber);
    for (const domain::BusinessEvent& event : cancelResult->events()) {
        EXPECT_EQ(std::visit([](const auto& typedEvent) { return typedEvent.eventId.commandSequence; }, event),
                  cancelResult->commandSequence());
    }
}

TEST(CommandResultBatchTest, RejectsEventSequenceThatDoesNotMatchCorrelation) {
    std::vector<domain::BusinessEvent> events;
    events.emplace_back(domain::OrderCancelled{
        .eventId =
            {
                .commandSequence = domain::CommandSequence{2},
                .eventIndex = domain::EventIndex{0},
            },
        .orderId = domain::OrderId{1},
        .clientId = domain::ClientId{100},
        .instrumentId = domain::InstrumentId{1},
        .cancelledQuantity = domain::Quantity{1},
        .reason = domain::CancelReason::IOC_REMAINDER,
    });

    EXPECT_THROW(matching_engine::CommandResultBatch(
                     domain::CommandResultCorrelation{
                         .clientId = domain::ClientId{100},
                         .clientCommandId = domain::ClientCommandId{"MISMATCH"},
                         .commandSequence = domain::CommandSequence{1},
                     },
                     matching_engine::ProcessingResult::APPLIED, std::move(events)),
                 std::invalid_argument);
}

TEST(MatchingEngineEventHandoffTest, SaturationRetriesPendingBatchBeforeProcessingNextCommandWithoutDuplicates) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    matching_engine::BoundedCommandResultQueue resultQueue(1);
    TestableMatchingEngine engine(&seq_q, bus, resultQueue);

    for (uint64_t id = 1; id <= 4; ++id) {
        auto order = makeOrder(id, sequencer::orderType::BUY, "SPY", 100 - id, 10, core::task::TimeInForce::GTC);
        ASSERT_TRUE(seq_q.push(order));
    }

    engine.invokeDrain();
    ASSERT_EQ(resultQueue.size(), 1u);
    ASSERT_TRUE(engine.hasPendingCommandResult());
    EXPECT_TRUE(engine.isActive(domain::OrderId{1}));
    EXPECT_TRUE(engine.isActive(domain::OrderId{2}));
    EXPECT_FALSE(engine.isActive(domain::OrderId{3}));
    EXPECT_FALSE(engine.isActive(domain::OrderId{4}));
    EXPECT_FALSE(seq_q.empty());

    engine.invokeDrain();
    engine.invokeDrain();
    EXPECT_EQ(resultQueue.size(), 1u);
    EXPECT_TRUE(engine.hasPendingCommandResult());
    EXPECT_FALSE(engine.isActive(domain::OrderId{3}));
    EXPECT_FALSE(engine.isActive(domain::OrderId{4}));

    std::vector<uint64_t> deliveredCommandSequences;
    const auto popBatch = [&]() {
        matching_engine::ImmutableCommandResultBatch batch;
        ASSERT_TRUE(resultQueue.tryPop(batch));
        ASSERT_NE(batch, nullptr);
        ASSERT_EQ(batch->events().size(), 1u);
        EXPECT_NE(std::get_if<domain::OrderRested>(&batch->events().front()), nullptr);
        deliveredCommandSequences.push_back(batch->commandSequence().value());
    };

    popBatch();
    engine.invokeDrain();
    ASSERT_EQ(resultQueue.size(), 1u);
    EXPECT_TRUE(engine.hasPendingCommandResult());
    EXPECT_TRUE(engine.isActive(domain::OrderId{3}));
    EXPECT_FALSE(engine.isActive(domain::OrderId{4}));

    engine.invokeDrain();
    EXPECT_FALSE(engine.isActive(domain::OrderId{4}));
    popBatch();
    engine.invokeDrain();
    ASSERT_EQ(resultQueue.size(), 1u);
    EXPECT_TRUE(engine.hasPendingCommandResult());
    EXPECT_TRUE(engine.isActive(domain::OrderId{4}));

    popBatch();
    engine.invokeDrain();
    EXPECT_FALSE(engine.hasPendingCommandResult());
    ASSERT_EQ(resultQueue.size(), 1u);
    popBatch();

    engine.invokeDrain();
    EXPECT_TRUE(seq_q.empty());
    EXPECT_TRUE(resultQueue.empty());
    EXPECT_FALSE(engine.hasPendingCommandResult());
    matching_engine::ImmutableCommandResultBatch duplicate;
    EXPECT_FALSE(resultQueue.tryPop(duplicate));
    EXPECT_EQ(deliveredCommandSequences, (std::vector<uint64_t>{1, 2, 3, 4}));
    EXPECT_EQ(engine.activeOrderCount(), 4u);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, UnsupportedTimeInForceIsInvariantFailureWithoutMutationForBothSides) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(1, sequencer::orderType::SELL, "SPY", 110, 100, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(2, sequencer::orderType::BUY, "SPY", 90, 100, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);

    constexpr std::array unsupported{core::task::TimeInForce::DAY, core::task::TimeInForce::FOK,
                                     core::task::TimeInForce::GTD, core::task::TimeInForce::GTX,
                                     core::task::TimeInForce::ATC, static_cast<core::task::TimeInForce>('Z')};
    uint64_t orderId = 10;
    for (const core::task::TimeInForce timeInForce : unsupported) {
        auto buy = makeOrder(orderId++, sequencer::orderType::BUY, "SPY", 110, 25, timeInForce);
        auto sell = makeOrder(orderId++, sequencer::orderType::SELL, "SPY", 90, 25, timeInForce);
        buy.expiry = std::chrono::system_clock::time_point{std::chrono::seconds{1}};
        sell.expiry = std::chrono::system_clock::time_point{std::chrono::seconds{1}};
        EXPECT_THROW(engine.invokeProcessMessage(buy), std::logic_error);
        EXPECT_THROW(engine.invokeProcessMessage(sell), std::logic_error);
    }

    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{1}), 100u);
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{2}), 100u);
    EXPECT_EQ(engine.bestSellLevelQuantity(domain::InstrumentId{1}), 100u);
    EXPECT_EQ(engine.bestBuyLevelQuantity(domain::InstrumentId{1}), 100u);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST_P(MatchingEngineEventLimitTest, EnforcesExactResultBoundBeforeStateMutation) {
    constexpr std::size_t MAX_EVENTS_PER_COMMAND = 4'096;
    constexpr uint64_t PRICE = 100;

    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    const EventLimitCase testCase = GetParam();
    const sequencer::orderType makerSide =
        testCase.takerSide == sequencer::orderType::BUY ? sequencer::orderType::SELL : sequencer::orderType::BUY;
    const std::size_t restingOrderCount = testCase.plannedEventCount - 1;
    for (std::size_t index = 0; index < restingOrderCount; ++index) {
        const uint64_t orderId = static_cast<uint64_t>(index + 1);
        ASSERT_EQ(engine.invokeAddOrder(makeOrder(orderId, makerSide, "SPY", PRICE, 1, core::task::TimeInForce::GTC)),
                  TestableMatchingEngine::Result::APPLIED)
            << "resting order " << orderId;
    }

    const auto* firstOrderLocation = engine.activeOrderLocation(domain::OrderId{1});
    const auto* lastOrderLocation =
        engine.activeOrderLocation(domain::OrderId{static_cast<uint64_t>(restingOrderCount)});
    const uint64_t incomingOrderId = static_cast<uint64_t>(restingOrderCount + 1);
    const auto incoming = makeOrder(incomingOrderId, testCase.takerSide, "SPY", PRICE,
                                    static_cast<uint64_t>(testCase.plannedEventCount), core::task::TimeInForce::IOC);
    const TestableMatchingEngine::Outcome outcome = testCase.takerSide == sequencer::orderType::BUY
                                                        ? engine.invokeProcessBuyOrder(incoming)
                                                        : engine.invokeProcessSellOrder(incoming);

    if (testCase.plannedEventCount <= MAX_EVENTS_PER_COMMAND) {
        ASSERT_EQ(outcome.result, TestableMatchingEngine::Result::APPLIED);
        ASSERT_EQ(outcome.events.size(), testCase.plannedEventCount);
        for (std::size_t index = 0; index < restingOrderCount; ++index) {
            const auto* trade = std::get_if<domain::Trade>(&outcome.events[index]);
            ASSERT_NE(trade, nullptr) << "event " << index;
            EXPECT_EQ(trade->eventId, (domain::EventId{domain::CommandSequence{incomingOrderId},
                                                       domain::EventIndex{static_cast<uint32_t>(index)}}));
            EXPECT_EQ(trade->makerOrderId, domain::OrderId{static_cast<uint64_t>(index + 1)});
            EXPECT_EQ(trade->executionQuantity, domain::Quantity{1});
        }

        const auto* terminal = std::get_if<domain::OrderCancelled>(&outcome.events.back());
        ASSERT_NE(terminal, nullptr);
        EXPECT_EQ(
            *terminal,
            (domain::OrderCancelled{
                .eventId = domain::EventId{domain::CommandSequence{incomingOrderId},
                                           domain::EventIndex{static_cast<uint32_t>(testCase.plannedEventCount - 1)}},
                .orderId = domain::OrderId{incomingOrderId},
                .clientId = domain::ClientId{1000 + incomingOrderId},
                .instrumentId = domain::InstrumentId{1},
                .cancelledQuantity = domain::Quantity{1},
                .reason = domain::CancelReason::IOC_REMAINDER,
            }));
        EXPECT_FALSE(engine.hasInstrument(domain::InstrumentId{1}));
        EXPECT_EQ(engine.activeOrderCount(), 0u);
    } else {
        ASSERT_EQ(outcome.result, TestableMatchingEngine::Result::BOOK_CAPACITY_EXCEEDED);
        ASSERT_EQ(outcome.events.size(), 1u);
        const auto* rejection = std::get_if<domain::CommandRejected>(&outcome.events.front());
        ASSERT_NE(rejection, nullptr);
        EXPECT_EQ(*rejection,
                  (domain::CommandRejected{
                      .eventId = domain::EventId{domain::CommandSequence{incomingOrderId}, domain::EventIndex{0}},
                      .commandType = domain::CommandType::NEW_ORDER,
                      .clientId = domain::ClientId{1000 + incomingOrderId},
                      .clientCommandId = domain::ClientCommandId{"COMMAND-" + std::to_string(incomingOrderId)},
                      .relevantOrderId = std::optional<domain::OrderId>{domain::OrderId{incomingOrderId}},
                      .reason = domain::CommandRejectionReason::BOOK_CAPACITY_EXCEEDED,
                  }));
        EXPECT_EQ(engine.activeOrderCount(), restingOrderCount);
        EXPECT_TRUE(engine.isActive(domain::OrderId{1}));
        EXPECT_TRUE(engine.isActive(domain::OrderId{static_cast<uint64_t>(restingOrderCount)}));
        EXPECT_EQ(engine.activeOrderLocation(domain::OrderId{1}), firstOrderLocation);
        EXPECT_EQ(engine.activeOrderLocation(domain::OrderId{static_cast<uint64_t>(restingOrderCount)}),
                  lastOrderLocation);
        if (makerSide == sequencer::orderType::BUY) {
            EXPECT_EQ(engine.buyLevelOrderCount(domain::InstrumentId{1}, domain::Price{PRICE}), restingOrderCount);
            EXPECT_EQ(engine.buyLevelQuantity(domain::InstrumentId{1}, domain::Price{PRICE}), restingOrderCount);
            EXPECT_EQ(engine.bestBuyOrderId(domain::InstrumentId{1}), domain::OrderId{1});
        } else {
            EXPECT_EQ(engine.sellLevelOrderCount(domain::InstrumentId{1}, domain::Price{PRICE}), restingOrderCount);
            EXPECT_EQ(engine.sellLevelQuantity(domain::InstrumentId{1}, domain::Price{PRICE}), restingOrderCount);
            EXPECT_EQ(engine.bestSellOrderId(domain::InstrumentId{1}), domain::OrderId{1});
        }
    }
    EXPECT_EQ(incoming.quantity, domain::Quantity{testCase.plannedEventCount});
    EXPECT_TRUE(engine.stateIsConsistent());
}

INSTANTIATE_TEST_SUITE_P(BuyAndSellBoundaries, MatchingEngineEventLimitTest,
                         ::testing::Values(EventLimitCase{sequencer::orderType::BUY, 4'095},
                                           EventLimitCase{sequencer::orderType::BUY, 4'096},
                                           EventLimitCase{sequencer::orderType::BUY, 4'097},
                                           EventLimitCase{sequencer::orderType::SELL, 4'095},
                                           EventLimitCase{sequencer::orderType::SELL, 4'096},
                                           EventLimitCase{sequencer::orderType::SELL, 4'097}),
                         eventLimitCaseName);

TEST(MatchingEngineTest, PartialFillUpdatesNodeAggregateAndActiveIndex) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    auto restingSell = makeOrder(1, sequencer::orderType::SELL, "SPY", 100, 100, core::task::TimeInForce::GTC);
    engine.invokeAddOrder(restingSell);

    auto buy = makeOrder(2, sequencer::orderType::BUY, "SPY", 100, 40, core::task::TimeInForce::IOC);
    EXPECT_EQ(engine.invokeProcessBuyOrder(buy).result, TestableMatchingEngine::Result::APPLIED);

    EXPECT_EQ(buy.quantity.value(), 40);
    EXPECT_EQ(engine.bestSellQuantity(domain::InstrumentId{1}), 60);
    EXPECT_EQ(engine.bestSellLevelQuantity(domain::InstrumentId{1}), 60);
    EXPECT_TRUE(engine.isActive(domain::OrderId{1}));
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{1}), 60);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, PriceLevelAggregateAcceptsMaximumAndRejectsNextUnit) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    for (uint64_t id = 1; id <= 10; ++id) {
        const auto order =
            makeOrder(id, sequencer::orderType::BUY, "SPY", 100, 100000000, core::task::TimeInForce::GTC);
        EXPECT_EQ(engine.invokeAddOrder(order), TestableMatchingEngine::Result::APPLIED);
    }
    EXPECT_EQ(engine.bestBuyLevelQuantity(domain::InstrumentId{1}), 1000000000u);

    const auto excess = makeOrder(11, sequencer::orderType::BUY, "SPY", 100, 1, core::task::TimeInForce::GTC);
    EXPECT_EQ(engine.invokeAddOrder(excess), TestableMatchingEngine::Result::BOOK_CAPACITY_EXCEEDED);
    EXPECT_EQ(engine.bestBuyLevelQuantity(domain::InstrumentId{1}), 1000000000u);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, AggregatePreflightRejectsCrossingGtcWithoutPartialMutation) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    for (uint64_t id = 1; id <= 9; ++id) {
        const auto order =
            makeOrder(id, sequencer::orderType::BUY, "SPY", 100, 100000000, core::task::TimeInForce::GTC);
        ASSERT_EQ(engine.invokeAddOrder(order), TestableMatchingEngine::Result::APPLIED);
    }
    const auto finalBuy = makeOrder(10, sequencer::orderType::BUY, "SPY", 100, 99999999, core::task::TimeInForce::GTC);
    ASSERT_EQ(engine.invokeAddOrder(finalBuy), TestableMatchingEngine::Result::APPLIED);

    const auto restingSell = makeOrder(11, sequencer::orderType::SELL, "SPY", 100, 1, core::task::TimeInForce::GTC);
    ASSERT_EQ(engine.invokeAddOrder(restingSell), TestableMatchingEngine::Result::APPLIED);

    auto incomingBuy = makeOrder(12, sequencer::orderType::BUY, "SPY", 100, 3, core::task::TimeInForce::GTC);
    const TestableMatchingEngine::Outcome outcome = engine.invokeProcessBuyOrder(incomingBuy);

    ASSERT_EQ(outcome.result, TestableMatchingEngine::Result::BOOK_CAPACITY_EXCEEDED);
    ASSERT_EQ(outcome.events.size(), 1u);
    const auto* rejection = std::get_if<domain::CommandRejected>(&outcome.events.front());
    ASSERT_NE(rejection, nullptr);
    EXPECT_EQ(rejection->eventId.commandSequence, domain::CommandSequence{12});
    EXPECT_EQ(rejection->eventId.eventIndex, domain::EventIndex{0});
    EXPECT_EQ(rejection->commandType, domain::CommandType::NEW_ORDER);
    EXPECT_EQ(rejection->clientId, domain::ClientId{1012});
    EXPECT_EQ(rejection->clientCommandId.value(), "COMMAND-12");
    EXPECT_EQ(rejection->relevantOrderId, std::optional<domain::OrderId>{domain::OrderId{12}});
    EXPECT_EQ(rejection->reason, domain::CommandRejectionReason::BOOK_CAPACITY_EXCEEDED);

    EXPECT_EQ(incomingBuy.quantity.value(), 3u);
    EXPECT_EQ(engine.bestBuyLevelQuantity(domain::InstrumentId{1}), 999999999u);
    EXPECT_EQ(engine.bestSellQuantity(domain::InstrumentId{1}), 1u);
    EXPECT_EQ(engine.bestSellLevelQuantity(domain::InstrumentId{1}), 1u);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, FullFillRemovesOrderLevelInstrumentAndActiveIndexEntry) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    const auto restingSell = makeOrder(1, sequencer::orderType::SELL, "SPY", 100, 100, core::task::TimeInForce::GTC);
    ASSERT_EQ(engine.invokeAddOrder(restingSell), TestableMatchingEngine::Result::APPLIED);

    auto incomingBuy = makeOrder(2, sequencer::orderType::BUY, "SPY", 100, 100, core::task::TimeInForce::IOC);
    ASSERT_EQ(engine.invokeProcessBuyOrder(incomingBuy).result, TestableMatchingEngine::Result::APPLIED);

    EXPECT_EQ(incomingBuy.quantity.value(), 100u);
    EXPECT_FALSE(engine.isActive(domain::OrderId{1}));
    EXPECT_FALSE(engine.hasInstrument(domain::InstrumentId{1}));
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, SamePriceOrdersKeepStableFifoPriority) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    const auto firstSell = makeOrder(1, sequencer::orderType::SELL, "SPY", 100, 100, core::task::TimeInForce::GTC);
    const auto secondSell = makeOrder(2, sequencer::orderType::SELL, "SPY", 100, 100, core::task::TimeInForce::GTC);
    ASSERT_EQ(engine.invokeAddOrder(firstSell), TestableMatchingEngine::Result::APPLIED);
    const auto* firstLocation = engine.activeOrderLocation(domain::OrderId{1});
    ASSERT_EQ(engine.invokeAddOrder(secondSell), TestableMatchingEngine::Result::APPLIED);

    EXPECT_EQ(engine.activeOrderLocation(domain::OrderId{1}), firstLocation);
    EXPECT_EQ(engine.bestSellOrderId(domain::InstrumentId{1}), domain::OrderId{1});

    auto incomingBuy = makeOrder(3, sequencer::orderType::BUY, "SPY", 100, 120, core::task::TimeInForce::IOC);
    ASSERT_EQ(engine.invokeProcessBuyOrder(incomingBuy).result, TestableMatchingEngine::Result::APPLIED);

    EXPECT_EQ(incomingBuy.quantity.value(), 120u);
    EXPECT_FALSE(engine.isActive(domain::OrderId{1}));
    EXPECT_TRUE(engine.isActive(domain::OrderId{2}));
    EXPECT_EQ(engine.bestSellOrderId(domain::InstrumentId{1}), domain::OrderId{2});
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{2}), 80u);
    EXPECT_EQ(engine.bestSellLevelQuantity(domain::InstrumentId{1}), 80u);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, AggregatesEqualRemainingQuantitiesAcrossLevelsAfterFills) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(1, sequencer::orderType::SELL, "SPY", 100, 30, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(2, sequencer::orderType::SELL, "SPY", 100, 40, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(3, sequencer::orderType::SELL, "SPY", 101, 50, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);

    auto incomingBuy = makeOrder(4, sequencer::orderType::BUY, "SPY", 101, 45, core::task::TimeInForce::IOC);
    ASSERT_EQ(engine.invokeProcessBuyOrder(incomingBuy).result, TestableMatchingEngine::Result::APPLIED);

    EXPECT_FALSE(engine.isActive(domain::OrderId{1}));
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{2}), 25u);
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{3}), 50u);
    EXPECT_EQ(engine.sellLevelQuantity(domain::InstrumentId{1}, domain::Price{100}), 25u);
    EXPECT_EQ(engine.sellLevelQuantity(domain::InstrumentId{1}, domain::Price{101}), 50u);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, InstrumentIdIsAuthoritativeForMatchingAndSymbolsDoNotCrossBooks) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    const auto restingSell = makeOrder(1, sequencer::orderType::SELL, "LEGACY", 100, 50, core::task::TimeInForce::GTC,
                                       domain::InstrumentId{1});
    ASSERT_EQ(engine.invokeAddOrder(restingSell), TestableMatchingEngine::Result::APPLIED);

    auto otherInstrumentBuy = makeOrder(2, sequencer::orderType::BUY, "LEGACY", 100, 50, core::task::TimeInForce::IOC,
                                        domain::InstrumentId{2});
    const auto isolatedOutcome = engine.invokeProcessBuyOrder(otherInstrumentBuy);
    ASSERT_EQ(isolatedOutcome.result, TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(isolatedOutcome.events.size(), 1u);
    EXPECT_NE(std::get_if<domain::OrderCancelled>(&isolatedOutcome.events.front()), nullptr);
    EXPECT_EQ(otherInstrumentBuy.quantity.value(), 50u);
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{1}), 50u);

    auto sameInstrumentBuy = makeOrder(3, sequencer::orderType::BUY, "RENAMED", 100, 50, core::task::TimeInForce::IOC,
                                       domain::InstrumentId{1});
    ASSERT_EQ(engine.invokeProcessBuyOrder(sameInstrumentBuy).result, TestableMatchingEngine::Result::APPLIED);
    EXPECT_EQ(sameInstrumentBuy.quantity.value(), 50u);
    EXPECT_FALSE(engine.isActive(domain::OrderId{1}));
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, DuplicateActiveAuthoritativeOrderIdIsInvariantFailure) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    const auto first = makeOrder(1, sequencer::orderType::SELL, "SPY", 100, 50, core::task::TimeInForce::GTC);
    const auto duplicate = makeOrder(1, sequencer::orderType::BUY, "SPY", 101, 25, core::task::TimeInForce::GTC);
    ASSERT_EQ(engine.invokeAddOrder(first), TestableMatchingEngine::Result::APPLIED);

    EXPECT_THROW(engine.invokeAddOrder(duplicate), std::logic_error);
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{1}), 50u);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, BuyMatchesBestAskThenFifoAndEmitsExactMakerPriceTrades) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(1, sequencer::orderType::SELL, "SPY", 101, 30, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(2, sequencer::orderType::SELL, "SPY", 100, 40, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(3, sequencer::orderType::SELL, "SPY", 100, 50, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(4, sequencer::orderType::SELL, "SPY", 102, 25, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);

    const auto incomingBuy = makeOrder(10, sequencer::orderType::BUY, "SPY", 101, 100, core::task::TimeInForce::IOC);
    const TestableMatchingEngine::Outcome outcome = engine.invokeProcessBuyOrder(incomingBuy);

    ASSERT_EQ(outcome.result, TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(outcome.events.size(), 3u);
    const auto* firstTrade = std::get_if<domain::Trade>(&outcome.events[0]);
    const auto* secondTrade = std::get_if<domain::Trade>(&outcome.events[1]);
    const auto* thirdTrade = std::get_if<domain::Trade>(&outcome.events[2]);
    ASSERT_NE(firstTrade, nullptr);
    ASSERT_NE(secondTrade, nullptr);
    ASSERT_NE(thirdTrade, nullptr);

    EXPECT_EQ(*firstTrade, (domain::Trade{
                               .eventId = domain::EventId{domain::CommandSequence{10}, domain::EventIndex{0}},
                               .instrumentId = domain::InstrumentId{1},
                               .makerOrderId = domain::OrderId{2},
                               .makerClientId = domain::ClientId{1002},
                               .takerOrderId = domain::OrderId{10},
                               .takerClientId = domain::ClientId{1010},
                               .takerSide = domain::Side::BUY,
                               .executionPrice = domain::Price{100},
                               .executionQuantity = domain::Quantity{40},
                               .makerRemainingQuantity = domain::Quantity{0},
                               .takerRemainingQuantity = domain::Quantity{60},
                           }));
    EXPECT_EQ(*secondTrade, (domain::Trade{
                                .eventId = domain::EventId{domain::CommandSequence{10}, domain::EventIndex{1}},
                                .instrumentId = domain::InstrumentId{1},
                                .makerOrderId = domain::OrderId{3},
                                .makerClientId = domain::ClientId{1003},
                                .takerOrderId = domain::OrderId{10},
                                .takerClientId = domain::ClientId{1010},
                                .takerSide = domain::Side::BUY,
                                .executionPrice = domain::Price{100},
                                .executionQuantity = domain::Quantity{50},
                                .makerRemainingQuantity = domain::Quantity{0},
                                .takerRemainingQuantity = domain::Quantity{10},
                            }));
    EXPECT_EQ(*thirdTrade, (domain::Trade{
                               .eventId = domain::EventId{domain::CommandSequence{10}, domain::EventIndex{2}},
                               .instrumentId = domain::InstrumentId{1},
                               .makerOrderId = domain::OrderId{1},
                               .makerClientId = domain::ClientId{1001},
                               .takerOrderId = domain::OrderId{10},
                               .takerClientId = domain::ClientId{1010},
                               .takerSide = domain::Side::BUY,
                               .executionPrice = domain::Price{101},
                               .executionQuantity = domain::Quantity{10},
                               .makerRemainingQuantity = domain::Quantity{20},
                               .takerRemainingQuantity = domain::Quantity{0},
                           }));

    EXPECT_EQ(incomingBuy.quantity.value(), 100u);
    EXPECT_FALSE(engine.isActive(domain::OrderId{10}));
    EXPECT_FALSE(engine.isActive(domain::OrderId{2}));
    EXPECT_FALSE(engine.isActive(domain::OrderId{3}));
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{1}), 20u);
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{4}), 25u);
    EXPECT_EQ(engine.sellLevelQuantity(domain::InstrumentId{1}, domain::Price{101}), 20u);
    EXPECT_EQ(engine.sellLevelQuantity(domain::InstrumentId{1}, domain::Price{102}), 25u);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, SellMatchesBestBidThenFifoAndEmitsExactMakerPriceTrades) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(1, sequencer::orderType::BUY, "SPY", 99, 30, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(2, sequencer::orderType::BUY, "SPY", 101, 40, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(3, sequencer::orderType::BUY, "SPY", 101, 50, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(4, sequencer::orderType::BUY, "SPY", 98, 25, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);

    const auto incomingSell = makeOrder(10, sequencer::orderType::SELL, "SPY", 99, 100, core::task::TimeInForce::IOC);
    const TestableMatchingEngine::Outcome outcome = engine.invokeProcessSellOrder(incomingSell);

    ASSERT_EQ(outcome.result, TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(outcome.events.size(), 3u);
    const auto* firstTrade = std::get_if<domain::Trade>(&outcome.events[0]);
    const auto* secondTrade = std::get_if<domain::Trade>(&outcome.events[1]);
    const auto* thirdTrade = std::get_if<domain::Trade>(&outcome.events[2]);
    ASSERT_NE(firstTrade, nullptr);
    ASSERT_NE(secondTrade, nullptr);
    ASSERT_NE(thirdTrade, nullptr);

    EXPECT_EQ(*firstTrade, (domain::Trade{
                               .eventId = domain::EventId{domain::CommandSequence{10}, domain::EventIndex{0}},
                               .instrumentId = domain::InstrumentId{1},
                               .makerOrderId = domain::OrderId{2},
                               .makerClientId = domain::ClientId{1002},
                               .takerOrderId = domain::OrderId{10},
                               .takerClientId = domain::ClientId{1010},
                               .takerSide = domain::Side::SELL,
                               .executionPrice = domain::Price{101},
                               .executionQuantity = domain::Quantity{40},
                               .makerRemainingQuantity = domain::Quantity{0},
                               .takerRemainingQuantity = domain::Quantity{60},
                           }));
    EXPECT_EQ(*secondTrade, (domain::Trade{
                                .eventId = domain::EventId{domain::CommandSequence{10}, domain::EventIndex{1}},
                                .instrumentId = domain::InstrumentId{1},
                                .makerOrderId = domain::OrderId{3},
                                .makerClientId = domain::ClientId{1003},
                                .takerOrderId = domain::OrderId{10},
                                .takerClientId = domain::ClientId{1010},
                                .takerSide = domain::Side::SELL,
                                .executionPrice = domain::Price{101},
                                .executionQuantity = domain::Quantity{50},
                                .makerRemainingQuantity = domain::Quantity{0},
                                .takerRemainingQuantity = domain::Quantity{10},
                            }));
    EXPECT_EQ(*thirdTrade, (domain::Trade{
                               .eventId = domain::EventId{domain::CommandSequence{10}, domain::EventIndex{2}},
                               .instrumentId = domain::InstrumentId{1},
                               .makerOrderId = domain::OrderId{1},
                               .makerClientId = domain::ClientId{1001},
                               .takerOrderId = domain::OrderId{10},
                               .takerClientId = domain::ClientId{1010},
                               .takerSide = domain::Side::SELL,
                               .executionPrice = domain::Price{99},
                               .executionQuantity = domain::Quantity{10},
                               .makerRemainingQuantity = domain::Quantity{20},
                               .takerRemainingQuantity = domain::Quantity{0},
                           }));

    EXPECT_EQ(incomingSell.quantity.value(), 100u);
    EXPECT_FALSE(engine.isActive(domain::OrderId{10}));
    EXPECT_FALSE(engine.isActive(domain::OrderId{2}));
    EXPECT_FALSE(engine.isActive(domain::OrderId{3}));
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{1}), 20u);
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{4}), 25u);
    EXPECT_EQ(engine.buyLevelQuantity(domain::InstrumentId{1}, domain::Price{99}), 20u);
    EXPECT_EQ(engine.buyLevelQuantity(domain::InstrumentId{1}, domain::Price{98}), 25u);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, SellGtcRemainderRestsWithPostTradeQuantity) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(1, sequencer::orderType::BUY, "SPY", 101, 40, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    const auto incomingSell = makeOrder(2, sequencer::orderType::SELL, "SPY", 100, 100, core::task::TimeInForce::GTC);

    const TestableMatchingEngine::Outcome outcome = engine.invokeProcessSellOrder(incomingSell);

    ASSERT_EQ(outcome.result, TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(outcome.events.size(), 2u);
    const auto* trade = std::get_if<domain::Trade>(&outcome.events.front());
    const auto* rested = std::get_if<domain::OrderRested>(&outcome.events[1]);
    ASSERT_NE(trade, nullptr);
    ASSERT_NE(rested, nullptr);
    EXPECT_EQ(trade->executionPrice, domain::Price{101});
    EXPECT_EQ(trade->executionQuantity, domain::Quantity{40});
    EXPECT_EQ(trade->makerRemainingQuantity, domain::Quantity{0});
    EXPECT_EQ(trade->takerRemainingQuantity, domain::Quantity{60});
    EXPECT_EQ(*rested, (domain::OrderRested{
                           .eventId = domain::EventId{domain::CommandSequence{2}, domain::EventIndex{1}},
                           .orderId = domain::OrderId{2},
                           .clientId = domain::ClientId{1002},
                           .instrumentId = domain::InstrumentId{1},
                           .side = domain::Side::SELL,
                           .price = domain::Price{100},
                           .remainingQuantity = domain::Quantity{60},
                       }));
    EXPECT_EQ(incomingSell.quantity.value(), 100u);
    EXPECT_FALSE(engine.isActive(domain::OrderId{1}));
    EXPECT_TRUE(engine.isActive(domain::OrderId{2}));
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{2}), 60u);
    EXPECT_EQ(engine.sellLevelQuantity(domain::InstrumentId{1}, domain::Price{100}), 60u);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, BuyGtcRemainderRestsWithPostTradeQuantity) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(1, sequencer::orderType::SELL, "SPY", 99, 40, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    const auto incomingBuy = makeOrder(2, sequencer::orderType::BUY, "SPY", 100, 100, core::task::TimeInForce::GTC);

    const TestableMatchingEngine::Outcome outcome = engine.invokeProcessBuyOrder(incomingBuy);

    ASSERT_EQ(outcome.result, TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(outcome.events.size(), 2u);
    const auto* trade = std::get_if<domain::Trade>(&outcome.events.front());
    const auto* rested = std::get_if<domain::OrderRested>(&outcome.events[1]);
    ASSERT_NE(trade, nullptr);
    ASSERT_NE(rested, nullptr);
    EXPECT_EQ(trade->executionPrice, domain::Price{99});
    EXPECT_EQ(trade->executionQuantity, domain::Quantity{40});
    EXPECT_EQ(trade->makerRemainingQuantity, domain::Quantity{0});
    EXPECT_EQ(trade->takerRemainingQuantity, domain::Quantity{60});
    EXPECT_EQ(*rested, (domain::OrderRested{
                           .eventId = domain::EventId{domain::CommandSequence{2}, domain::EventIndex{1}},
                           .orderId = domain::OrderId{2},
                           .clientId = domain::ClientId{1002},
                           .instrumentId = domain::InstrumentId{1},
                           .side = domain::Side::BUY,
                           .price = domain::Price{100},
                           .remainingQuantity = domain::Quantity{60},
                       }));
    EXPECT_EQ(incomingBuy.quantity.value(), 100u);
    EXPECT_FALSE(engine.isActive(domain::OrderId{1}));
    EXPECT_TRUE(engine.isActive(domain::OrderId{2}));
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{2}), 60u);
    EXPECT_EQ(engine.buyLevelQuantity(domain::InstrumentId{1}, domain::Price{100}), 60u);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, SelfTradingIsAllowedAndIocRemainderDoesNotRest) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    const auto restingBuy = makeOrder(1, sequencer::orderType::BUY, "SPY", 100, 50, core::task::TimeInForce::GTC);
    ASSERT_EQ(engine.invokeAddOrder(restingBuy), TestableMatchingEngine::Result::APPLIED);
    auto incomingSell = makeOrder(2, sequencer::orderType::SELL, "SPY", 100, 70, core::task::TimeInForce::IOC);
    incomingSell.clientId = restingBuy.clientId;

    const TestableMatchingEngine::Outcome outcome = engine.invokeProcessSellOrder(incomingSell);

    ASSERT_EQ(outcome.result, TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(outcome.events.size(), 2u);
    const auto* trade = std::get_if<domain::Trade>(&outcome.events.front());
    const auto* cancelled = std::get_if<domain::OrderCancelled>(&outcome.events[1]);
    ASSERT_NE(trade, nullptr);
    ASSERT_NE(cancelled, nullptr);
    EXPECT_EQ(trade->makerClientId, restingBuy.clientId);
    EXPECT_EQ(trade->takerClientId, restingBuy.clientId);
    EXPECT_EQ(trade->executionQuantity, domain::Quantity{50});
    EXPECT_EQ(trade->takerRemainingQuantity, domain::Quantity{20});
    EXPECT_EQ(*cancelled, (domain::OrderCancelled{
                              .eventId = domain::EventId{domain::CommandSequence{2}, domain::EventIndex{1}},
                              .orderId = domain::OrderId{2},
                              .clientId = restingBuy.clientId,
                              .instrumentId = domain::InstrumentId{1},
                              .cancelledQuantity = domain::Quantity{20},
                              .reason = domain::CancelReason::IOC_REMAINDER,
                          }));
    EXPECT_FALSE(engine.isActive(domain::OrderId{1}));
    EXPECT_FALSE(engine.isActive(domain::OrderId{2}));
    EXPECT_FALSE(engine.hasInstrument(domain::InstrumentId{1}));
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, SellMatchingIsIsolatedByInstrumentIdAndIgnoresLegacySymbolText) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    const auto restingBuy = makeOrder(1, sequencer::orderType::BUY, "LEGACY", 100, 50, core::task::TimeInForce::GTC,
                                      domain::InstrumentId{1});
    ASSERT_EQ(engine.invokeAddOrder(restingBuy), TestableMatchingEngine::Result::APPLIED);

    const auto otherInstrumentSell = makeOrder(2, sequencer::orderType::SELL, "LEGACY", 100, 50,
                                               core::task::TimeInForce::IOC, domain::InstrumentId{2});
    const TestableMatchingEngine::Outcome isolatedOutcome = engine.invokeProcessSellOrder(otherInstrumentSell);
    ASSERT_EQ(isolatedOutcome.events.size(), 1u);
    EXPECT_NE(std::get_if<domain::OrderCancelled>(&isolatedOutcome.events.front()), nullptr);
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{1}), 50u);

    const auto sameInstrumentSell = makeOrder(3, sequencer::orderType::SELL, "RENAMED", 100, 50,
                                              core::task::TimeInForce::IOC, domain::InstrumentId{1});
    const TestableMatchingEngine::Outcome matchingOutcome = engine.invokeProcessSellOrder(sameInstrumentSell);
    ASSERT_EQ(matchingOutcome.events.size(), 1u);
    EXPECT_NE(std::get_if<domain::Trade>(&matchingOutcome.events.front()), nullptr);
    EXPECT_FALSE(engine.isActive(domain::OrderId{1}));
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, UnmatchedGtcOrdersRestAndEmitExactTerminalEventsForBothSides) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    const auto buy = makeOrder(1, sequencer::orderType::BUY, "SPY", 99, 25, core::task::TimeInForce::GTC);
    const TestableMatchingEngine::Outcome buyOutcome = engine.invokeProcessBuyOrder(buy);
    ASSERT_EQ(buyOutcome.result, TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(buyOutcome.events.size(), 1u);
    const auto* buyRested = std::get_if<domain::OrderRested>(&buyOutcome.events.front());
    ASSERT_NE(buyRested, nullptr);
    EXPECT_EQ(*buyRested, (domain::OrderRested{
                              .eventId = domain::EventId{domain::CommandSequence{1}, domain::EventIndex{0}},
                              .orderId = domain::OrderId{1},
                              .clientId = domain::ClientId{1001},
                              .instrumentId = domain::InstrumentId{1},
                              .side = domain::Side::BUY,
                              .price = domain::Price{99},
                              .remainingQuantity = domain::Quantity{25},
                          }));

    const auto sell = makeOrder(2, sequencer::orderType::SELL, "SPY", 101, 30, core::task::TimeInForce::GTC);
    const TestableMatchingEngine::Outcome sellOutcome = engine.invokeProcessSellOrder(sell);
    ASSERT_EQ(sellOutcome.result, TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(sellOutcome.events.size(), 1u);
    const auto* sellRested = std::get_if<domain::OrderRested>(&sellOutcome.events.front());
    ASSERT_NE(sellRested, nullptr);
    EXPECT_EQ(*sellRested, (domain::OrderRested{
                               .eventId = domain::EventId{domain::CommandSequence{2}, domain::EventIndex{0}},
                               .orderId = domain::OrderId{2},
                               .clientId = domain::ClientId{1002},
                               .instrumentId = domain::InstrumentId{1},
                               .side = domain::Side::SELL,
                               .price = domain::Price{101},
                               .remainingQuantity = domain::Quantity{30},
                           }));

    EXPECT_EQ(buy.quantity, domain::Quantity{25});
    EXPECT_EQ(sell.quantity, domain::Quantity{30});
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{1}), 25u);
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{2}), 30u);
    EXPECT_EQ(engine.buyLevelQuantity(domain::InstrumentId{1}, domain::Price{99}), 25u);
    EXPECT_EQ(engine.sellLevelQuantity(domain::InstrumentId{1}, domain::Price{101}), 30u);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, UnmatchedIocOrdersCancelExactlyAndNeverRestForBothSides) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    const auto buy = makeOrder(1, sequencer::orderType::BUY, "SPY", 99, 25, core::task::TimeInForce::IOC);
    const TestableMatchingEngine::Outcome buyOutcome = engine.invokeProcessBuyOrder(buy);
    ASSERT_EQ(buyOutcome.result, TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(buyOutcome.events.size(), 1u);
    const auto* buyCancelled = std::get_if<domain::OrderCancelled>(&buyOutcome.events.front());
    ASSERT_NE(buyCancelled, nullptr);
    EXPECT_EQ(*buyCancelled, (domain::OrderCancelled{
                                 .eventId = domain::EventId{domain::CommandSequence{1}, domain::EventIndex{0}},
                                 .orderId = domain::OrderId{1},
                                 .clientId = domain::ClientId{1001},
                                 .instrumentId = domain::InstrumentId{1},
                                 .cancelledQuantity = domain::Quantity{25},
                                 .reason = domain::CancelReason::IOC_REMAINDER,
                             }));

    const auto sell = makeOrder(2, sequencer::orderType::SELL, "SPY", 101, 30, core::task::TimeInForce::IOC);
    const TestableMatchingEngine::Outcome sellOutcome = engine.invokeProcessSellOrder(sell);
    ASSERT_EQ(sellOutcome.result, TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(sellOutcome.events.size(), 1u);
    const auto* sellCancelled = std::get_if<domain::OrderCancelled>(&sellOutcome.events.front());
    ASSERT_NE(sellCancelled, nullptr);
    EXPECT_EQ(*sellCancelled, (domain::OrderCancelled{
                                  .eventId = domain::EventId{domain::CommandSequence{2}, domain::EventIndex{0}},
                                  .orderId = domain::OrderId{2},
                                  .clientId = domain::ClientId{1002},
                                  .instrumentId = domain::InstrumentId{1},
                                  .cancelledQuantity = domain::Quantity{30},
                                  .reason = domain::CancelReason::IOC_REMAINDER,
                              }));

    EXPECT_EQ(buy.quantity, domain::Quantity{25});
    EXPECT_EQ(sell.quantity, domain::Quantity{30});
    EXPECT_FALSE(engine.isActive(domain::OrderId{1}));
    EXPECT_FALSE(engine.isActive(domain::OrderId{2}));
    EXPECT_FALSE(engine.hasInstrument(domain::InstrumentId{1}));
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, BuyIocPartialFillEmitsTradeThenExactCancellation) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(1, sequencer::orderType::SELL, "SPY", 99, 20, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(2, sequencer::orderType::SELL, "SPY", 99, 30, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    const auto incomingBuy = makeOrder(3, sequencer::orderType::BUY, "SPY", 100, 100, core::task::TimeInForce::IOC);
    const TestableMatchingEngine::Outcome outcome = engine.invokeProcessBuyOrder(incomingBuy);

    ASSERT_EQ(outcome.result, TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(outcome.events.size(), 3u);
    const auto* firstTrade = std::get_if<domain::Trade>(&outcome.events[0]);
    const auto* secondTrade = std::get_if<domain::Trade>(&outcome.events[1]);
    const auto* cancelled = std::get_if<domain::OrderCancelled>(&outcome.events[2]);
    ASSERT_NE(firstTrade, nullptr);
    ASSERT_NE(secondTrade, nullptr);
    ASSERT_NE(cancelled, nullptr);
    EXPECT_EQ(firstTrade->eventId, (domain::EventId{domain::CommandSequence{3}, domain::EventIndex{0}}));
    EXPECT_EQ(firstTrade->executionQuantity, domain::Quantity{20});
    EXPECT_EQ(firstTrade->takerRemainingQuantity, domain::Quantity{80});
    EXPECT_EQ(secondTrade->eventId, (domain::EventId{domain::CommandSequence{3}, domain::EventIndex{1}}));
    EXPECT_EQ(secondTrade->executionQuantity, domain::Quantity{30});
    EXPECT_EQ(secondTrade->takerRemainingQuantity, domain::Quantity{50});
    EXPECT_EQ(*cancelled, (domain::OrderCancelled{
                              .eventId = domain::EventId{domain::CommandSequence{3}, domain::EventIndex{2}},
                              .orderId = domain::OrderId{3},
                              .clientId = domain::ClientId{1003},
                              .instrumentId = domain::InstrumentId{1},
                              .cancelledQuantity = domain::Quantity{50},
                              .reason = domain::CancelReason::IOC_REMAINDER,
                          }));
    EXPECT_EQ(incomingBuy.quantity, domain::Quantity{100});
    EXPECT_FALSE(engine.isActive(domain::OrderId{1}));
    EXPECT_FALSE(engine.isActive(domain::OrderId{2}));
    EXPECT_FALSE(engine.isActive(domain::OrderId{3}));
    EXPECT_FALSE(engine.hasInstrument(domain::InstrumentId{1}));
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, FullyFilledGtcOrdersEmitOnlyTradesForBothSides) {
    {
        core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
        core::Bus bus(8);
        TestableMatchingEngine engine(&seq_q, bus);
        ASSERT_EQ(engine.invokeAddOrder(
                      makeOrder(1, sequencer::orderType::SELL, "SPY", 99, 40, core::task::TimeInForce::GTC)),
                  TestableMatchingEngine::Result::APPLIED);
        const auto buy = makeOrder(2, sequencer::orderType::BUY, "SPY", 100, 40, core::task::TimeInForce::GTC);
        const auto outcome = engine.invokeProcessBuyOrder(buy);
        ASSERT_EQ(outcome.events.size(), 1u);
        EXPECT_NE(std::get_if<domain::Trade>(&outcome.events.front()), nullptr);
        EXPECT_EQ(buy.quantity, domain::Quantity{40});
        EXPECT_FALSE(engine.isActive(domain::OrderId{1}));
        EXPECT_FALSE(engine.isActive(domain::OrderId{2}));
        EXPECT_TRUE(engine.stateIsConsistent());
    }
    {
        core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
        core::Bus bus(8);
        TestableMatchingEngine engine(&seq_q, bus);
        ASSERT_EQ(engine.invokeAddOrder(
                      makeOrder(1, sequencer::orderType::BUY, "SPY", 101, 40, core::task::TimeInForce::GTC)),
                  TestableMatchingEngine::Result::APPLIED);
        const auto sell = makeOrder(2, sequencer::orderType::SELL, "SPY", 100, 40, core::task::TimeInForce::GTC);
        const auto outcome = engine.invokeProcessSellOrder(sell);
        ASSERT_EQ(outcome.events.size(), 1u);
        EXPECT_NE(std::get_if<domain::Trade>(&outcome.events.front()), nullptr);
        EXPECT_EQ(sell.quantity, domain::Quantity{40});
        EXPECT_FALSE(engine.isActive(domain::OrderId{1}));
        EXPECT_FALSE(engine.isActive(domain::OrderId{2}));
        EXPECT_TRUE(engine.stateIsConsistent());
    }
}

TEST(MatchingEngineTest, SellCapacityPreflightRejectsBeforeCrossingBidMutation) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    for (uint64_t id = 1; id <= 9; ++id) {
        const auto order =
            makeOrder(id, sequencer::orderType::SELL, "SPY", 100, 100000000, core::task::TimeInForce::GTC);
        ASSERT_EQ(engine.invokeAddOrder(order), TestableMatchingEngine::Result::APPLIED);
    }
    ASSERT_EQ(engine.invokeAddOrder(
                  makeOrder(10, sequencer::orderType::SELL, "SPY", 100, 99999999, core::task::TimeInForce::GTC)),
              TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(11, sequencer::orderType::BUY, "SPY", 101, 1, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);

    const auto incomingSell = makeOrder(12, sequencer::orderType::SELL, "SPY", 100, 3, core::task::TimeInForce::GTC);
    const TestableMatchingEngine::Outcome outcome = engine.invokeProcessSellOrder(incomingSell);

    ASSERT_EQ(outcome.result, TestableMatchingEngine::Result::BOOK_CAPACITY_EXCEEDED);
    ASSERT_EQ(outcome.events.size(), 1u);
    const auto* rejection = std::get_if<domain::CommandRejected>(&outcome.events.front());
    ASSERT_NE(rejection, nullptr);
    EXPECT_EQ(rejection->eventId, (domain::EventId{domain::CommandSequence{12}, domain::EventIndex{0}}));
    EXPECT_EQ(rejection->reason, domain::CommandRejectionReason::BOOK_CAPACITY_EXCEEDED);
    EXPECT_EQ(incomingSell.quantity.value(), 3u);
    EXPECT_EQ(engine.bestSellLevelQuantity(domain::InstrumentId{1}), 999999999u);
    EXPECT_EQ(engine.bestBuyOrderId(domain::InstrumentId{1}), domain::OrderId{11});
    EXPECT_EQ(engine.bestBuyLevelQuantity(domain::InstrumentId{1}), 1u);
    EXPECT_TRUE(engine.isActive(domain::OrderId{11}));
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineCancellationTest, SuccessfulBuyAndSellCancellationEmitExactEventsAndCleanEmptyState) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(1, sequencer::orderType::BUY, "SPY", 99, 40, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(2, sequencer::orderType::SELL, "SPY", 101, 60, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);

    const auto cancelBuy = makeCancel(3, 1001, 1);
    const auto buyOutcome = engine.invokeProcessCancel(cancelBuy);
    ASSERT_EQ(buyOutcome.result, TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(buyOutcome.events.size(), 1u);
    const auto* buyCancelled = std::get_if<domain::OrderCancelled>(&buyOutcome.events.front());
    ASSERT_NE(buyCancelled, nullptr);
    EXPECT_EQ(*buyCancelled, (domain::OrderCancelled{
                                 .eventId = domain::EventId{domain::CommandSequence{3}, domain::EventIndex{0}},
                                 .orderId = domain::OrderId{1},
                                 .clientId = domain::ClientId{1001},
                                 .instrumentId = domain::InstrumentId{1},
                                 .cancelledQuantity = domain::Quantity{40},
                                 .reason = domain::CancelReason::CLIENT_REQUESTED,
                             }));
    EXPECT_EQ(cancelBuy.targetOrderId, std::optional<domain::TargetOrderId>{domain::TargetOrderId{1}});
    EXPECT_FALSE(engine.isActive(domain::OrderId{1}));
    EXPECT_FALSE(engine.hasBuyLevel(domain::InstrumentId{1}, domain::Price{99}));
    EXPECT_TRUE(engine.hasInstrument(domain::InstrumentId{1}));

    const auto cancelSell = makeCancel(4, 1002, 2);
    const auto sellOutcome = engine.invokeProcessCancel(cancelSell);
    ASSERT_EQ(sellOutcome.result, TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(sellOutcome.events.size(), 1u);
    const auto* sellCancelled = std::get_if<domain::OrderCancelled>(&sellOutcome.events.front());
    ASSERT_NE(sellCancelled, nullptr);
    EXPECT_EQ(*sellCancelled, (domain::OrderCancelled{
                                  .eventId = domain::EventId{domain::CommandSequence{4}, domain::EventIndex{0}},
                                  .orderId = domain::OrderId{2},
                                  .clientId = domain::ClientId{1002},
                                  .instrumentId = domain::InstrumentId{1},
                                  .cancelledQuantity = domain::Quantity{60},
                                  .reason = domain::CancelReason::CLIENT_REQUESTED,
                              }));
    EXPECT_FALSE(engine.isActive(domain::OrderId{2}));
    EXPECT_FALSE(engine.hasInstrument(domain::InstrumentId{1}));
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineCancellationTest, CancellingMiddleSellPreservesUnaffectedFifoAndAggregate) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(1, sequencer::orderType::SELL, "SPY", 100, 40, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(2, sequencer::orderType::SELL, "SPY", 100, 50, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(3, sequencer::orderType::SELL, "SPY", 100, 30, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    const auto* firstLocation = engine.activeOrderLocation(domain::OrderId{1});
    const auto* thirdLocation = engine.activeOrderLocation(domain::OrderId{3});

    const auto outcome = engine.invokeProcessCancel(makeCancel(4, 1002, 2));
    ASSERT_EQ(outcome.events.size(), 1u);
    const auto* cancelled = std::get_if<domain::OrderCancelled>(&outcome.events.front());
    ASSERT_NE(cancelled, nullptr);
    EXPECT_EQ(cancelled->cancelledQuantity, domain::Quantity{50});
    EXPECT_EQ(engine.sellLevelOrderCount(domain::InstrumentId{1}, domain::Price{100}), 2u);
    EXPECT_EQ(engine.sellLevelQuantity(domain::InstrumentId{1}, domain::Price{100}), 70u);
    EXPECT_EQ(engine.activeOrderLocation(domain::OrderId{1}), firstLocation);
    EXPECT_EQ(engine.activeOrderLocation(domain::OrderId{3}), thirdLocation);
    EXPECT_EQ(engine.bestSellOrderId(domain::InstrumentId{1}), domain::OrderId{1});

    const auto buy = makeOrder(10, sequencer::orderType::BUY, "SPY", 100, 45, core::task::TimeInForce::IOC);
    const auto buyOutcome = engine.invokeProcessBuyOrder(buy);
    ASSERT_EQ(buyOutcome.events.size(), 2u);
    const auto* firstTrade = std::get_if<domain::Trade>(&buyOutcome.events[0]);
    const auto* secondTrade = std::get_if<domain::Trade>(&buyOutcome.events[1]);
    ASSERT_NE(firstTrade, nullptr);
    ASSERT_NE(secondTrade, nullptr);
    EXPECT_EQ(firstTrade->makerOrderId, domain::OrderId{1});
    EXPECT_EQ(secondTrade->makerOrderId, domain::OrderId{3});
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{3}), 25u);
    EXPECT_EQ(engine.sellLevelQuantity(domain::InstrumentId{1}, domain::Price{100}), 25u);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineCancellationTest, PartialFillCancellationUsesExactCurrentRemainder) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(1, sequencer::orderType::BUY, "SPY", 100, 100, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    const auto sell = makeOrder(2, sequencer::orderType::SELL, "SPY", 100, 40, core::task::TimeInForce::IOC);
    ASSERT_EQ(engine.invokeProcessSellOrder(sell).events.size(), 1u);
    ASSERT_EQ(engine.activeRemainingQuantity(domain::OrderId{1}), 60u);

    const auto cancel = makeCancel(3, 1001, 1);
    const auto outcome = engine.invokeProcessCancel(cancel);
    ASSERT_EQ(outcome.events.size(), 1u);
    const auto* cancelled = std::get_if<domain::OrderCancelled>(&outcome.events.front());
    ASSERT_NE(cancelled, nullptr);
    EXPECT_EQ(*cancelled, (domain::OrderCancelled{
                              .eventId = domain::EventId{domain::CommandSequence{3}, domain::EventIndex{0}},
                              .orderId = domain::OrderId{1},
                              .clientId = domain::ClientId{1001},
                              .instrumentId = domain::InstrumentId{1},
                              .cancelledQuantity = domain::Quantity{60},
                              .reason = domain::CancelReason::CLIENT_REQUESTED,
                          }));
    EXPECT_EQ(cancel.globalSequenceNumber, domain::CommandSequence{3});
    EXPECT_EQ(cancel.clientId, domain::ClientId{1001});
    EXPECT_EQ(cancel.clientCommandId->value(), "CANCEL-3");
    EXPECT_FALSE(engine.isActive(domain::OrderId{1}));
    EXPECT_FALSE(engine.hasInstrument(domain::InstrumentId{1}));
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineCancellationTest, MissingAndNotOwnerRejectionsAreExactAndLeaveBookUnchanged) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(1, sequencer::orderType::BUY, "SPY", 100, 40, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(2, sequencer::orderType::BUY, "SPY", 100, 50, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    const auto* firstLocation = engine.activeOrderLocation(domain::OrderId{1});
    const auto* secondLocation = engine.activeOrderLocation(domain::OrderId{2});

    const auto missingOutcome = engine.invokeProcessCancel(makeCancel(3, 1001, 99));
    ASSERT_EQ(missingOutcome.events.size(), 1u);
    const auto* missing = std::get_if<domain::CommandRejected>(&missingOutcome.events.front());
    ASSERT_NE(missing, nullptr);
    EXPECT_EQ(*missing, (domain::CommandRejected{
                            .eventId = domain::EventId{domain::CommandSequence{3}, domain::EventIndex{0}},
                            .commandType = domain::CommandType::CANCEL,
                            .clientId = domain::ClientId{1001},
                            .clientCommandId = domain::ClientCommandId{"CANCEL-3"},
                            .relevantOrderId = std::optional<domain::OrderId>{domain::OrderId{99}},
                            .reason = domain::CommandRejectionReason::ORDER_NOT_ACTIVE,
                        }));

    const auto notOwnerOutcome = engine.invokeProcessCancel(makeCancel(4, 9999, 1));
    ASSERT_EQ(notOwnerOutcome.events.size(), 1u);
    const auto* notOwner = std::get_if<domain::CommandRejected>(&notOwnerOutcome.events.front());
    ASSERT_NE(notOwner, nullptr);
    EXPECT_EQ(*notOwner, (domain::CommandRejected{
                             .eventId = domain::EventId{domain::CommandSequence{4}, domain::EventIndex{0}},
                             .commandType = domain::CommandType::CANCEL,
                             .clientId = domain::ClientId{9999},
                             .clientCommandId = domain::ClientCommandId{"CANCEL-4"},
                             .relevantOrderId = std::optional<domain::OrderId>{domain::OrderId{1}},
                             .reason = domain::CommandRejectionReason::NOT_OWNER,
                         }));

    EXPECT_EQ(engine.buyLevelQuantity(domain::InstrumentId{1}, domain::Price{100}), 90u);
    EXPECT_EQ(engine.bestBuyOrderId(domain::InstrumentId{1}), domain::OrderId{1});
    EXPECT_EQ(engine.activeOrderLocation(domain::OrderId{1}), firstLocation);
    EXPECT_EQ(engine.activeOrderLocation(domain::OrderId{2}), secondLocation);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineCancellationTest, TerminalTargetsReturnOrderNotActive) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(1, sequencer::orderType::SELL, "SPY", 100, 25, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    ASSERT_EQ(engine
                  .invokeProcessBuyOrder(
                      makeOrder(2, sequencer::orderType::BUY, "SPY", 100, 25, core::task::TimeInForce::IOC))
                  .events.size(),
              1u);

    const auto ioc = makeOrder(3, sequencer::orderType::BUY, "SPY", 90, 10, core::task::TimeInForce::IOC);
    const auto iocOutcome = engine.invokeProcessBuyOrder(ioc);
    ASSERT_NE(std::get_if<domain::OrderCancelled>(&iocOutcome.events.front()), nullptr);

    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(4, sequencer::orderType::BUY, "SPY", 90, 15, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);
    const auto initialCancelOutcome = engine.invokeProcessCancel(makeCancel(5, 1004, 4));
    ASSERT_NE(std::get_if<domain::OrderCancelled>(&initialCancelOutcome.events.front()), nullptr);

    for (const auto cancel : {makeCancel(6, 1001, 1), makeCancel(7, 1003, 3), makeCancel(8, 1004, 4)}) {
        const auto outcome = engine.invokeProcessCancel(cancel);
        ASSERT_EQ(outcome.events.size(), 1u);
        const auto* rejected = std::get_if<domain::CommandRejected>(&outcome.events.front());
        ASSERT_NE(rejected, nullptr);
        EXPECT_EQ(rejected->reason, domain::CommandRejectionReason::ORDER_NOT_ACTIVE);
        EXPECT_EQ(rejected->relevantOrderId,
                  std::optional<domain::OrderId>{domain::OrderId{cancel.targetOrderId->value()}});
    }
    EXPECT_FALSE(engine.hasInstrument(domain::InstrumentId{1}));
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineCancellationTest, CapacityRejectedTargetIsNotActive) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    for (uint64_t id = 1; id <= 10; ++id) {
        ASSERT_EQ(engine.invokeAddOrder(
                      makeOrder(id, sequencer::orderType::BUY, "SPY", 100, 100000000, core::task::TimeInForce::GTC)),
                  TestableMatchingEngine::Result::APPLIED);
    }
    const auto rejectedOrder = makeOrder(11, sequencer::orderType::BUY, "SPY", 100, 1, core::task::TimeInForce::GTC);
    const auto rejectedOutcome = engine.invokeProcessBuyOrder(rejectedOrder);
    ASSERT_EQ(rejectedOutcome.result, TestableMatchingEngine::Result::BOOK_CAPACITY_EXCEEDED);

    const auto cancelOutcome = engine.invokeProcessCancel(makeCancel(12, 1011, 11));
    ASSERT_EQ(cancelOutcome.events.size(), 1u);
    const auto* rejected = std::get_if<domain::CommandRejected>(&cancelOutcome.events.front());
    ASSERT_NE(rejected, nullptr);
    EXPECT_EQ(rejected->reason, domain::CommandRejectionReason::ORDER_NOT_ACTIVE);
    EXPECT_EQ(engine.buyLevelQuantity(domain::InstrumentId{1}, domain::Price{100}), 1000000000u);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineCancellationTest, MissingNormalizedFieldsAndInvalidKindsAreInvariantFailures) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);
    ASSERT_EQ(
        engine.invokeAddOrder(makeOrder(1, sequencer::orderType::BUY, "SPY", 100, 40, core::task::TimeInForce::GTC)),
        TestableMatchingEngine::Result::APPLIED);

    const auto valid = makeCancel(2, 1001, 1);
    auto missingSequence = valid;
    missingSequence.globalSequenceNumber = domain::CommandSequence{};
    auto missingClient = valid;
    missingClient.clientId = domain::ClientId{};
    auto missingClientCommand = valid;
    missingClientCommand.clientCommandId.reset();
    auto missingInstrument = valid;
    missingInstrument.instrumentId = domain::InstrumentId{};
    auto missingTarget = valid;
    missingTarget.targetOrderId.reset();
    auto duplicateIdentity = valid;
    duplicateIdentity.orderId = domain::OrderId{2};

    EXPECT_THROW(engine.invokeProcessCancel(missingSequence), std::logic_error);
    EXPECT_THROW(engine.invokeProcessCancel(missingClient), std::logic_error);
    EXPECT_THROW(engine.invokeProcessCancel(missingClientCommand), std::logic_error);
    EXPECT_THROW(engine.invokeProcessCancel(missingInstrument), std::logic_error);
    EXPECT_THROW(engine.invokeProcessCancel(missingTarget), std::logic_error);
    EXPECT_THROW(engine.invokeProcessCancel(duplicateIdentity), std::logic_error);

    auto wrongInstrument = valid;
    wrongInstrument.instrumentId = domain::InstrumentId{2};
    EXPECT_THROW(engine.invokeProcessCancel(wrongInstrument), std::logic_error);

    auto invalidKind = valid;
    invalidKind.type = sequencer::orderType::CANCELREJ;
    EXPECT_THROW(engine.invokeProcessMessage(invalidKind), std::logic_error);

    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{1}), 40u);
    EXPECT_EQ(engine.buyLevelQuantity(domain::InstrumentId{1}, domain::Price{100}), 40u);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineCancellationTest, FillVersusCancelIsDeterminedByProcessingOrder) {
    {
        core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
        core::Bus bus(8);
        TestableMatchingEngine engine(&seq_q, bus);
        ASSERT_EQ(engine.invokeAddOrder(
                      makeOrder(1, sequencer::orderType::SELL, "SPY", 100, 50, core::task::TimeInForce::GTC)),
                  TestableMatchingEngine::Result::APPLIED);

        const auto cancelOutcome = engine.invokeProcessCancel(makeCancel(2, 1001, 1));
        ASSERT_NE(std::get_if<domain::OrderCancelled>(&cancelOutcome.events.front()), nullptr);
        const auto buyOutcome = engine.invokeProcessBuyOrder(
            makeOrder(3, sequencer::orderType::BUY, "SPY", 100, 50, core::task::TimeInForce::IOC));
        ASSERT_EQ(buyOutcome.events.size(), 1u);
        const auto* iocTerminal = std::get_if<domain::OrderCancelled>(&buyOutcome.events.front());
        ASSERT_NE(iocTerminal, nullptr);
        EXPECT_EQ(iocTerminal->reason, domain::CancelReason::IOC_REMAINDER);
        EXPECT_TRUE(engine.stateIsConsistent());
    }
    {
        core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
        core::Bus bus(8);
        TestableMatchingEngine engine(&seq_q, bus);
        ASSERT_EQ(engine.invokeAddOrder(
                      makeOrder(1, sequencer::orderType::SELL, "SPY", 100, 50, core::task::TimeInForce::GTC)),
                  TestableMatchingEngine::Result::APPLIED);

        const auto buyOutcome = engine.invokeProcessBuyOrder(
            makeOrder(2, sequencer::orderType::BUY, "SPY", 100, 50, core::task::TimeInForce::IOC));
        ASSERT_EQ(buyOutcome.events.size(), 1u);
        EXPECT_NE(std::get_if<domain::Trade>(&buyOutcome.events.front()), nullptr);
        const auto cancelOutcome = engine.invokeProcessCancel(makeCancel(3, 1001, 1));
        ASSERT_EQ(cancelOutcome.events.size(), 1u);
        const auto* rejected = std::get_if<domain::CommandRejected>(&cancelOutcome.events.front());
        ASSERT_NE(rejected, nullptr);
        EXPECT_EQ(rejected->reason, domain::CommandRejectionReason::ORDER_NOT_ACTIVE);
        EXPECT_TRUE(engine.stateIsConsistent());
    }
}
