#include "fix_parser.hpp"
#include <gtest/gtest.h>

using namespace exchange::core::fix;
using namespace exchange::sequencer;

TEST(FixParserTest, ProcessableTypes) {
    EXPECT_TRUE(isProcessableMessageType("D"));
    EXPECT_TRUE(isProcessableMessageType("F"));
    EXPECT_FALSE(isProcessableMessageType("Z"));
}

TEST(FixParserTest, ParseNewOrderSingle) {
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::ClOrdID("ABC123"));
    msg.setField(FIX::Symbol("TEST"));
    msg.setField(FIX::Side(FIX::Side_BUY));
    msg.setField(FIX::OrderQty(100));
    msg.setField(FIX::Price(12.34));
    msg.setField(FIX::TimeInForce('0'));

    FIX::SessionID sid("FIX.4.4", "SENDER", "TARGET");
    auto seq = parseFixMessage(msg, sid, 4);

    EXPECT_EQ(seq.type, orderType::BUY);
    EXPECT_EQ(seq.quantity, 100);
    EXPECT_EQ(seq.price, static_cast<uint64_t>(12.34 * 10000));
    EXPECT_STREQ(seq.symbol, "TEST");
    EXPECT_LT(seq.shard_id, 4);
    EXPECT_NE(seq.id, 0);
}
