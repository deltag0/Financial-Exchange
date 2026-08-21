#include <gtest/gtest.h>

#include "fix_parser.hpp"
#include "fix_task.hpp"

using namespace exchange::sequencer;
using namespace exchange::core::task;

namespace {

FIX::Message makeValidOrder(const char timeInForce = FIX::TimeInForce_GOOD_TILL_CANCEL) {
    FIX::Message message;
    message.getHeader().setField(FIX::MsgType("D"));
    message.setField(FIX::ClOrdID("VALID"));
    message.setField(FIX::Symbol("SPY"));
    message.setField(FIX::Side(FIX::Side_BUY));
    message.setField(FIX::OrderQty(100));
    message.setField(FIX::Price(12.34));
    message.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    message.setField(FIX::TimeInForce(timeInForce));
    return message;
}

} // namespace

TEST(FixParserEdgeCases, FixCancelIsRejectedBecauseTargetOrderIdMappingIsUnresolved) {
    FIX::SessionID sid("FIX.4.4", "S", "T");
    const auto before = exchange::core::fix::parseFixMessage(makeValidOrder(), sid, 3);

    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("F"));
    msg.setField(FIX::ClOrdID("CXL1"));
    msg.setField(FIX::OrigClOrdID("CLIENT-ORDER-REFERENCE"));
    msg.setField(FIX::Symbol("SPY"));

    EXPECT_THROW(exchange::core::fix::parseFixMessage(msg, sid, 3), exchange::core::fix::FixValidationError);

    const auto after = exchange::core::fix::parseFixMessage(makeValidOrder(), sid, 3);
    EXPECT_EQ(after.order, before.order + 1);
}

TEST(FixParserEdgeCases, MissingSymbolQtyPriceClOrdIDTIF) {
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::Side(FIX::Side_SELL));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));

    FIX::SessionID sid("FIX.4.4", "S", "T");
    EXPECT_THROW(exchange::core::fix::parseFixMessage(msg, sid, 5), FIX::FieldNotFound);
}

TEST(FixParserEdgeCases, MissingClientCommandIdIsRejectedIndependently) {
    FIX::Message message;
    message.getHeader().setField(FIX::MsgType("D"));
    message.setField(FIX::Symbol("SPY"));
    message.setField(FIX::Side(FIX::Side_BUY));
    message.setField(FIX::OrderQty(1));
    message.setField(FIX::Price(1.0));
    message.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    message.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));

    const FIX::SessionID session("FIX.4.4", "S", "T");
    EXPECT_THROW(exchange::core::fix::parseFixMessage(message, session, 1), FIX::FieldNotFound);
}

TEST(FixParserEdgeCases, MissingTimeInForceIsRejectedIndependently) {
    FIX::Message message = makeValidOrder();
    message.removeField(FIX::FIELD::TimeInForce);

    const FIX::SessionID session("FIX.4.4", "S", "T");
    EXPECT_THROW(exchange::core::fix::parseFixMessage(message, session, 1), FIX::FieldNotFound);
}

TEST(FixParserEdgeCases, ConfiguredMaximumValuesAndOrderCounter) {
    FIX::Message msg1;
    msg1.getHeader().setField(FIX::MsgType("D"));
    msg1.setField(FIX::ClOrdID("L1"));
    msg1.setField(FIX::Symbol("SPY"));
    msg1.setField(FIX::Side(FIX::Side_BUY));
    msg1.setField(FIX::StringField(FIX::FIELD::OrderQty, "100000000"));
    msg1.setField(FIX::StringField(FIX::FIELD::Price, "1000000.0000"));
    msg1.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    msg1.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));

    FIX::SessionID sid("FIX.4.4", "S", "T");
    auto seq1 = exchange::core::fix::parseFixMessage(msg1, sid, 2);

    FIX::Message msg2 = msg1;
    auto seq2 = exchange::core::fix::parseFixMessage(msg2, sid, 2);

    EXPECT_GT(seq1.order, 0u);
    EXPECT_EQ(seq2.order, seq1.order + 1);
    EXPECT_EQ(seq1.quantity.value(), 100000000u);
    EXPECT_EQ(seq1.price.value(), 10000000000u);
}

TEST(FixParserEdgeCases, HeaderMissingThrows) {
    FIX::Message msg;
    // Intentionally do not set MsgType in header
    FIX::SessionID sid("FIX.4.4", "S", "T");
    EXPECT_THROW(exchange::core::fix::parseFixMessage(msg, sid, 1), FIX::FieldNotFound);
}

TEST(FixTaskEdgeCases, NonProcessableMessageDoesNotPush) {
    exchange::core::SharedQueue<sequenceMessage> seq_q(8);
    std::vector<exchange::core::SharedQueue<sequenceMessage>*> sequencer_queues;
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
    std::vector<exchange::core::SharedQueue<sequenceMessage>*> sequencer_queues;
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

TEST(FixTaskEdgeCases, UnmappedFixCancelDoesNotPushToQueue) {
    exchange::core::SharedQueue<sequenceMessage> seq_q(8);
    std::vector<exchange::core::SharedQueue<sequenceMessage>*> sequencer_queues;
    sequencer_queues.push_back(&seq_q);
    exchange::core::Bus bus(8);

    FixTask fix_task(sequencer_queues, bus);

    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("F"));
    msg.setField(FIX::ClOrdID("CXL42"));
    msg.setField(FIX::Symbol("SPY"));
    FIX::SessionID sid("FIX.4.4", "S", "T");

    fix_task.fromApp(msg, sid);

    sequenceMessage out{};
    EXPECT_FALSE(fix_task.getFixMessageQueue()->pop(out));
}

TEST(FixParserValidation, RejectsEveryUnsupportedTimeInForce) {
    const FIX::SessionID session("FIX.4.4", "S", "T");
    const auto before = exchange::core::fix::parseFixMessage(makeValidOrder(), session, 1);
    for (const char timeInForce : {FIX::TimeInForce_DAY, FIX::TimeInForce_FILL_OR_KILL, FIX::TimeInForce_GOOD_TILL_DATE,
                                   FIX::TimeInForce_GOOD_TILL_CROSSING, FIX::TimeInForce_AT_THE_CLOSE, 'Z'}) {
        const FIX::Message message = makeValidOrder(timeInForce);
        EXPECT_THROW(exchange::core::fix::parseFixMessage(message, session, 1), exchange::core::fix::FixValidationError)
            << "TimeInForce=" << timeInForce;
    }

    FIX::Message missing = makeValidOrder();
    missing.removeField(FIX::FIELD::TimeInForce);
    EXPECT_THROW(exchange::core::fix::parseFixMessage(missing, session, 1), FIX::FieldNotFound);

    const auto after =
        exchange::core::fix::parseFixMessage(makeValidOrder(FIX::TimeInForce_IMMEDIATE_OR_CANCEL), session, 1);
    EXPECT_EQ(after.order, before.order + 1);
}

TEST(FixParserValidation, GtcAndIocRejectExpireTime) {
    const FIX::SessionID session("FIX.4.4", "S", "T");
    for (const char timeInForce : {FIX::TimeInForce_GOOD_TILL_CANCEL, FIX::TimeInForce_IMMEDIATE_OR_CANCEL}) {
        FIX::Message message = makeValidOrder(timeInForce);
        message.setField(FIX::StringField(FIX::FIELD::ExpireTime, "20990101-00:00:00"));
        EXPECT_THROW(exchange::core::fix::parseFixMessage(message, session, 1),
                     exchange::core::fix::FixValidationError);
    }
}

TEST(FixParserValidation, RejectsUnsupportedOrderTypeWithValidTimeInForce) {
    FIX::Message message = makeValidOrder();
    message.setField(FIX::OrdType(FIX::OrdType_MARKET));

    const FIX::SessionID session("FIX.4.4", "S", "T");
    EXPECT_THROW(exchange::core::fix::parseFixMessage(message, session, 1), exchange::core::fix::FixValidationError);
}

TEST(FixTaskValidation, InvalidOrderDoesNotPushToQueue) {
    exchange::core::SharedQueue<sequenceMessage> seq_q(8);
    std::vector<exchange::core::SharedQueue<sequenceMessage>*> sequencer_queues{&seq_q};
    exchange::core::Bus bus(8);
    FixTask fix_task(sequencer_queues, bus);

    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::ClOrdID("DAYEXP"));
    msg.setField(FIX::Symbol("SPY"));
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
