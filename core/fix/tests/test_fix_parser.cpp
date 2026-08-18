#include "fix_parser.hpp"
#include <chrono>
#include <gtest/gtest.h>

using namespace exchange::core::fix;
using namespace exchange::core::task;
using namespace exchange::sequencer;

namespace {

FIX::Message makeSpyOrder(const std::string& quantity, const std::string& price) {
    FIX::Message message;
    message.getHeader().setField(FIX::MsgType("D"));
    message.setField(FIX::ClOrdID("NUMERIC-BOUNDARY"));
    message.setField(FIX::Symbol("SPY"));
    message.setField(FIX::Side(FIX::Side_BUY));
    message.setField(FIX::StringField(FIX::FIELD::OrderQty, quantity));
    message.setField(FIX::StringField(FIX::FIELD::Price, price));
    message.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    message.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));
    return message;
}

std::chrono::system_clock::time_point atcCloseFor(const std::chrono::system_clock::time_point now) {
    constexpr int64_t secondsPerDay = 86400;
    constexpr int64_t atcCloseSeconds = (16 * 60 * 60) + (30 * 60);

    const auto secondsSinceEpoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const auto currentUtcMidnightSeconds = (secondsSinceEpoch / secondsPerDay) * secondsPerDay;
    return std::chrono::system_clock::time_point{std::chrono::seconds(currentUtcMidnightSeconds + atcCloseSeconds)};
}

} // namespace

TEST(FixParserTest, ProcessableTypes) {
    EXPECT_TRUE(isProcessableMessageType("D"));
    EXPECT_TRUE(isProcessableMessageType("F"));
    EXPECT_FALSE(isProcessableMessageType("Z"));
}

TEST(FixParserTest, ParseNewOrderSingle) {
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::ClOrdID("ABC123"));
    msg.setField(FIX::Symbol("SPY"));
    msg.setField(FIX::Side(FIX::Side_BUY));
    msg.setField(FIX::OrderQty(100));
    msg.setField(FIX::Price(12.34));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    msg.setField(FIX::TimeInForce('0'));

    FIX::SessionID sid("FIX.4.4", "SENDER", "TARGET");
    auto seq = parseFixMessage(msg, sid, 4);

    EXPECT_EQ(seq.type, orderType::BUY);
    EXPECT_EQ(seq.quantity, 100);
    EXPECT_EQ(seq.price, 123400u);
    EXPECT_STREQ(seq.symbol, "SPY");
    EXPECT_EQ(seq.instrumentId, 1u);
    EXPECT_EQ(seq.configurationVersion, 1u);
    EXPECT_LT(seq.shard_id, 4);
    EXPECT_NE(seq.id, 0);
}

TEST(FixParserTest, SpyV1AcceptsExactNumericBoundariesAndEquivalentDecimals) {
    const FIX::SessionID session("FIX.4.4", "SENDER", "TARGET");

    const auto maximum = parseFixMessage(makeSpyOrder("100000000.0", "1000000.0000"), session, 1);
    EXPECT_EQ(maximum.quantity, 100000000u);
    EXPECT_EQ(maximum.price, 10000000000u);
    EXPECT_EQ(maximum.instrumentId, 1u);
    EXPECT_EQ(maximum.configurationVersion, 1u);

    const auto equivalent = parseFixMessage(makeSpyOrder("1", "12.34000"), session, 1);
    EXPECT_EQ(equivalent.quantity, 1u);
    EXPECT_EQ(equivalent.price, 123400u);
}

TEST(FixParserTest, SpyV1RejectsNumericValuesOutsideConfiguredBoundaries) {
    const FIX::SessionID session("FIX.4.4", "SENDER", "TARGET");

    for (const std::string price : {"0", "0.00009", "12.34001", "1000000.0001"}) {
        EXPECT_THROW(parseFixMessage(makeSpyOrder("1", price), session, 1), FixValidationError) << "price=" << price;
    }
    for (const std::string quantity : {"0", "1.5", "100000001"}) {
        EXPECT_THROW(parseFixMessage(makeSpyOrder(quantity, "1.0000"), session, 1), FixValidationError)
            << "quantity=" << quantity;
    }
}

TEST(FixParserTest, RejectsUnknownInstrumentBeforeSequencing) {
    FIX::Message message = makeSpyOrder("1", "1.0000");
    message.setField(FIX::Symbol("TEST"));

    const FIX::SessionID session("FIX.4.4", "SENDER", "TARGET");
    EXPECT_THROW(parseFixMessage(message, session, 1), FixValidationError);
}

TEST(FixParserTest, DayOrderMissingExpiryDefaultsToEndOfUtcDay) {
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::ClOrdID("EXP123"));
    msg.setField(FIX::Symbol("SPY"));
    msg.setField(FIX::Side(FIX::Side_BUY));
    msg.setField(FIX::OrderQty(100));
    msg.setField(FIX::Price(12.34));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    msg.setField(FIX::TimeInForce('0'));

    FIX::SessionID sid("FIX.4.4", "SENDER", "TARGET");
    auto before = std::chrono::system_clock::now();
    auto seq = parseFixMessage(msg, sid, 4);
    auto after = std::chrono::system_clock::now();

    auto beforeSeconds = std::chrono::duration_cast<std::chrono::seconds>(before.time_since_epoch()).count();
    auto afterSeconds = std::chrono::duration_cast<std::chrono::seconds>(after.time_since_epoch()).count();
    auto minExpected =
        std::chrono::system_clock::time_point{std::chrono::seconds(((beforeSeconds / 86400) + 1) * 86400)};
    auto maxExpected =
        std::chrono::system_clock::time_point{std::chrono::seconds(((afterSeconds / 86400) + 1) * 86400)};

    EXPECT_GE(seq.expiry, minExpected);
    EXPECT_LE(seq.expiry, maxExpected);
}

TEST(FixParserTest, AtcOrderDefaultsToSameUtcDayClose) {
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::ClOrdID("ATC123"));
    msg.setField(FIX::Symbol("SPY"));
    msg.setField(FIX::Side(FIX::Side_BUY));
    msg.setField(FIX::OrderQty(100));
    msg.setField(FIX::Price(12.34));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    msg.setField(FIX::TimeInForce(FIX::TimeInForce_AT_THE_CLOSE));

    FIX::SessionID sid("FIX.4.4", "SENDER", "TARGET");
    const auto before = std::chrono::system_clock::now();
    const auto seq = parseFixMessage(msg, sid, 4);
    const auto after = std::chrono::system_clock::now();

    EXPECT_EQ(seq.tif, TimeInForce::ATC);
    EXPECT_GE(seq.expiry, atcCloseFor(before));
    EXPECT_LE(seq.expiry, atcCloseFor(after));
}
