#include "fix_parser.hpp"
#include <chrono>
#include <gtest/gtest.h>

using namespace exchange::core::fix;
using namespace exchange::core::task;
using namespace exchange::sequencer;

namespace {

std::chrono::system_clock::time_point atcCloseFor(const std::chrono::system_clock::time_point now) {
    constexpr int64_t secondsPerDay = 86400;
    constexpr int64_t atcCloseSeconds = (16 * 60 * 60) + (30 * 60);

    const auto secondsSinceEpoch =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const auto currentUtcMidnightSeconds = (secondsSinceEpoch / secondsPerDay) * secondsPerDay;
    return std::chrono::system_clock::time_point{
        std::chrono::seconds(currentUtcMidnightSeconds + atcCloseSeconds)};
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
    msg.setField(FIX::Symbol("TEST"));
    msg.setField(FIX::Side(FIX::Side_BUY));
    msg.setField(FIX::OrderQty(100));
    msg.setField(FIX::Price(12.34));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));
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

TEST(FixParserTest, DayOrderMissingExpiryDefaultsToEndOfUtcDay) {
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::ClOrdID("EXP123"));
    msg.setField(FIX::Symbol("TEST"));
    msg.setField(FIX::Side(FIX::Side_BUY));
    msg.setField(FIX::OrderQty(100));
    msg.setField(FIX::Price(12.34));
    msg.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    msg.setField(FIX::TimeInForce('0'));

    FIX::SessionID sid("FIX.4.4", "SENDER", "TARGET");
    auto before = std::chrono::system_clock::now();
    auto seq = parseFixMessage(msg, sid, 4);
    auto after = std::chrono::system_clock::now();

    auto beforeSeconds =
        std::chrono::duration_cast<std::chrono::seconds>(before.time_since_epoch()).count();
    auto afterSeconds =
        std::chrono::duration_cast<std::chrono::seconds>(after.time_since_epoch()).count();
    auto minExpected = std::chrono::system_clock::time_point{
        std::chrono::seconds(((beforeSeconds / 86400) + 1) * 86400)};
    auto maxExpected = std::chrono::system_clock::time_point{
        std::chrono::seconds(((afterSeconds / 86400) + 1) * 86400)};

    EXPECT_GE(seq.expiry, minExpected);
    EXPECT_LE(seq.expiry, maxExpected);
}

TEST(FixParserTest, AtcOrderDefaultsToSameUtcDayClose) {
    FIX::Message msg;
    msg.getHeader().setField(FIX::MsgType("D"));
    msg.setField(FIX::ClOrdID("ATC123"));
    msg.setField(FIX::Symbol("TEST"));
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
