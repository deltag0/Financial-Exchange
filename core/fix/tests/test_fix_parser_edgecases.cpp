#include <gtest/gtest.h>

#include "fix_parser.hpp"
#include "fix_task.hpp"
#include "fix_test_identities.hpp"

#include <limits>

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

FIX::Message makeValidCancel(const std::string& orderId = "42", const std::string& clientCommandId = "CXL-42",
                             const std::string& originalClientCommandId = "ORIGINAL-CLIENT-REFERENCE") {
    FIX::Message message;
    message.getHeader().setField(FIX::MsgType("F"));
    message.setField(FIX::ClOrdID(clientCommandId));
    message.setField(FIX::StringField(FIX::FIELD::OrderID, orderId));
    message.setField(FIX::OrigClOrdID(originalClientCommandId));
    message.setField(FIX::Symbol("SPY"));
    return message;
}

} // namespace

TEST(FixParserCancelNormalizationTest, UsesOrderIdAndKeepsCommandAndCorrelationIdentifiersIndependent) {
    const FIX::SessionID session("FIX.4.4", "S", "T");
    const auto first =
        exchange::core::fix::parseFixMessage(makeValidCancel("42", "CANCEL-COMMAND", "UNRELATED-ORIGINAL"), session,
                                             exchange::core::fix::test::clientIdentityResolver());
    const auto differentTarget =
        exchange::core::fix::parseFixMessage(makeValidCancel("43", "CANCEL-COMMAND", "UNRELATED-ORIGINAL"), session,
                                             exchange::core::fix::test::clientIdentityResolver());
    const auto differentCommand =
        exchange::core::fix::parseFixMessage(makeValidCancel("42", "OTHER-CANCEL", "UNRELATED-ORIGINAL"), session,
                                             exchange::core::fix::test::clientIdentityResolver());
    const auto differentCorrelation =
        exchange::core::fix::parseFixMessage(makeValidCancel("42", "CANCEL-COMMAND", "DIFFERENT-ORIGINAL"), session,
                                             exchange::core::fix::test::clientIdentityResolver());

    EXPECT_EQ(first.type, orderType::CANCEL);
    EXPECT_EQ(first.orderId, exchange::domain::OrderId{});
    EXPECT_EQ(first.targetOrderId, std::optional<exchange::domain::TargetOrderId>{exchange::domain::TargetOrderId{42}});
    ASSERT_TRUE(first.clientCommandId.has_value());
    EXPECT_EQ(first.clientCommandId->value(), "CANCEL-COMMAND");
    EXPECT_EQ(first.instrumentId, exchange::domain::InstrumentId{1});
    EXPECT_EQ(first.configurationVersion, 1u);
    EXPECT_STREQ(first.symbol, "SPY");
    EXPECT_NE(first.targetOrderId, differentTarget.targetOrderId);
    EXPECT_EQ(first.clientCommandId, differentTarget.clientCommandId);
    EXPECT_EQ(first.targetOrderId, differentCommand.targetOrderId);
    EXPECT_NE(first.clientCommandId, differentCommand.clientCommandId);
    EXPECT_EQ(first.targetOrderId, differentCorrelation.targetOrderId);
    EXPECT_EQ(first.clientCommandId, differentCorrelation.clientCommandId);
}

TEST(FixParserCancelNormalizationTest, PreservesMaximumUint64TargetOrderIdExactly) {
    const FIX::SessionID session("FIX.4.4", "S", "T");
    const std::string maximumClientCommandId(64, 'C');
    const auto cancel =
        exchange::core::fix::parseFixMessage(makeValidCancel("18446744073709551615", maximumClientCommandId), session,
                                             exchange::core::fix::test::clientIdentityResolver());

    EXPECT_EQ(cancel.targetOrderId, std::optional<exchange::domain::TargetOrderId>{
                                        exchange::domain::TargetOrderId{std::numeric_limits<std::uint64_t>::max()}});
    ASSERT_TRUE(cancel.clientCommandId.has_value());
    EXPECT_EQ(cancel.clientCommandId->value(), maximumClientCommandId);
}

TEST(FixParserCancelNormalizationTest, RejectsMissingEmptySignedWhitespaceAndNonDecimalOrderId) {
    const FIX::SessionID session("FIX.4.4", "S", "T");

    FIX::Message missing = makeValidCancel();
    missing.removeField(FIX::FIELD::OrderID);
    EXPECT_THROW(
        exchange::core::fix::parseFixMessage(missing, session, exchange::core::fix::test::clientIdentityResolver()),
        FIX::FieldNotFound);

    for (const std::string orderId : {"", "+1", "-1", " 1", "1 ", "1.0", "1a"}) {
        EXPECT_THROW(exchange::core::fix::parseFixMessage(makeValidCancel(orderId), session,
                                                          exchange::core::fix::test::clientIdentityResolver()),
                     exchange::core::fix::FixValidationError)
            << "OrderID=" << orderId;
    }
}

TEST(FixParserCancelNormalizationTest, RejectsZeroAndUint64Overflow) {
    const FIX::SessionID session("FIX.4.4", "S", "T");

    EXPECT_THROW(exchange::core::fix::parseFixMessage(makeValidCancel("0", "ZERO"), session,
                                                      exchange::core::fix::test::clientIdentityResolver()),
                 exchange::core::fix::FixValidationError);
    EXPECT_THROW(exchange::core::fix::parseFixMessage(makeValidCancel("18446744073709551616", "OVERFLOW"), session,
                                                      exchange::core::fix::test::clientIdentityResolver()),
                 exchange::core::fix::FixValidationError);
}

TEST(FixParserCancelNormalizationTest, RejectsUnknownInstrumentAndMissingRequiredCommandFields) {
    const FIX::SessionID session("FIX.4.4", "S", "T");

    FIX::Message unknown = makeValidCancel();
    unknown.setField(FIX::Symbol("UNKNOWN"));
    EXPECT_THROW(
        exchange::core::fix::parseFixMessage(unknown, session, exchange::core::fix::test::clientIdentityResolver()),
        exchange::core::fix::FixValidationError);

    for (const int field : {FIX::FIELD::ClOrdID, FIX::FIELD::Symbol}) {
        FIX::Message missing = makeValidCancel();
        missing.removeField(field);
        EXPECT_THROW(
            exchange::core::fix::parseFixMessage(missing, session, exchange::core::fix::test::clientIdentityResolver()),
            FIX::FieldNotFound)
            << "field=" << field;
    }

    FIX::Message emptyClientCommand = makeValidCancel();
    emptyClientCommand.setField(FIX::ClOrdID(""));
    EXPECT_THROW(exchange::core::fix::parseFixMessage(emptyClientCommand, session,
                                                      exchange::core::fix::test::clientIdentityResolver()),
                 exchange::core::fix::FixValidationError);

    FIX::Message overflowingClientCommand = makeValidCancel();
    overflowingClientCommand.setField(FIX::ClOrdID(std::string(65, 'C')));
    EXPECT_THROW(exchange::core::fix::parseFixMessage(overflowingClientCommand, session,
                                                      exchange::core::fix::test::clientIdentityResolver()),
                 exchange::core::fix::FixValidationError);
}

TEST(FixParserEdgeCases, MissingSymbolQtyPriceClOrdIDTIF) {
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::Side(FIX::Side_SELL));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));

    FIX::SessionID sid("FIX.4.4", "S", "T");
    EXPECT_THROW(exchange::core::fix::parseFixMessage(msg, sid, exchange::core::fix::test::clientIdentityResolver()),
                 FIX::FieldNotFound);
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
    EXPECT_THROW(
        exchange::core::fix::parseFixMessage(message, session, exchange::core::fix::test::clientIdentityResolver()),
        FIX::FieldNotFound);
}

TEST(FixParserEdgeCases, MissingTimeInForceIsRejectedIndependently) {
    FIX::Message message = makeValidOrder();
    message.removeField(FIX::FIELD::TimeInForce);

    const FIX::SessionID session("FIX.4.4", "S", "T");
    EXPECT_THROW(
        exchange::core::fix::parseFixMessage(message, session, exchange::core::fix::test::clientIdentityResolver()),
        FIX::FieldNotFound);
}

TEST(FixParserEdgeCases, ConfiguredMaximumValuesNormalizeDeterministically) {
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
    auto seq1 = exchange::core::fix::parseFixMessage(msg1, sid, exchange::core::fix::test::clientIdentityResolver());

    FIX::Message msg2 = msg1;
    auto seq2 = exchange::core::fix::parseFixMessage(msg2, sid, exchange::core::fix::test::clientIdentityResolver());

    EXPECT_EQ(seq1.quantity.value(), 100000000u);
    EXPECT_EQ(seq1.price.value(), 10000000000u);
    EXPECT_EQ(seq2.quantity, seq1.quantity);
    EXPECT_EQ(seq2.price, seq1.price);
    EXPECT_EQ(seq2.clientId, seq1.clientId);
    EXPECT_EQ(seq2.clientCommandId, seq1.clientCommandId);
}

TEST(FixParserEdgeCases, HeaderMissingThrows) {
    FIX::Message msg;
    // Intentionally do not set MsgType in header
    FIX::SessionID sid("FIX.4.4", "S", "T");
    EXPECT_THROW(exchange::core::fix::parseFixMessage(msg, sid, exchange::core::fix::test::clientIdentityResolver()),
                 FIX::FieldNotFound);
}

TEST(FixTaskEdgeCases, NonProcessableMessageDoesNotPush) {
    exchange::core::SharedQueue<sequenceMessage> seq_q(8);
    exchange::core::Bus bus(8);
    exchange::core::admission::CommandAdmissionIndex admissionIndex(8);

    FixTask fix_task(seq_q, bus, exchange::core::fix::test::clientIdentityResolver(), admissionIndex);

    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("Z"));
    FIX::SessionID sid("FIX.4.4", "S", "T");

    fix_task.fromApp(msg, sid);

    sequenceMessage out{};
    EXPECT_FALSE(fix_task.processNextStagedCommand());
    EXPECT_FALSE(seq_q.pop(out));
}

TEST(FixTaskEdgeCases, FromAppHandlesMissingHeader) {
    exchange::core::SharedQueue<sequenceMessage> seq_q(8);
    exchange::core::Bus bus(8);
    exchange::core::admission::CommandAdmissionIndex admissionIndex(8);

    FixTask fix_task(seq_q, bus, exchange::core::fix::test::clientIdentityResolver(), admissionIndex);

    FIX::Message msg; // no MsgType header
    FIX::SessionID sid("FIX.4.4", "S", "T");

    // Should not throw; fromApp catches FieldNotFound internally
    EXPECT_NO_THROW(fix_task.fromApp(msg, sid));
    sequenceMessage out{};
    EXPECT_FALSE(fix_task.processNextStagedCommand());
    EXPECT_FALSE(seq_q.pop(out));
    EXPECT_EQ(fix_task.statistics().normalizationRejections, 1u);
}

TEST(FixTaskCancelNormalizationTest, ValidCancelEntersSequencingIngressWithExactTarget) {
    exchange::core::SharedQueue<sequenceMessage> seq_q(8);
    exchange::core::Bus bus(8);
    exchange::core::admission::CommandAdmissionIndex admissionIndex(8);

    FixTask fix_task(seq_q, bus, exchange::core::fix::test::clientIdentityResolver(), admissionIndex);

    FIX::Message msg = makeValidCancel("18446744073709551615", "CXL42", "NOT-AN-ORDER-ID");
    FIX::SessionID sid("FIX.4.4", "S", "T");

    fix_task.fromApp(msg, sid);

    sequenceMessage out{};
    ASSERT_TRUE(fix_task.processNextStagedCommand());
    ASSERT_TRUE(seq_q.pop(out));
    EXPECT_EQ(out.type, orderType::CANCEL);
    EXPECT_EQ(out.orderId, exchange::domain::OrderId{});
    EXPECT_EQ(out.targetOrderId, std::optional<exchange::domain::TargetOrderId>{
                                     exchange::domain::TargetOrderId{std::numeric_limits<std::uint64_t>::max()}});
    ASSERT_TRUE(out.clientCommandId.has_value());
    EXPECT_EQ(out.clientCommandId->value(), "CXL42");
    EXPECT_EQ(out.instrumentId, exchange::domain::InstrumentId{1});
}

TEST(FixTaskCancelNormalizationTest, InvalidCancelNeverEntersSequencingIngress) {
    exchange::core::SharedQueue<sequenceMessage> seq_q(8);
    exchange::core::Bus bus(8);
    exchange::core::admission::CommandAdmissionIndex admissionIndex(8);
    FixTask fix_task(seq_q, bus, exchange::core::fix::test::clientIdentityResolver(), admissionIndex);
    const FIX::SessionID session("FIX.4.4", "S", "T");

    for (const std::string orderId : {"", "0", "-1", " 1", "abc", "18446744073709551616"}) {
        fix_task.fromApp(makeValidCancel(orderId), session);
        sequenceMessage output{};
        EXPECT_FALSE(fix_task.processNextStagedCommand()) << "OrderID=" << orderId;
        EXPECT_FALSE(seq_q.pop(output)) << "OrderID=" << orderId;
    }
    EXPECT_EQ(fix_task.statistics().normalizationRejections, 6u);
}

TEST(FixParserValidation, RejectsEveryUnsupportedTimeInForce) {
    const FIX::SessionID session("FIX.4.4", "S", "T");
    for (const char timeInForce : {FIX::TimeInForce_DAY, FIX::TimeInForce_FILL_OR_KILL, FIX::TimeInForce_GOOD_TILL_DATE,
                                   FIX::TimeInForce_GOOD_TILL_CROSSING, FIX::TimeInForce_AT_THE_CLOSE, 'Z'}) {
        const FIX::Message message = makeValidOrder(timeInForce);
        EXPECT_THROW(
            exchange::core::fix::parseFixMessage(message, session, exchange::core::fix::test::clientIdentityResolver()),
            exchange::core::fix::FixValidationError)
            << "TimeInForce=" << timeInForce;
    }

    FIX::Message missing = makeValidOrder();
    missing.removeField(FIX::FIELD::TimeInForce);
    EXPECT_THROW(
        exchange::core::fix::parseFixMessage(missing, session, exchange::core::fix::test::clientIdentityResolver()),
        FIX::FieldNotFound);

    const auto after =
        exchange::core::fix::parseFixMessage(makeValidOrder(FIX::TimeInForce_IMMEDIATE_OR_CANCEL), session,
                                             exchange::core::fix::test::clientIdentityResolver());
    EXPECT_EQ(after.tif, exchange::core::task::TimeInForce::IOC);
}

TEST(FixParserValidation, GtcAndIocRejectExpireTime) {
    const FIX::SessionID session("FIX.4.4", "S", "T");
    for (const char timeInForce : {FIX::TimeInForce_GOOD_TILL_CANCEL, FIX::TimeInForce_IMMEDIATE_OR_CANCEL}) {
        FIX::Message message = makeValidOrder(timeInForce);
        message.setField(FIX::StringField(FIX::FIELD::ExpireTime, "20990101-00:00:00"));
        EXPECT_THROW(
            exchange::core::fix::parseFixMessage(message, session, exchange::core::fix::test::clientIdentityResolver()),
            exchange::core::fix::FixValidationError);
    }
}

TEST(FixParserValidation, RejectsUnsupportedOrderTypeWithValidTimeInForce) {
    FIX::Message message = makeValidOrder();
    message.setField(FIX::OrdType(FIX::OrdType_MARKET));

    const FIX::SessionID session("FIX.4.4", "S", "T");
    EXPECT_THROW(
        exchange::core::fix::parseFixMessage(message, session, exchange::core::fix::test::clientIdentityResolver()),
        exchange::core::fix::FixValidationError);
}

TEST(FixTaskValidation, InvalidOrderDoesNotPushToQueue) {
    exchange::core::SharedQueue<sequenceMessage> seq_q(8);
    exchange::core::Bus bus(8);
    exchange::core::admission::CommandAdmissionIndex admissionIndex(8);
    FixTask fix_task(seq_q, bus, exchange::core::fix::test::clientIdentityResolver(), admissionIndex);

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
    EXPECT_FALSE(fix_task.processNextStagedCommand());
    EXPECT_FALSE(seq_q.pop(out));
}
