#include <gtest/gtest.h>

#include "../../bus/include/bus.hpp"
#include "../../core/shared_queue/include/shared_queue.hpp"
#include "../../sequencer/include/sequencer.hpp"
#include "../include/matching_engine.hpp"

using namespace exchange;

namespace {
class TestableMatchingEngine : public matching_engine::MatchingEngine {
public:
    using Result = matching_engine::MatchingEngine::ProcessingResult;
    using matching_engine::MatchingEngine::MatchingEngine;
    void invokeDrain() {
        drainQueue(sequencerQueue, "Sequencer");
    }
    Result invokeAddOrder(const sequencer::sequenceMessage& message) {
        return addOrder(message);
    }
    Result invokeProcessBuyOrder(sequencer::sequenceMessage& message) {
        return processBuyOrder(message);
    }
    uint64_t bestBuyLevelQuantity(const char* symbol) {
        return buyOrders.at(symbol).begin()->second.totalQuantity;
    }
    uint64_t bestSellQuantity(const char* symbol) {
        return sellOrders.at(symbol).begin()->second.orders.front().quantity;
    }
    uint64_t bestSellLevelQuantity(const char* symbol) {
        return sellOrders.at(symbol).begin()->second.totalQuantity;
    }
};

sequencer::sequenceMessage makeOrder(uint64_t id, sequencer::orderType type, const char* symbol, uint64_t price,
                                     uint64_t quantity, core::task::TimeInForce tif) {
    sequencer::sequenceMessage message{};
    message.id = id;
    message.type = type;
    message.instrumentId = 1;
    message.configurationVersion = 1;
    message.price = price;
    message.quantity = quantity;
    message.tif = tif;
    strcpy(message.symbol, symbol);
    return message;
}
} // namespace

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
    engine.invokeProcessBuyOrder(fokBuy);

    EXPECT_EQ(fokBuy.quantity, 250);
    EXPECT_EQ(engine.bestSellQuantity("SPY"), 100);
    EXPECT_EQ(engine.bestSellLevelQuantity("SPY"), 100);
}

TEST(MatchingEngineTest, BuyMatchUpdatesRestingSellLevelQuantity) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    auto restingSell = makeOrder(1, sequencer::orderType::SELL, "SPY", 100, 100, core::task::TimeInForce::DAY);
    engine.invokeAddOrder(restingSell);

    auto buy = makeOrder(2, sequencer::orderType::BUY, "SPY", 100, 40, core::task::TimeInForce::IOC);
    engine.invokeProcessBuyOrder(buy);

    EXPECT_EQ(buy.quantity, 0);
    EXPECT_EQ(engine.bestSellQuantity("SPY"), 60);
    EXPECT_EQ(engine.bestSellLevelQuantity("SPY"), 60);
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
    EXPECT_EQ(engine.bestBuyLevelQuantity("SPY"), 1000000000u);

    const auto excess = makeOrder(11, sequencer::orderType::BUY, "SPY", 100, 1, core::task::TimeInForce::GTC);
    EXPECT_EQ(engine.invokeAddOrder(excess), TestableMatchingEngine::Result::BOOK_CAPACITY_EXCEEDED);
    EXPECT_EQ(engine.bestBuyLevelQuantity("SPY"), 1000000000u);
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
    EXPECT_EQ(engine.invokeProcessBuyOrder(incomingBuy), TestableMatchingEngine::Result::BOOK_CAPACITY_EXCEEDED);
    EXPECT_EQ(incomingBuy.quantity, 3u);
    EXPECT_EQ(engine.bestBuyLevelQuantity("SPY"), 999999999u);
    EXPECT_EQ(engine.bestSellQuantity("SPY"), 1u);
    EXPECT_EQ(engine.bestSellLevelQuantity("SPY"), 1u);
}
