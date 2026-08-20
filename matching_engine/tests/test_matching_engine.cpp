#include <gtest/gtest.h>

#include <string>
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
    using matching_engine::MatchingEngine::MatchingEngine;
    void invokeDrain() {
        drainQueue(sequencerQueue, "Sequencer");
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
    message.id = id;
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

static_assert(!std::is_same_v<domain::Price, domain::Quantity>);
static_assert(!std::is_same_v<domain::AdmissionRejectionReason, domain::CommandRejectionReason>);
static_assert(!std::is_convertible_v<domain::AdmissionRejectionReason, domain::CommandRejectionReason>);
static_assert(std::is_same_v<decltype(domain::CommandRejected::reason), const domain::CommandRejectionReason>);
static_assert(!std::is_assignable_v<domain::CommandRejected&, domain::CommandRejected>);
static_assert(!std::is_assignable_v<domain::Trade&, domain::Trade>);
static_assert(!std::is_assignable_v<domain::OrderRested&, domain::OrderRested>);
static_assert(!std::is_assignable_v<domain::OrderCancelled&, domain::OrderCancelled>);
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

TEST(MatchingEngineTest, FokBuyDoesNotReuseSameRestingOrderInAvailabilityCheck) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    auto restingSell = makeOrder(1, sequencer::orderType::SELL, "SPY", 100, 100, core::task::TimeInForce::DAY);
    engine.invokeAddOrder(restingSell);

    auto fokBuy = makeOrder(2, sequencer::orderType::BUY, "SPY", 100, 250, core::task::TimeInForce::FOK);
    EXPECT_EQ(engine.invokeProcessBuyOrder(fokBuy).result, TestableMatchingEngine::Result::APPLIED);

    EXPECT_EQ(fokBuy.quantity.value(), 250);
    EXPECT_EQ(engine.bestSellQuantity(domain::InstrumentId{1}), 100);
    EXPECT_EQ(engine.bestSellLevelQuantity(domain::InstrumentId{1}), 100);
    EXPECT_TRUE(engine.stateIsConsistent());
}

TEST(MatchingEngineTest, PartialFillUpdatesNodeAggregateAndActiveIndex) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    auto restingSell = makeOrder(1, sequencer::orderType::SELL, "SPY", 100, 100, core::task::TimeInForce::DAY);
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
    ASSERT_EQ(engine.invokeProcessBuyOrder(otherInstrumentBuy).result, TestableMatchingEngine::Result::APPLIED);
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
    ASSERT_EQ(outcome.events.size(), 1u);
    const auto* trade = std::get_if<domain::Trade>(&outcome.events.front());
    ASSERT_NE(trade, nullptr);
    EXPECT_EQ(trade->executionPrice, domain::Price{101});
    EXPECT_EQ(trade->executionQuantity, domain::Quantity{40});
    EXPECT_EQ(trade->makerRemainingQuantity, domain::Quantity{0});
    EXPECT_EQ(trade->takerRemainingQuantity, domain::Quantity{60});
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
    ASSERT_EQ(outcome.events.size(), 1u);
    const auto* trade = std::get_if<domain::Trade>(&outcome.events.front());
    ASSERT_NE(trade, nullptr);
    EXPECT_EQ(trade->executionPrice, domain::Price{99});
    EXPECT_EQ(trade->executionQuantity, domain::Quantity{40});
    EXPECT_EQ(trade->makerRemainingQuantity, domain::Quantity{0});
    EXPECT_EQ(trade->takerRemainingQuantity, domain::Quantity{60});
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
    ASSERT_EQ(outcome.events.size(), 1u);
    const auto* trade = std::get_if<domain::Trade>(&outcome.events.front());
    ASSERT_NE(trade, nullptr);
    EXPECT_EQ(trade->makerClientId, restingBuy.clientId);
    EXPECT_EQ(trade->takerClientId, restingBuy.clientId);
    EXPECT_EQ(trade->executionQuantity, domain::Quantity{50});
    EXPECT_EQ(trade->takerRemainingQuantity, domain::Quantity{20});
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
    EXPECT_TRUE(isolatedOutcome.events.empty());
    EXPECT_EQ(engine.activeRemainingQuantity(domain::OrderId{1}), 50u);

    const auto sameInstrumentSell = makeOrder(3, sequencer::orderType::SELL, "RENAMED", 100, 50,
                                              core::task::TimeInForce::IOC, domain::InstrumentId{1});
    const TestableMatchingEngine::Outcome matchingOutcome = engine.invokeProcessSellOrder(sameInstrumentSell);
    ASSERT_EQ(matchingOutcome.events.size(), 1u);
    EXPECT_NE(std::get_if<domain::Trade>(&matchingOutcome.events.front()), nullptr);
    EXPECT_FALSE(engine.isActive(domain::OrderId{1}));
    EXPECT_TRUE(engine.stateIsConsistent());
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
