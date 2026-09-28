#include "application_event_json.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace exchange::application_event {
namespace {

constexpr domain::ExchangeRunId RUN_ID{9};
constexpr domain::CommandSequence COMMAND_SEQUENCE{17};
constexpr domain::ClientId RECIPIENT{22};

domain::EventId eventId(const std::uint32_t index, const domain::ExchangeRunId runId = RUN_ID,
                        const domain::CommandSequence commandSequence = COMMAND_SEQUENCE) {
    return {
        .commandSequence = commandSequence,
        .eventIndex = domain::EventIndex{index},
        .exchangeRunId = runId,
    };
}

private_result::PrivateTrade privateTrade(const private_result::TradeRole role,
                                          const domain::Side side = domain::Side::BUY, const std::uint32_t index = 0,
                                          const domain::ExchangeRunId runId = RUN_ID,
                                          const domain::CommandSequence commandSequence = COMMAND_SEQUENCE) {
    return {
        .eventId = eventId(index, runId, commandSequence),
        .instrumentId = domain::InstrumentId{3},
        .orderId = role == private_result::TradeRole::MAKER ? domain::OrderId{101} : domain::OrderId{202},
        .side = side,
        .role = role,
        .executionPrice = domain::Price{1'250},
        .executionQuantity = domain::Quantity{7},
        .remainingQuantity = role == private_result::TradeRole::MAKER ? domain::Quantity{4} : domain::Quantity{0},
    };
}

private_result::RecipientResult recipientResult(
    private_result::PrivateResult events, std::optional<domain::CommandResultCorrelation> correlation = std::nullopt) {
    return {
        .recipient = RECIPIENT,
        .correlation = std::move(correlation),
        .privateResult = std::move(events),
    };
}

TEST(ApplicationEventJsonTest, SerializesExactPublicTradeGolden) {
    const std::vector<market_data::PublicTrade> trades{{
        .eventId = eventId(0),
        .instrumentId = domain::InstrumentId{3},
        .executionPrice = domain::Price{1'250},
        .executionQuantity = domain::Quantity{7},
        .aggressorSide = domain::Side::BUY,
    }};
    std::string output;

    EXPECT_EQ(serializePublicTradesV1(RUN_ID, trades, output), ApplicationEventJsonError::NONE);
    EXPECT_EQ(
        output,
        R"({"schemaVersion":1,"type":"publicTrades","exchangeRunId":"9","trades":[{"eventId":{"exchangeRunId":"9","commandSequence":"17","eventIndex":0},"instrumentId":"3","executionPrice":"1250","executionQuantity":"7","aggressorSide":"BUY"}]})");
}

TEST(ApplicationEventJsonTest, SerializesMakerAndTakerPrivateTradeGoldensWithoutCorrelation) {
    const auto maker = recipientResult({privateTrade(private_result::TradeRole::MAKER, domain::Side::SELL)});
    const auto taker = recipientResult({privateTrade(private_result::TradeRole::TAKER)});
    std::string output;

    ASSERT_EQ(serializePrivateResultV1(RUN_ID, maker, output), ApplicationEventJsonError::NONE);
    EXPECT_EQ(
        output,
        R"({"schemaVersion":1,"type":"privateResult","exchangeRunId":"9","recipientClientId":"22","events":[{"type":"privateTrade","eventId":{"exchangeRunId":"9","commandSequence":"17","eventIndex":0},"instrumentId":"3","orderId":"101","side":"SELL","role":"MAKER","executionPrice":"1250","executionQuantity":"7","remainingQuantity":"4"}]})");

    ASSERT_EQ(serializePrivateResultV1(RUN_ID, taker, output), ApplicationEventJsonError::NONE);
    EXPECT_EQ(
        output,
        R"({"schemaVersion":1,"type":"privateResult","exchangeRunId":"9","recipientClientId":"22","events":[{"type":"privateTrade","eventId":{"exchangeRunId":"9","commandSequence":"17","eventIndex":0},"instrumentId":"3","orderId":"202","side":"BUY","role":"TAKER","executionPrice":"1250","executionQuantity":"7","remainingQuantity":"0"}]})");
}

TEST(ApplicationEventJsonTest, SerializesOrderRestedGolden) {
    const auto result = recipientResult({domain::OrderRested{
        .eventId = eventId(1),
        .orderId = domain::OrderId{303},
        .clientId = RECIPIENT,
        .instrumentId = domain::InstrumentId{4},
        .side = domain::Side::SELL,
        .price = domain::Price{1'260},
        .remainingQuantity = domain::Quantity{5},
    }});
    std::string output;

    EXPECT_EQ(serializePrivateResultV1(RUN_ID, result, output), ApplicationEventJsonError::NONE);
    EXPECT_EQ(
        output,
        R"({"schemaVersion":1,"type":"privateResult","exchangeRunId":"9","recipientClientId":"22","events":[{"type":"orderRested","eventId":{"exchangeRunId":"9","commandSequence":"17","eventIndex":1},"orderId":"303","clientId":"22","instrumentId":"4","side":"SELL","price":"1260","remainingQuantity":"5"}]})");
}

TEST(ApplicationEventJsonTest, SerializesOrderCancelledGolden) {
    const auto result = recipientResult({domain::OrderCancelled{
        .eventId = eventId(2),
        .orderId = domain::OrderId{404},
        .clientId = RECIPIENT,
        .instrumentId = domain::InstrumentId{4},
        .cancelledQuantity = domain::Quantity{6},
        .reason = domain::CancelReason::IOC_REMAINDER,
    }});
    std::string output;

    EXPECT_EQ(serializePrivateResultV1(RUN_ID, result, output), ApplicationEventJsonError::NONE);
    EXPECT_EQ(
        output,
        R"({"schemaVersion":1,"type":"privateResult","exchangeRunId":"9","recipientClientId":"22","events":[{"type":"orderCancelled","eventId":{"exchangeRunId":"9","commandSequence":"17","eventIndex":2},"orderId":"404","clientId":"22","instrumentId":"4","cancelledQuantity":"6","reason":"IOC_REMAINDER"}]})");
}

TEST(ApplicationEventJsonTest, SerializesCommandRejectedWithCorrelationAndRelevantOrderGolden) {
    const auto result = recipientResult(
        {domain::CommandRejected{
            .eventId = eventId(3),
            .commandType = domain::CommandType::CANCEL,
            .clientId = RECIPIENT,
            .clientCommandId = domain::ClientCommandId{"CANCEL-3"},
            .relevantOrderId = domain::OrderId{505},
            .reason = domain::CommandRejectionReason::ORDER_NOT_ACTIVE,
        }},
        domain::CommandResultCorrelation{RECIPIENT, domain::ClientCommandId{"CANCEL-3"}, COMMAND_SEQUENCE, RUN_ID});
    std::string output;

    EXPECT_EQ(serializePrivateResultV1(RUN_ID, result, output), ApplicationEventJsonError::NONE);
    EXPECT_EQ(
        output,
        R"({"schemaVersion":1,"type":"privateResult","exchangeRunId":"9","recipientClientId":"22","correlation":{"exchangeRunId":"9","clientId":"22","clientCommandId":"CANCEL-3","commandSequence":"17"},"events":[{"type":"commandRejected","eventId":{"exchangeRunId":"9","commandSequence":"17","eventIndex":3},"commandType":"CANCEL","clientId":"22","clientCommandId":"CANCEL-3","relevantOrderId":"505","reason":"ORDER_NOT_ACTIVE"}]})");
}

TEST(ApplicationEventJsonTest, OmitsAbsentRelevantOrderAndEscapesEveryPermittedAsciiClass) {
    std::string commandId{"A\"\\\b\f\n\r\t"};
    commandId.push_back('\x01');
    commandId.push_back('\x7f');
    commandId.push_back('Z');
    const auto result = recipientResult({domain::CommandRejected{
        .eventId = eventId(4),
        .commandType = domain::CommandType::NEW_ORDER,
        .clientId = RECIPIENT,
        .clientCommandId = domain::ClientCommandId{std::string_view{commandId.data(), commandId.size()}},
        .relevantOrderId = std::nullopt,
        .reason = domain::CommandRejectionReason::BOOK_CAPACITY_EXCEEDED,
    }});
    std::string output;

    EXPECT_EQ(serializePrivateResultV1(RUN_ID, result, output), ApplicationEventJsonError::NONE);
    EXPECT_EQ(
        output,
        R"({"schemaVersion":1,"type":"privateResult","exchangeRunId":"9","recipientClientId":"22","events":[{"type":"commandRejected","eventId":{"exchangeRunId":"9","commandSequence":"17","eventIndex":4},"commandType":"NEW_ORDER","clientId":"22","clientCommandId":"A\"\\\b\f\n\r\t\u0001\u007fZ","reason":"BOOK_CAPACITY_EXCEEDED"}]})");
}

TEST(ApplicationEventJsonTest, PreservesSelfTradeAndMultiEventOrder) {
    const auto result = recipientResult({
        privateTrade(private_result::TradeRole::MAKER, domain::Side::SELL, 0),
        privateTrade(private_result::TradeRole::TAKER, domain::Side::BUY, 0),
        domain::OrderRested{
            .eventId = eventId(1),
            .orderId = domain::OrderId{303},
            .clientId = RECIPIENT,
            .instrumentId = domain::InstrumentId{3},
            .side = domain::Side::BUY,
            .price = domain::Price{1'240},
            .remainingQuantity = domain::Quantity{2},
        },
    });
    std::string output;

    ASSERT_EQ(serializePrivateResultV1(RUN_ID, result, output), ApplicationEventJsonError::NONE);
    const auto makerPosition = output.find("\"role\":\"MAKER\"");
    const auto takerPosition = output.find("\"role\":\"TAKER\"");
    const auto restedPosition = output.find("\"type\":\"orderRested\"");
    ASSERT_NE(makerPosition, std::string::npos);
    ASSERT_NE(takerPosition, std::string::npos);
    ASSERT_NE(restedPosition, std::string::npos);
    EXPECT_LT(makerPosition, takerPosition);
    EXPECT_LT(takerPosition, restedPosition);
}

TEST(ApplicationEventJsonTest, EncodesMaximumUnsignedValuesAsStringsAndEventIndexAsInteger) {
    constexpr auto MAXIMUM = std::numeric_limits<std::uint64_t>::max();
    const domain::ExchangeRunId maximumRun{MAXIMUM};
    const std::vector<market_data::PublicTrade> trades{{
        .eventId =
            {
                .commandSequence = domain::CommandSequence{MAXIMUM},
                .eventIndex = domain::EventIndex{std::numeric_limits<std::uint32_t>::max()},
                .exchangeRunId = maximumRun,
            },
        .instrumentId = domain::InstrumentId{MAXIMUM},
        .executionPrice = domain::Price{MAXIMUM},
        .executionQuantity = domain::Quantity{MAXIMUM},
        .aggressorSide = domain::Side::SELL,
    }};
    std::string output;

    ASSERT_EQ(serializePublicTradesV1(maximumRun, trades, output), ApplicationEventJsonError::NONE);
    EXPECT_EQ(
        output,
        R"({"schemaVersion":1,"type":"publicTrades","exchangeRunId":"18446744073709551615","trades":[{"eventId":{"exchangeRunId":"18446744073709551615","commandSequence":"18446744073709551615","eventIndex":4294967295},"instrumentId":"18446744073709551615","executionPrice":"18446744073709551615","executionQuantity":"18446744073709551615","aggressorSide":"SELL"}]})");
}

TEST(ApplicationEventJsonTest, MapsEveryRemainingEnumName) {
    const auto result = recipientResult({
        domain::OrderCancelled{
            .eventId = eventId(0),
            .orderId = domain::OrderId{1},
            .clientId = RECIPIENT,
            .instrumentId = domain::InstrumentId{3},
            .cancelledQuantity = domain::Quantity{1},
            .reason = domain::CancelReason::CLIENT_REQUESTED,
        },
        domain::CommandRejected{
            .eventId = eventId(1),
            .commandType = domain::CommandType::CANCEL,
            .clientId = RECIPIENT,
            .clientCommandId = domain::ClientCommandId{"NOT-OWNER"},
            .relevantOrderId = domain::OrderId{2},
            .reason = domain::CommandRejectionReason::NOT_OWNER,
        },
        domain::CommandRejected{
            .eventId = eventId(2),
            .commandType = domain::CommandType::NEW_ORDER,
            .clientId = RECIPIENT,
            .clientCommandId = domain::ClientCommandId{"CAPACITY"},
            .relevantOrderId = std::nullopt,
            .reason = domain::CommandRejectionReason::BOOK_CAPACITY_EXCEEDED,
        },
    });
    std::string output;

    ASSERT_EQ(serializePrivateResultV1(RUN_ID, result, output), ApplicationEventJsonError::NONE);
    EXPECT_NE(output.find("\"reason\":\"CLIENT_REQUESTED\""), std::string::npos);
    EXPECT_NE(output.find("\"commandType\":\"CANCEL\""), std::string::npos);
    EXPECT_NE(output.find("\"reason\":\"NOT_OWNER\""), std::string::npos);
    EXPECT_NE(output.find("\"commandType\":\"NEW_ORDER\""), std::string::npos);
    EXPECT_NE(output.find("\"reason\":\"BOOK_CAPACITY_EXCEEDED\""), std::string::npos);
}

TEST(ApplicationEventJsonTest, RejectsEmptyAndMixedPublicInputsWithoutChangingOutput) {
    std::string output{"unchanged"};
    EXPECT_EQ(serializePublicTradesV1(RUN_ID, {}, output), ApplicationEventJsonError::EMPTY_EVENTS);
    EXPECT_EQ(output, "unchanged");

    const std::vector<market_data::PublicTrade> wrongRun{{
        .eventId = eventId(0, domain::ExchangeRunId{10}),
        .instrumentId = domain::InstrumentId{3},
        .executionPrice = domain::Price{1},
        .executionQuantity = domain::Quantity{1},
        .aggressorSide = domain::Side::BUY,
    }};
    EXPECT_EQ(serializePublicTradesV1(RUN_ID, wrongRun, output), ApplicationEventJsonError::RUN_MISMATCH);
    EXPECT_EQ(output, "unchanged");

    const std::vector<market_data::PublicTrade> mixedSequence{
        wrongRun.front(),
        {
            .eventId = eventId(1, domain::ExchangeRunId{10}, domain::CommandSequence{18}),
            .instrumentId = domain::InstrumentId{3},
            .executionPrice = domain::Price{1},
            .executionQuantity = domain::Quantity{1},
            .aggressorSide = domain::Side::BUY,
        },
    };
    EXPECT_EQ(serializePublicTradesV1(domain::ExchangeRunId{10}, mixedSequence, output),
              ApplicationEventJsonError::COMMAND_SEQUENCE_MISMATCH);
    EXPECT_EQ(output, "unchanged");
}

TEST(ApplicationEventJsonTest, RejectsMalformedPrivateInputsWithoutChangingOutput) {
    std::string output{"unchanged"};
    auto empty = recipientResult({});
    EXPECT_EQ(serializePrivateResultV1(RUN_ID, empty, output), ApplicationEventJsonError::EMPTY_EVENTS);

    auto wrongRun = recipientResult(
        {privateTrade(private_result::TradeRole::TAKER, domain::Side::BUY, 0, domain::ExchangeRunId{10})});
    EXPECT_EQ(serializePrivateResultV1(RUN_ID, wrongRun, output), ApplicationEventJsonError::RUN_MISMATCH);

    auto mixedSequence = recipientResult(
        {privateTrade(private_result::TradeRole::MAKER),
         privateTrade(private_result::TradeRole::TAKER, domain::Side::BUY, 1, RUN_ID, domain::CommandSequence{18})});
    EXPECT_EQ(serializePrivateResultV1(RUN_ID, mixedSequence, output),
              ApplicationEventJsonError::COMMAND_SEQUENCE_MISMATCH);

    auto wrongCorrelation =
        recipientResult({privateTrade(private_result::TradeRole::TAKER)},
                        domain::CommandResultCorrelation{RECIPIENT, domain::ClientCommandId{"WRONG-RUN"},
                                                         COMMAND_SEQUENCE, domain::ExchangeRunId{10}});
    EXPECT_EQ(serializePrivateResultV1(RUN_ID, wrongCorrelation, output),
              ApplicationEventJsonError::CORRELATION_MISMATCH);

    auto wrongRecipient = recipientResult({domain::OrderRested{
        .eventId = eventId(0),
        .orderId = domain::OrderId{1},
        .clientId = domain::ClientId{99},
        .instrumentId = domain::InstrumentId{3},
        .side = domain::Side::BUY,
        .price = domain::Price{1},
        .remainingQuantity = domain::Quantity{1},
    }});
    EXPECT_EQ(serializePrivateResultV1(RUN_ID, wrongRecipient, output), ApplicationEventJsonError::RECIPIENT_MISMATCH);
    EXPECT_EQ(output, "unchanged");
}

TEST(ApplicationEventJsonTest, RejectsUnknownEnumsWithoutChangingOutput) {
    std::string output{"unchanged"};
    const std::vector<market_data::PublicTrade> trades{{
        .eventId = eventId(0),
        .instrumentId = domain::InstrumentId{3},
        .executionPrice = domain::Price{1},
        .executionQuantity = domain::Quantity{1},
        .aggressorSide = static_cast<domain::Side>(99),
    }};
    EXPECT_EQ(serializePublicTradesV1(RUN_ID, trades, output), ApplicationEventJsonError::INVALID_ENUM);
    EXPECT_EQ(output, "unchanged");
}

} // namespace
} // namespace exchange::application_event
