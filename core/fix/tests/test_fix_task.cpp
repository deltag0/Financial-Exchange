#include "fix_parser.hpp"
#include "fix_task.hpp"
#include "fix_test_identities.hpp"
#include <gtest/gtest.h>

using namespace exchange::core::task;
using namespace exchange::sequencer;

TEST(FixTaskTest, FromAppPushesToQueue) {
    // Prepare sequencer queue and multicast bus
    exchange::core::SharedQueue<sequenceMessage> seq_q(16);
    std::vector<exchange::core::SharedQueue<sequenceMessage>*> sequencer_queues;
    sequencer_queues.push_back(&seq_q);

    exchange::core::Bus bus(8);
    exchange::core::admission::CommandAdmissionIndex admissionIndex(8);

    FixTask fix_task(sequencer_queues, bus, exchange::core::fix::test::clientIdentityResolver(), admissionIndex);

    // Build a simple NewOrderSingle
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::ClOrdID("ORD1"));
    msg.setField(FIX::Symbol("SPY"));
    msg.setField(FIX::Side(FIX::Side_BUY));
    msg.setField(FIX::OrderQty(5));
    msg.setField(FIX::Price(1.23));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    msg.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));

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
