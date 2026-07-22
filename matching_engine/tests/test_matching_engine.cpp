#include <gtest/gtest.h>

#include "../../bus/include/bus.hpp"
#include "../../core/shared_queue/include/shared_queue.hpp"
#include "../../sequencer/include/sequencer.hpp"
#include "../include/matching_engine.hpp"

using namespace exchange;

namespace {
class TestableMatchingEngine : public matching_engine::MatchingEngine {
  public:
    using matching_engine::MatchingEngine::MatchingEngine;
    void invokeDrain() { drainQueue(sequencerQueue, "Sequencer"); }
    void invokeAddOrder(const sequencer::sequenceMessage &message) { addOrder(message); }
    void invokeProcessBuyOrder(sequencer::sequenceMessage &message) { processBuyOrder(message); }
    uint64_t bestSellQuantity(const char *symbol) {
        return sellOrders.at(symbol).begin()->second.orders.front().quantity;
    }
    uint64_t bestSellLevelQuantity(const char *symbol) {
        return sellOrders.at(symbol).begin()->second.totalQuantity;
    }
};

sequencer::sequenceMessage makeOrder(uint64_t id, sequencer::orderType type, const char *symbol,
                                     uint64_t price, uint64_t quantity,
                                     core::task::TimeInForce tif) {
    sequencer::sequenceMessage message{};
    message.id = id;
    message.type = type;
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
    sequencer::sequenceMessage m1{};
    m1.id = 1;
    strcpy(m1.symbol, "ABC");
    sequencer::sequenceMessage m2{};
    m2.id = 2;
    strcpy(m2.symbol, "XYZ");

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

    auto restingSell =
        makeOrder(1, sequencer::orderType::SELL, "ABC", 100, 100, core::task::TimeInForce::DAY);
    engine.invokeAddOrder(restingSell);

    auto fokBuy =
        makeOrder(2, sequencer::orderType::BUY, "ABC", 100, 250, core::task::TimeInForce::FOK);
    engine.invokeProcessBuyOrder(fokBuy);

    EXPECT_EQ(fokBuy.quantity, 250);
    EXPECT_EQ(engine.bestSellQuantity("ABC"), 100);
    EXPECT_EQ(engine.bestSellLevelQuantity("ABC"), 100);
}

TEST(MatchingEngineTest, BuyMatchUpdatesRestingSellLevelQuantity) {
    core::SharedQueue<sequencer::sequenceMessage> seq_q(16);
    core::Bus bus(8);
    TestableMatchingEngine engine(&seq_q, bus);

    auto restingSell =
        makeOrder(1, sequencer::orderType::SELL, "ABC", 100, 100, core::task::TimeInForce::DAY);
    engine.invokeAddOrder(restingSell);

    auto buy =
        makeOrder(2, sequencer::orderType::BUY, "ABC", 100, 40, core::task::TimeInForce::IOC);
    engine.invokeProcessBuyOrder(buy);

    EXPECT_EQ(buy.quantity, 0);
    EXPECT_EQ(engine.bestSellQuantity("ABC"), 60);
    EXPECT_EQ(engine.bestSellLevelQuantity("ABC"), 60);
}
