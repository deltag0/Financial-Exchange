#include "public_trade_projection.hpp"

#include "command_result.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace exchange::market_data {
namespace {

template <typename T>
concept LeaksPrivateOrAuthoritativeData =
    requires(const T& value) { value.clientId; } || requires(const T& value) { value.clientCommandId; } ||
    requires(const T& value) { value.orderId; } || requires(const T& value) { value.makerOrderId; } ||
    requires(const T& value) { value.takerOrderId; } || requires(const T& value) { value.makerClientId; } ||
    requires(const T& value) { value.takerClientId; } || requires(const T& value) { value.remainingQuantity; } ||
    requires(const T& value) { value.makerRemainingQuantity; } ||
    requires(const T& value) { value.takerRemainingQuantity; } || requires(const T& value) { value.correlation; } ||
    requires(const T& value) { value.authoritativeResult; };

static_assert(!LeaksPrivateOrAuthoritativeData<PublicTrade>);
static_assert(!std::is_constructible_v<PublicTrade, domain::Trade>);

constexpr domain::ExchangeRunId RUN_ID{7};
constexpr domain::CommandSequence COMMAND_SEQUENCE{12};

domain::EventId eventId(const std::uint32_t index) {
    return {
        .commandSequence = COMMAND_SEQUENCE,
        .eventIndex = domain::EventIndex{index},
        .exchangeRunId = RUN_ID,
    };
}

domain::Trade trade(const std::uint32_t index, const domain::ClientId maker, const domain::ClientId taker,
                    const domain::Side aggressorSide, const domain::Price price, const domain::Quantity quantity) {
    return {
        .eventId = eventId(index),
        .instrumentId = domain::InstrumentId{3},
        .makerOrderId = domain::OrderId{100 + index},
        .makerClientId = maker,
        .takerOrderId = domain::OrderId{200 + index},
        .takerClientId = taker,
        .takerSide = aggressorSide,
        .executionPrice = price,
        .executionQuantity = quantity,
        .makerRemainingQuantity = domain::Quantity{8},
        .takerRemainingQuantity = domain::Quantity{2},
    };
}

matching_engine::CommandResultBatch authoritativeBatch(std::vector<domain::BusinessEvent> events) {
    return {
        domain::CommandResultCorrelation{
            domain::ClientId{22},
            domain::ClientCommandId{"PUBLIC-PROJECTION"},
            COMMAND_SEQUENCE,
            RUN_ID,
        },
        matching_engine::ProcessingResult::APPLIED,
        std::move(events),
    };
}

TEST(PublicTradeProjectionTest, FiltersMixedBatchAndPreservesEveryTradeFieldAndSourceOrder) {
    std::vector<domain::BusinessEvent> events;
    events.emplace_back(domain::OrderRested{
        .eventId = eventId(0),
        .orderId = domain::OrderId{50},
        .clientId = domain::ClientId{11},
        .instrumentId = domain::InstrumentId{3},
        .side = domain::Side::SELL,
        .price = domain::Price{1'300},
        .remainingQuantity = domain::Quantity{8},
    });
    events.emplace_back(trade(1, domain::ClientId{11}, domain::ClientId{22}, domain::Side::BUY, domain::Price{1'300},
                              domain::Quantity{4}));
    events.emplace_back(domain::OrderCancelled{
        .eventId = eventId(2),
        .orderId = domain::OrderId{51},
        .clientId = domain::ClientId{33},
        .instrumentId = domain::InstrumentId{3},
        .cancelledQuantity = domain::Quantity{3},
        .reason = domain::CancelReason::IOC_REMAINDER,
    });
    events.emplace_back(trade(3, domain::ClientId{44}, domain::ClientId{55}, domain::Side::SELL, domain::Price{1'290},
                              domain::Quantity{7}));
    const auto source = authoritativeBatch(std::move(events));
    const auto originalCorrelation = source.correlation();
    const auto originalEvents = source.events();

    const auto projected = projectPublicTrades(source);

    EXPECT_EQ(projected, (std::vector<PublicTrade>{
                             PublicTrade{
                                 .eventId = eventId(1),
                                 .instrumentId = domain::InstrumentId{3},
                                 .executionPrice = domain::Price{1'300},
                                 .executionQuantity = domain::Quantity{4},
                                 .aggressorSide = domain::Side::BUY,
                             },
                             PublicTrade{
                                 .eventId = eventId(3),
                                 .instrumentId = domain::InstrumentId{3},
                                 .executionPrice = domain::Price{1'290},
                                 .executionQuantity = domain::Quantity{7},
                                 .aggressorSide = domain::Side::SELL,
                             },
                         }));
    EXPECT_EQ(source.correlation(), originalCorrelation);
    EXPECT_EQ(source.events(), originalEvents);
}

TEST(PublicTradeProjectionTest, NonTradeOnlyBatchProducesNoPublicValue) {
    const auto source = authoritativeBatch({domain::CommandRejected{
        .eventId = eventId(0),
        .commandType = domain::CommandType::CANCEL,
        .clientId = domain::ClientId{22},
        .clientCommandId = domain::ClientCommandId{"PRIVATE-REJECTION"},
        .relevantOrderId = domain::OrderId{9},
        .reason = domain::CommandRejectionReason::ORDER_NOT_ACTIVE,
    }});

    EXPECT_TRUE(projectPublicTrades(source).empty());
}

TEST(PublicTradeProjectionTest, SelfTradeProducesOneSanitizedTrade) {
    const domain::ClientId client{22};
    const auto source =
        authoritativeBatch({trade(0, client, client, domain::Side::BUY, domain::Price{1'250}, domain::Quantity{5})});

    const auto projected = projectPublicTrades(source);

    ASSERT_EQ(projected.size(), 1U);
    EXPECT_EQ(projected[0], (PublicTrade{
                                .eventId = eventId(0),
                                .instrumentId = domain::InstrumentId{3},
                                .executionPrice = domain::Price{1'250},
                                .executionQuantity = domain::Quantity{5},
                                .aggressorSide = domain::Side::BUY,
                            }));
}

} // namespace
} // namespace exchange::market_data
