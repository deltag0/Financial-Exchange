#include "private_result_projection.hpp"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

namespace exchange::private_result {
namespace {

template <typename T>
concept LeaksCounterpartyOrCommandIdentity =
    requires(const T& value) { value.clientId; } || requires(const T& value) { value.makerClientId; } ||
    requires(const T& value) { value.takerClientId; } || requires(const T& value) { value.makerOrderId; } ||
    requires(const T& value) { value.takerOrderId; } || requires(const T& value) { value.makerRemainingQuantity; } ||
    requires(const T& value) { value.takerRemainingQuantity; } || requires(const T& value) { value.clientCommandId; };

static_assert(!LeaksCounterpartyOrCommandIdentity<PrivateTrade>);

constexpr domain::ExchangeRunId RUN_ID{9};
constexpr domain::CommandSequence COMMAND_SEQUENCE{17};

domain::EventId eventId(const std::uint32_t index) {
    return {
        .commandSequence = COMMAND_SEQUENCE,
        .eventIndex = domain::EventIndex{index},
        .exchangeRunId = RUN_ID,
    };
}

matching_engine::CommandResultBatch authoritativeBatch(std::vector<domain::BusinessEvent> events,
                                                       const domain::ClientId originatingClient = domain::ClientId{
                                                           99}) {
    return {
        domain::CommandResultCorrelation{
            originatingClient,
            domain::ClientCommandId{"CORRELATION-MUST-NOT-ESCAPE"},
            COMMAND_SEQUENCE,
            RUN_ID,
        },
        matching_engine::ProcessingResult::APPLIED,
        std::move(events),
    };
}

domain::Trade trade(const domain::ClientId maker, const domain::ClientId taker,
                    const domain::EventIndex eventIndex = domain::EventIndex{0},
                    const domain::OrderId makerOrderId = domain::OrderId{101},
                    const domain::OrderId takerOrderId = domain::OrderId{202},
                    const domain::Quantity makerRemaining = domain::Quantity{4},
                    const domain::Quantity takerRemaining = domain::Quantity{0}) {
    return {
        .eventId = eventId(eventIndex.value()),
        .instrumentId = domain::InstrumentId{3},
        .makerOrderId = makerOrderId,
        .makerClientId = maker,
        .takerOrderId = takerOrderId,
        .takerClientId = taker,
        .takerSide = domain::Side::BUY,
        .executionPrice = domain::Price{1'250},
        .executionQuantity = domain::Quantity{7},
        .makerRemainingQuantity = makerRemaining,
        .takerRemainingQuantity = takerRemaining,
    };
}

domain::EventId privateEventId(const PrivateEvent& event) {
    return std::visit([](const auto& typedEvent) { return typedEvent.eventId; }, event);
}

TEST(PrivateResultProjectionTest, ProjectsExactMakerAndTakerViewsAndNothingForUnrelatedClient) {
    const domain::ClientId maker{11};
    const domain::ClientId taker{22};
    const auto source = authoritativeBatch({trade(maker, taker)});

    const auto makerView = projectPrivateResult(source, maker);
    const auto takerView = projectPrivateResult(source, taker);
    const auto unrelatedView = projectPrivateResult(source, domain::ClientId{33});

    ASSERT_TRUE(makerView.has_value());
    ASSERT_TRUE(takerView.has_value());
    EXPECT_FALSE(unrelatedView.has_value());
    ASSERT_EQ(makerView->privateResult.size(), 1U);
    ASSERT_EQ(takerView->privateResult.size(), 1U);
    EXPECT_FALSE(makerView->correlation.has_value());
    EXPECT_FALSE(takerView->correlation.has_value());
    const auto* makerTrade = std::get_if<PrivateTrade>(&makerView->privateResult[0]);
    const auto* takerTrade = std::get_if<PrivateTrade>(&takerView->privateResult[0]);
    ASSERT_NE(makerTrade, nullptr);
    ASSERT_NE(takerTrade, nullptr);
    EXPECT_EQ(*makerTrade, (PrivateTrade{
                               .eventId = eventId(0),
                               .instrumentId = domain::InstrumentId{3},
                               .orderId = domain::OrderId{101},
                               .side = domain::Side::SELL,
                               .role = TradeRole::MAKER,
                               .executionPrice = domain::Price{1'250},
                               .executionQuantity = domain::Quantity{7},
                               .remainingQuantity = domain::Quantity{4},
                           }));
    EXPECT_EQ(*takerTrade, (PrivateTrade{
                               .eventId = eventId(0),
                               .instrumentId = domain::InstrumentId{3},
                               .orderId = domain::OrderId{202},
                               .side = domain::Side::BUY,
                               .role = TradeRole::TAKER,
                               .executionPrice = domain::Price{1'250},
                               .executionQuantity = domain::Quantity{7},
                               .remainingQuantity = domain::Quantity{0},
                           }));
}

TEST(PrivateResultProjectionTest, SelfTradeExpandsMakerThenTakerWithBothOwnedOrders) {
    const domain::ClientId recipient{44};
    const auto source = authoritativeBatch({trade(recipient, recipient)});

    const auto projected = projectPrivateResult(source, recipient);

    ASSERT_TRUE(projected.has_value());
    ASSERT_EQ(projected->privateResult.size(), 2U);
    const auto* maker = std::get_if<PrivateTrade>(&projected->privateResult[0]);
    const auto* taker = std::get_if<PrivateTrade>(&projected->privateResult[1]);
    ASSERT_NE(maker, nullptr);
    ASSERT_NE(taker, nullptr);
    EXPECT_EQ(maker->eventId, eventId(0));
    EXPECT_EQ(maker->orderId, domain::OrderId{101});
    EXPECT_EQ(maker->side, domain::Side::SELL);
    EXPECT_EQ(maker->role, TradeRole::MAKER);
    EXPECT_EQ(maker->remainingQuantity, domain::Quantity{4});
    EXPECT_EQ(taker->eventId, eventId(0));
    EXPECT_EQ(taker->orderId, domain::OrderId{202});
    EXPECT_EQ(taker->side, domain::Side::BUY);
    EXPECT_EQ(taker->role, TradeRole::TAKER);
    EXPECT_EQ(taker->remainingQuantity, domain::Quantity{0});
}

TEST(PrivateResultProjectionTest, FiltersPrivateEventsAndPreservesMixedAuthoritativeOrderAndIdentity) {
    const domain::ClientId recipient{55};
    const domain::ClientId other{66};
    std::vector<domain::BusinessEvent> events;
    events.emplace_back(trade(recipient, other, domain::EventIndex{0}));
    events.emplace_back(domain::OrderRested{
        .eventId = eventId(1),
        .orderId = domain::OrderId{303},
        .clientId = other,
        .instrumentId = domain::InstrumentId{3},
        .side = domain::Side::BUY,
        .price = domain::Price{1'240},
        .remainingQuantity = domain::Quantity{8},
    });
    events.emplace_back(domain::OrderRested{
        .eventId = eventId(2),
        .orderId = domain::OrderId{404},
        .clientId = recipient,
        .instrumentId = domain::InstrumentId{3},
        .side = domain::Side::SELL,
        .price = domain::Price{1'260},
        .remainingQuantity = domain::Quantity{5},
    });
    events.emplace_back(trade(recipient, recipient, domain::EventIndex{3}));
    events.emplace_back(domain::OrderCancelled{
        .eventId = eventId(4),
        .orderId = domain::OrderId{505},
        .clientId = other,
        .instrumentId = domain::InstrumentId{3},
        .cancelledQuantity = domain::Quantity{9},
        .reason = domain::CancelReason::CLIENT_REQUESTED,
    });
    events.emplace_back(domain::OrderCancelled{
        .eventId = eventId(5),
        .orderId = domain::OrderId{505},
        .clientId = recipient,
        .instrumentId = domain::InstrumentId{3},
        .cancelledQuantity = domain::Quantity{2},
        .reason = domain::CancelReason::IOC_REMAINDER,
    });
    events.emplace_back(domain::CommandRejected{
        .eventId = eventId(6),
        .commandType = domain::CommandType::CANCEL,
        .clientId = other,
        .clientCommandId = domain::ClientCommandId{"OTHER-REJECTION"},
        .relevantOrderId = domain::OrderId{606},
        .reason = domain::CommandRejectionReason::NOT_OWNER,
    });
    const domain::CommandRejected recipientRejection{
        .eventId = eventId(7),
        .commandType = domain::CommandType::CANCEL,
        .clientId = recipient,
        .clientCommandId = domain::ClientCommandId{"OWN-REJECTION"},
        .relevantOrderId = domain::OrderId{707},
        .reason = domain::CommandRejectionReason::ORDER_NOT_ACTIVE,
    };
    events.emplace_back(recipientRejection);
    const auto source = authoritativeBatch(std::move(events));

    const auto projected = projectPrivateResult(source, recipient);

    ASSERT_TRUE(projected.has_value());
    ASSERT_EQ(projected->privateResult.size(), 6U);
    const auto* firstTrade = std::get_if<PrivateTrade>(&projected->privateResult[0]);
    const auto* rested = std::get_if<domain::OrderRested>(&projected->privateResult[1]);
    const auto* selfMaker = std::get_if<PrivateTrade>(&projected->privateResult[2]);
    const auto* selfTaker = std::get_if<PrivateTrade>(&projected->privateResult[3]);
    const auto* cancelled = std::get_if<domain::OrderCancelled>(&projected->privateResult[4]);
    const auto* rejected = std::get_if<domain::CommandRejected>(&projected->privateResult[5]);
    ASSERT_NE(firstTrade, nullptr);
    ASSERT_NE(rested, nullptr);
    ASSERT_NE(selfMaker, nullptr);
    ASSERT_NE(selfTaker, nullptr);
    ASSERT_NE(cancelled, nullptr);
    ASSERT_NE(rejected, nullptr);
    EXPECT_EQ(firstTrade->eventId, eventId(0));
    EXPECT_EQ(*rested, (domain::OrderRested{
                           .eventId = eventId(2),
                           .orderId = domain::OrderId{404},
                           .clientId = recipient,
                           .instrumentId = domain::InstrumentId{3},
                           .side = domain::Side::SELL,
                           .price = domain::Price{1'260},
                           .remainingQuantity = domain::Quantity{5},
                       }));
    EXPECT_EQ(selfMaker->eventId, eventId(3));
    EXPECT_EQ(selfMaker->role, TradeRole::MAKER);
    EXPECT_EQ(selfTaker->eventId, eventId(3));
    EXPECT_EQ(selfTaker->role, TradeRole::TAKER);
    EXPECT_EQ(*cancelled, (domain::OrderCancelled{
                              .eventId = eventId(5),
                              .orderId = domain::OrderId{505},
                              .clientId = recipient,
                              .instrumentId = domain::InstrumentId{3},
                              .cancelledQuantity = domain::Quantity{2},
                              .reason = domain::CancelReason::IOC_REMAINDER,
                          }));
    EXPECT_EQ(*rejected, recipientRejection);
}

TEST(PrivateResultFanOutTest, DistinctMakerAndTakerReceiveOneSafeResultAndOnlyOriginGetsCorrelation) {
    const domain::ClientId maker{11};
    const domain::ClientId taker{22};
    const auto source = authoritativeBatch({trade(maker, taker)}, taker);
    const auto originalCorrelation = source.correlation();
    const auto originalEvents = source.events();

    const auto projected = projectPrivateResults(source);

    ASSERT_EQ(projected.size(), 2U);
    EXPECT_EQ(projected[0].recipient, maker);
    EXPECT_FALSE(projected[0].correlation.has_value());
    EXPECT_FALSE(projected[0].privateResult.empty());
    const auto makerProjection = projectPrivateResult(source, maker);
    ASSERT_TRUE(makerProjection.has_value());
    EXPECT_EQ(projected[0], *makerProjection);
    EXPECT_EQ(projected[1].recipient, taker);
    ASSERT_TRUE(projected[1].correlation.has_value());
    EXPECT_FALSE(projected[1].privateResult.empty());
    EXPECT_EQ(*projected[1].correlation, source.correlation());
    const auto takerProjection = projectPrivateResult(source, taker);
    ASSERT_TRUE(takerProjection.has_value());
    EXPECT_EQ(projected[1], *takerProjection);
    EXPECT_EQ(source.correlation(), originalCorrelation);
    EXPECT_EQ(source.events(), originalEvents);
}

TEST(PrivateResultFanOutTest, CoalescesSeveralMakersAndTakerTerminalEventsInFirstAppearanceOrder) {
    const domain::ClientId firstMaker{11};
    const domain::ClientId taker{22};
    const domain::ClientId secondMaker{33};
    std::vector<domain::BusinessEvent> events;
    events.emplace_back(trade(firstMaker, taker, domain::EventIndex{0}, domain::OrderId{101}, domain::OrderId{900},
                              domain::Quantity{0}, domain::Quantity{13}));
    events.emplace_back(trade(secondMaker, taker, domain::EventIndex{1}, domain::OrderId{303}, domain::OrderId{900},
                              domain::Quantity{0}, domain::Quantity{6}));
    events.emplace_back(trade(firstMaker, taker, domain::EventIndex{2}, domain::OrderId{404}, domain::OrderId{900},
                              domain::Quantity{0}, domain::Quantity{2}));
    events.emplace_back(domain::OrderCancelled{
        .eventId = eventId(3),
        .orderId = domain::OrderId{900},
        .clientId = taker,
        .instrumentId = domain::InstrumentId{3},
        .cancelledQuantity = domain::Quantity{2},
        .reason = domain::CancelReason::IOC_REMAINDER,
    });
    const auto source = authoritativeBatch(std::move(events), taker);

    const auto projected = projectPrivateResults(source);

    ASSERT_EQ(projected.size(), 3U);
    EXPECT_EQ(projected[0].recipient, firstMaker);
    EXPECT_EQ(projected[1].recipient, taker);
    EXPECT_EQ(projected[2].recipient, secondMaker);
    for (const auto& result : projected) {
        EXPECT_FALSE(result.privateResult.empty());
        const auto explicitResult = projectPrivateResult(source, result.recipient);
        ASSERT_TRUE(explicitResult.has_value());
        EXPECT_EQ(result, *explicitResult);
        EXPECT_EQ(result.correlation.has_value(), result.recipient == taker);
    }
    ASSERT_EQ(projected[0].privateResult.size(), 2U);
    EXPECT_EQ(privateEventId(projected[0].privateResult[0]), eventId(0));
    EXPECT_EQ(privateEventId(projected[0].privateResult[1]), eventId(2));
    ASSERT_EQ(projected[1].privateResult.size(), 4U);
    EXPECT_EQ(privateEventId(projected[1].privateResult[0]), eventId(0));
    EXPECT_EQ(privateEventId(projected[1].privateResult[1]), eventId(1));
    EXPECT_EQ(privateEventId(projected[1].privateResult[2]), eventId(2));
    EXPECT_EQ(privateEventId(projected[1].privateResult[3]), eventId(3));
    ASSERT_EQ(projected[2].privateResult.size(), 1U);
    EXPECT_EQ(privateEventId(projected[2].privateResult[0]), eventId(1));
}

TEST(PrivateResultFanOutTest, SelfTradeProducesOneCorrelatedResultWithMakerThenTakerViews) {
    const domain::ClientId recipient{44};
    const auto source = authoritativeBatch({trade(recipient, recipient)}, recipient);

    const auto projected = projectPrivateResults(source);

    ASSERT_EQ(projected.size(), 1U);
    EXPECT_EQ(projected[0].recipient, recipient);
    ASSERT_TRUE(projected[0].correlation.has_value());
    EXPECT_EQ(*projected[0].correlation, source.correlation());
    const auto explicitResult = projectPrivateResult(source, recipient);
    ASSERT_TRUE(explicitResult.has_value());
    EXPECT_EQ(projected[0], *explicitResult);
    ASSERT_EQ(projected[0].privateResult.size(), 2U);
    const auto* maker = std::get_if<PrivateTrade>(&projected[0].privateResult[0]);
    const auto* taker = std::get_if<PrivateTrade>(&projected[0].privateResult[1]);
    ASSERT_NE(maker, nullptr);
    ASSERT_NE(taker, nullptr);
    EXPECT_EQ(maker->role, TradeRole::MAKER);
    EXPECT_EQ(taker->role, TradeRole::TAKER);
}

TEST(PrivateResultFanOutTest, EmptyAuthoritativeResultProducesNoRecipientEnvelope) {
    const auto source = authoritativeBatch({});

    EXPECT_TRUE(projectPrivateResults(source).empty());
    EXPECT_FALSE(projectPrivateResult(source, domain::ClientId{99}).has_value());
}

TEST(PrivateResultFanOutTest, ProjectionFailureLeavesTheAuthoritativeSourceUnchanged) {
    const domain::Trade invalid{
        .eventId = eventId(0),
        .instrumentId = domain::InstrumentId{3},
        .makerOrderId = domain::OrderId{101},
        .makerClientId = domain::ClientId{11},
        .takerOrderId = domain::OrderId{202},
        .takerClientId = domain::ClientId{22},
        .takerSide = static_cast<domain::Side>(99),
        .executionPrice = domain::Price{1'250},
        .executionQuantity = domain::Quantity{7},
        .makerRemainingQuantity = domain::Quantity{0},
        .takerRemainingQuantity = domain::Quantity{0},
    };
    const auto source = authoritativeBatch({invalid}, domain::ClientId{22});
    const auto originalCorrelation = source.correlation();
    const auto originalEvents = source.events();

    EXPECT_THROW((void)projectPrivateResults(source), std::logic_error);
    EXPECT_EQ(source.correlation(), originalCorrelation);
    EXPECT_EQ(source.events(), originalEvents);
}

} // namespace
} // namespace exchange::private_result
