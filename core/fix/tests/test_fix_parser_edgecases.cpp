#include <gtest/gtest.h>

#include "fix_parser.hpp"
#include "fix_task.hpp"

using namespace exchange::sequencer;
using namespace exchange::core::task;

TEST(FixParserEdgeCases, ParseCancelMessage) {
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("F"));
    msg.setField(FIX::ClOrdID("CXL1"));
    msg.setField(FIX::Symbol("CXLSYM"));

    FIX::SessionID sid("FIX.4.4", "S", "T");
    auto seq = exchange::core::fix::parseFixMessage(msg, sid, 3);

    EXPECT_EQ(seq.type, orderType::CANCEL);
    EXPECT_STREQ(seq.symbol, "CXLSYM");
    EXPECT_NE(seq.id, 0);
}

TEST(FixParserEdgeCases, MissingSymbolQtyPriceClOrdIDTIF) {
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::Side(FIX::Side_SELL));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));

    FIX::SessionID sid("FIX.4.4", "S", "T");
    EXPECT_THROW(exchange::core::fix::parseFixMessage(msg, sid, 5), FIX::FieldNotFound);
}

TEST(FixParserEdgeCases, LargeValuesAndOrderCounter) {
    FIX::Message msg1;
    msg1.getHeader().setField(FIX::MsgType("D"));
    msg1.setField(FIX::ClOrdID("L1"));
    msg1.setField(FIX::Symbol("BIG"));
    msg1.setField(FIX::Side(FIX::Side_BUY));
    msg1.setField(FIX::OrderQty(static_cast<double>(1000000000))); // 1e9
    msg1.setField(FIX::Price(123456.789));
    msg1.setField(FIX::OrdType(FIX::OrdType_LIMIT));

    FIX::SessionID sid("FIX.4.4", "S", "T");
    auto seq1 = exchange::core::fix::parseFixMessage(msg1, sid, 2);

    FIX::Message msg2 = msg1;
    auto seq2 = exchange::core::fix::parseFixMessage(msg2, sid, 2);

    EXPECT_GT(seq1.order, 0u);
    EXPECT_EQ(seq2.order, seq1.order + 1);
    EXPECT_EQ(seq1.quantity, static_cast<uint64_t>(1000000000));
    EXPECT_EQ(seq1.price, static_cast<uint64_t>(123456.789 * 10000));
}

TEST(FixParserEdgeCases, HeaderMissingThrows) {
    FIX::Message msg;
    // Intentionally do not set MsgType in header
    FIX::SessionID sid("FIX.4.4", "S", "T");
    EXPECT_THROW(exchange::core::fix::parseFixMessage(msg, sid, 1), FIX::FieldNotFound);
}

TEST(FixTaskEdgeCases, NonProcessableMessageDoesNotPush) {
    exchange::core::SharedQueue<sequenceMessage> seq_q(8);
    std::vector<exchange::core::SharedQueue<sequenceMessage> *> sequencer_queues;
    sequencer_queues.push_back(&seq_q);
    exchange::core::Bus bus(8);

    FixTask fix_task(sequencer_queues, bus);

    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("Z"));
    FIX::SessionID sid("FIX.4.4", "S", "T");

    fix_task.fromApp(msg, sid);

    sequenceMessage out{};
    EXPECT_FALSE(fix_task.getFixMessageQueue()->pop(out));
}

TEST(FixTaskEdgeCases, FromAppHandlesMissingHeader) {
    exchange::core::SharedQueue<sequenceMessage> seq_q(8);
    std::vector<exchange::core::SharedQueue<sequenceMessage> *> sequencer_queues;
    sequencer_queues.push_back(&seq_q);
    exchange::core::Bus bus(8);

    FixTask fix_task(sequencer_queues, bus);

    FIX::Message msg; // no MsgType header
    FIX::SessionID sid("FIX.4.4", "S", "T");

    // Should not throw; fromApp catches FieldNotFound internally
    EXPECT_NO_THROW(fix_task.fromApp(msg, sid));
    sequenceMessage out{};
    EXPECT_FALSE(fix_task.getFixMessageQueue()->pop(out));
}

TEST(FixTaskEdgeCases, CancelMessagePushesToQueue) {
    exchange::core::SharedQueue<sequenceMessage> seq_q(8);
    std::vector<exchange::core::SharedQueue<sequenceMessage> *> sequencer_queues;
    sequencer_queues.push_back(&seq_q);
    exchange::core::Bus bus(8);

    FixTask fix_task(sequencer_queues, bus);

    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("F"));
    msg.setField(FIX::ClOrdID("CXL42"));
    msg.setField(FIX::Symbol("CXL"));
    FIX::SessionID sid("FIX.4.4", "S", "T");

    fix_task.fromApp(msg, sid);

    sequenceMessage out{};
    EXPECT_TRUE(fix_task.getFixMessageQueue()->pop(out));
    EXPECT_EQ(out.type, orderType::CANCEL);
    EXPECT_STREQ(out.symbol, "CXL");
}

TEST(FixParserValidation, DayOrderRejectsExplicitExpireTime) {
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::ClOrdID("DAYEXP"));
    msg.setField(FIX::Symbol("TEST"));
    msg.setField(FIX::Side(FIX::Side_BUY));
    msg.setField(FIX::OrderQty(100));
    msg.setField(FIX::Price(12.34));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    msg.setField(FIX::TimeInForce(FIX::TimeInForce_DAY));
    msg.setField(FIX::StringField(FIX::FIELD::ExpireTime, "20990101-00:00:00"));

    FIX::SessionID sid("FIX.4.4", "S", "T");
    EXPECT_THROW(exchange::core::fix::parseFixMessage(msg, sid, 1),
                 exchange::core::fix::FixValidationError);
}

TEST(FixParserValidation, GtdRequiresFutureExpireTime) {
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::ClOrdID("GTD1"));
    msg.setField(FIX::Symbol("TEST"));
    msg.setField(FIX::Side(FIX::Side_BUY));
    msg.setField(FIX::OrderQty(100));
    msg.setField(FIX::Price(12.34));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    msg.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_DATE));

    FIX::SessionID sid("FIX.4.4", "S", "T");
    EXPECT_THROW(exchange::core::fix::parseFixMessage(msg, sid, 1),
                 exchange::core::fix::FixValidationError);

    msg.setField(FIX::StringField(FIX::FIELD::ExpireTime, "20000101-00:00:00"));
    EXPECT_THROW(exchange::core::fix::parseFixMessage(msg, sid, 1),
                 exchange::core::fix::FixValidationError);

    msg.setField(FIX::StringField(FIX::FIELD::ExpireTime, "20990101-00:00:00"));
    auto seq = exchange::core::fix::parseFixMessage(msg, sid, 1);
    EXPECT_EQ(seq.tif, TimeInForce::GTD);
}

TEST(FixParserValidation, IocFokAndGtcRejectExpireTime) {
    FIX::SessionID sid("FIX.4.4", "S", "T");

    for (char tif : {FIX::TimeInForce_IMMEDIATE_OR_CANCEL, FIX::TimeInForce_FILL_OR_KILL,
                     FIX::TimeInForce_GOOD_TILL_CANCEL}) {
        FIX::Message msg;
        msg.getHeader().setField(FIX::MsgType("D"));
        msg.setField(FIX::ClOrdID(std::string("TIF") + tif));
        msg.setField(FIX::Symbol("TEST"));
        msg.setField(FIX::Side(FIX::Side_BUY));
        msg.setField(FIX::OrderQty(100));
        msg.setField(FIX::Price(12.34));
        msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));
        msg.setField(FIX::TimeInForce(tif));
        msg.setField(FIX::StringField(FIX::FIELD::ExpireTime, "20990101-00:00:00"));

        EXPECT_THROW(exchange::core::fix::parseFixMessage(msg, sid, 1),
                     exchange::core::fix::FixValidationError);
    }
}

TEST(FixParserValidation, AtcRejectsExplicitExpireTime) {
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::ClOrdID("ATCEXP"));
    msg.setField(FIX::Symbol("TEST"));
    msg.setField(FIX::Side(FIX::Side_BUY));
    msg.setField(FIX::OrderQty(100));
    msg.setField(FIX::Price(12.34));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    msg.setField(FIX::TimeInForce(FIX::TimeInForce_AT_THE_CLOSE));
    msg.setField(FIX::StringField(FIX::FIELD::ExpireTime, "20990101-00:00:00"));

    FIX::SessionID sid("FIX.4.4", "S", "T");
    EXPECT_THROW(exchange::core::fix::parseFixMessage(msg, sid, 1),
                 exchange::core::fix::FixValidationError);
}

TEST(FixParserValidation, RejectsUnsupportedTifAndOrdType) {
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::ClOrdID("BADTIF"));
    msg.setField(FIX::Symbol("TEST"));
    msg.setField(FIX::Side(FIX::Side_BUY));
    msg.setField(FIX::OrderQty(100));
    msg.setField(FIX::Price(12.34));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    msg.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CROSSING));

    FIX::SessionID sid("FIX.4.4", "S", "T");
    EXPECT_THROW(exchange::core::fix::parseFixMessage(msg, sid, 1),
                 exchange::core::fix::FixValidationError);

    msg.setField(FIX::TimeInForce(FIX::TimeInForce_DAY));
    msg.setField(FIX::OrdType(FIX::OrdType_MARKET));
    EXPECT_THROW(exchange::core::fix::parseFixMessage(msg, sid, 1),
                 exchange::core::fix::FixValidationError);
}

TEST(FixTaskValidation, InvalidOrderDoesNotPushToQueue) {
    exchange::core::SharedQueue<sequenceMessage> seq_q(8);
    std::vector<exchange::core::SharedQueue<sequenceMessage> *> sequencer_queues{&seq_q};
    exchange::core::Bus bus(8);
    FixTask fix_task(sequencer_queues, bus);

    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::ClOrdID("DAYEXP"));
    msg.setField(FIX::Symbol("TEST"));
    msg.setField(FIX::Side(FIX::Side_BUY));
    msg.setField(FIX::OrderQty(100));
    msg.setField(FIX::Price(12.34));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    msg.setField(FIX::TimeInForce(FIX::TimeInForce_DAY));
    msg.setField(FIX::StringField(FIX::FIELD::ExpireTime, "20990101-00:00:00"));

    FIX::SessionID sid("FIX.4.4", "S", "T");
    fix_task.fromApp(msg, sid);

    sequenceMessage out{};
    EXPECT_FALSE(fix_task.getFixMessageQueue()->pop(out));
}
