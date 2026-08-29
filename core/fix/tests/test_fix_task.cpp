#include "fix_parser.hpp"
#include "fix_task.hpp"
#include "fix_test_identities.hpp"
#include <gtest/gtest.h>

using namespace exchange::core::task;
using namespace exchange::sequencer;

namespace {

FIX::Message makeNewOrder(const std::string& clientCommandId) {
    FIX::Message message;
    message.getHeader().setField(FIX::MsgType("D"));
    message.setField(FIX::ClOrdID(clientCommandId));
    message.setField(FIX::Symbol("SPY"));
    message.setField(FIX::Side(FIX::Side_BUY));
    message.setField(FIX::OrderQty(5));
    message.setField(FIX::Price(1.23));
    message.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    message.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));
    return message;
}

} // namespace

TEST(FixTaskTest, FromAppPushesToQueue) {
    // Prepare sequencer queue and multicast bus
    exchange::core::SharedQueue<sequenceMessage> seq_q(16);
    std::vector<exchange::core::SharedQueue<sequenceMessage>*> sequencer_queues;
    sequencer_queues.push_back(&seq_q);

    exchange::core::Bus bus(8);
    exchange::core::admission::CommandAdmissionIndex admissionIndex(8);

    FixTask fix_task(sequencer_queues, bus, exchange::core::fix::test::clientIdentityResolver(), admissionIndex);

    // Build a simple NewOrderSingle
    FIX::Message msg = makeNewOrder("ORD1");

    FIX::SessionID sid("FIX.4.4", "SENDER", "TARGET");

    // Call fromApp which should parse and push into internal queue
    fix_task.fromApp(msg, sid);

    sequenceMessage out{};
    bool popped = fix_task.getFixMessageQueue()->pop(out);
    EXPECT_TRUE(popped);
    EXPECT_EQ(out.quantity.value(), 5);
    EXPECT_EQ(out.type, orderType::BUY);
    EXPECT_STREQ(out.symbol, "SPY");
}

TEST(FixTaskTest, FullShardQueueRetainsOnePendingCommandAndPreventsOvertaking) {
    exchange::core::SharedQueue<sequenceMessage> shardQueue(1);
    std::vector<exchange::core::SharedQueue<sequenceMessage>*> shardQueues{&shardQueue};
    exchange::core::Bus bus(8);
    exchange::core::admission::CommandAdmissionIndex admissionIndex(8);
    FixTask fixTask(shardQueues, bus, exchange::core::fix::test::clientIdentityResolver(), admissionIndex);
    const FIX::SessionID session("FIX.4.4", "SENDER", "TARGET");

    sequenceMessage blocker{};
    blocker.id = 999;
    ASSERT_TRUE(shardQueue.push(blocker));
    fixTask.fromApp(makeNewOrder("FIRST"), session);
    fixTask.fromApp(makeNewOrder("SECOND"), session);

    EXPECT_FALSE(fixTask.processNextNormalizedCommand());
    ASSERT_TRUE(fixTask.hasPendingNormalizedCommand());
    EXPECT_FALSE(fixTask.processNextNormalizedCommand());
    EXPECT_EQ(admissionIndex.statistics().firstSubmissions, 2u);

    sequenceMessage removedBlocker{};
    ASSERT_TRUE(shardQueue.pop(removedBlocker));
    ASSERT_EQ(removedBlocker.id, 999u);
    ASSERT_TRUE(fixTask.processNextNormalizedCommand());
    EXPECT_FALSE(fixTask.hasPendingNormalizedCommand());

    sequenceMessage first{};
    ASSERT_TRUE(shardQueue.pop(first));
    ASSERT_TRUE(first.clientCommandId.has_value());
    EXPECT_EQ(first.clientCommandId->value(), "FIRST");
    ASSERT_TRUE(fixTask.processNextNormalizedCommand());

    sequenceMessage second{};
    ASSERT_TRUE(shardQueue.pop(second));
    ASSERT_TRUE(second.clientCommandId.has_value());
    EXPECT_EQ(second.clientCommandId->value(), "SECOND");
    EXPECT_FALSE(fixTask.processNextNormalizedCommand());
    EXPECT_EQ(admissionIndex.statistics().firstSubmissions, 2u);
}
