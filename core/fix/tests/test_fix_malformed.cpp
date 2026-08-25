#include "fix_parser.hpp"
#include "fix_task.hpp"
#include "fix_test_identities.hpp"
#include <gtest/gtest.h>

using namespace exchange::sequencer;
using exchange::core::task::FixTask;

TEST(FixMalformed, MalformedNumericFieldsThrow) {
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::ClOrdID("MAL1"));
    msg.setField(FIX::Symbol("SPY"));
    msg.setField(FIX::Side(FIX::Side_BUY));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    msg.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));
    // Intentionally set non-numeric OrderQty and Price using StringField
    msg.setField(FIX::StringField(38, "not-a-number"));
    msg.setField(FIX::StringField(44, "nope"));

    FIX::SessionID sid("FIX.4.4", "S", "T");
    EXPECT_THROW(exchange::core::fix::parseFixMessage(msg, sid, exchange::core::fix::test::clientIdentityResolver(), 1),
                 std::exception);
}

TEST(FixMalformed, FixTaskFromAppHandlesMalformed) {
    exchange::core::SharedQueue<sequenceMessage> q(4);
    std::vector<exchange::core::SharedQueue<sequenceMessage>*> shards{&q};
    exchange::core::Bus bus(8);
    FixTask fix_task(shards, bus, exchange::core::fix::test::clientIdentityResolver());

    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::ClOrdID("MAL2"));
    msg.setField(FIX::Symbol("SPY"));
    msg.setField(FIX::Side(FIX::Side_BUY));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    msg.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));
    msg.setField(FIX::StringField(38, "not-a-number"));

    FIX::SessionID sid("FIX.4.4", "S", "T");
    // fromApp should catch parse exceptions and not throw
    EXPECT_NO_THROW(fix_task.fromApp(msg, sid));
    sequenceMessage out{};
    EXPECT_FALSE(fix_task.getFixMessageQueue()->pop(out));
}

TEST(FixMalformed, UnknownOverlongSymbolIsRejected) {
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::ClOrdID("LONG1"));
    std::string longsym = "VERYLONGSYMBOL123";
    msg.setField(FIX::Symbol(longsym));
    msg.setField(FIX::Side(FIX::Side_SELL));
    msg.setField(FIX::OrderQty(1));
    msg.setField(FIX::Price(1.01));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    msg.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));

    FIX::SessionID sid("FIX.4.4", "S", "T");
    EXPECT_THROW(exchange::core::fix::parseFixMessage(msg, sid, exchange::core::fix::test::clientIdentityResolver(), 4),
                 exchange::core::fix::FixValidationError);
}

TEST(FixMalformed, UnknownNonAsciiSymbolIsRejected) {
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::ClOrdID("UNI1"));
    std::string uni = "T\x80ST\x81"; // non-ascii bytes
    msg.setField(FIX::Symbol(uni));
    msg.setField(FIX::Side(FIX::Side_BUY));
    msg.setField(FIX::OrderQty(2));
    msg.setField(FIX::Price(1.01));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    msg.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));

    FIX::SessionID sid("FIX.4.4", "S", "T");
    EXPECT_THROW(exchange::core::fix::parseFixMessage(msg, sid, exchange::core::fix::test::clientIdentityResolver(), 2),
                 exchange::core::fix::FixValidationError);
}
