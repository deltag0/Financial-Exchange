#include "application_event_gateway.hpp"
#include "exchange_run_controller.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace exchange::application_event {
namespace {

constexpr domain::ExchangeRunId RUN_ID{1};
constexpr domain::ExchangeRunId OTHER_RUN_ID{2};
constexpr domain::ClientId MAKER_ID{11};
constexpr domain::ClientId TAKER_ID{22};
const std::size_t OUTBOUND_CAPACITY = MIN_APPLICATION_GATEWAY_OUTBOUND_CAPACITY_V1;

class ApplicationEventGatewayTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::array<char, 64> buffer{};
        constexpr char TEMPLATE[] = "/tmp/exchange-application-gateway-XXXXXX";
        std::copy(std::begin(TEMPLATE), std::end(TEMPLATE), buffer.begin());
        const char* created = ::mkdtemp(buffer.data());
        ASSERT_NE(created, nullptr);
        directory_ = created;
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
        EXPECT_FALSE(error);
    }

    void startController(const std::size_t privateCapacity = 8'192, const std::size_t publicCapacity = 4'096,
                         const std::uint64_t maxRunCommands = 100,
                         const std::uint64_t maxRunJournalBytes = 1024 * 1024) {
        const storage::NewRunConfigurationV1 configuration{
            .behavioralRulesVersion = 1,
            .maxEventsPerCommand = 4'096,
            .maxRunCommands = maxRunCommands,
            .maxRunJournalBytes = maxRunJournalBytes,
            .instruments = {{domain::InstrumentId{1}, 1}},
        };
        ASSERT_EQ(controller_.startupNewRunV1(directory_ / "run-catalog-v1", configuration).outcome,
                  core::NewRunStartupOutcome::READY);
        ASSERT_TRUE(controller_.installCommandProcessingV1(8, 8, 8, privateCapacity, publicCapacity));
    }

    static sequencer::sequenceMessage order(const std::string_view commandId, const domain::ClientId clientId,
                                            const bool buy, const std::uint64_t quantity = 1) {
        sequencer::sequenceMessage command{};
        command.clientId = clientId;
        command.clientCommandId = domain::ClientCommandId{commandId};
        command.instrumentId = domain::InstrumentId{1};
        command.configurationVersion = 1;
        command.type = buy ? sequencer::orderType::BUY : sequencer::orderType::SELL;
        command.price = domain::Price{100};
        command.quantity = domain::Quantity{quantity};
        command.tif = buy ? core::task::TimeInForce::IOC : core::task::TimeInForce::GTC;
        return command;
    }

    void process(const sequencer::sequenceMessage& command) {
        ASSERT_EQ(controller_.submitCommandV1(RUN_ID, command).status,
                  core::admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_TRUE(controller_.advanceCommandProcessingV1());
    }

    void processCross(const std::string_view suffix, const domain::ClientId maker = MAKER_ID,
                      const domain::ClientId taker = TAKER_ID) {
        process(order(std::string{"SELL-"} + std::string{suffix}, maker, false));
        process(order(std::string{"BUY-"} + std::string{suffix}, taker, true));
    }

    core::ExchangeRunController controller_;

private:
    std::filesystem::path directory_;
};

TEST_F(ApplicationEventGatewayTest, ConfigurationAndConnectionBoundsAreExplicit) {
    EXPECT_THROW(ApplicationEventGateway(controller_, 0, OUTBOUND_CAPACITY), std::invalid_argument);
    EXPECT_THROW(ApplicationEventGateway(controller_, 1, 0), std::invalid_argument);
    EXPECT_THROW(ApplicationEventGateway(controller_, 1, OUTBOUND_CAPACITY - 1), std::invalid_argument);

    ApplicationEventGateway gateway(controller_, 1, OUTBOUND_CAPACITY);
    EXPECT_EQ(gateway.registerConnection(ConnectionHandle{0}, MAKER_ID, RUN_ID).outcome,
              ConnectionRegistrationOutcome::INVALID_BINDING);

    startController();
    EXPECT_EQ(gateway.registerConnection(ConnectionHandle{1}, domain::ClientId{}, RUN_ID).outcome,
              ConnectionRegistrationOutcome::INVALID_BINDING);
    EXPECT_EQ(gateway.registerConnection(ConnectionHandle{1}, MAKER_ID, domain::ExchangeRunId{}).outcome,
              ConnectionRegistrationOutcome::INVALID_BINDING);
    EXPECT_EQ(gateway.registerConnection(ConnectionHandle{1}, MAKER_ID, OTHER_RUN_ID).outcome,
              ConnectionRegistrationOutcome::INVALID_BINDING);
    EXPECT_EQ(gateway.registerConnection(ConnectionHandle{0}, MAKER_ID, RUN_ID).outcome,
              ConnectionRegistrationOutcome::REGISTERED);
    EXPECT_EQ(gateway.registerConnection(ConnectionHandle{0}, TAKER_ID, RUN_ID).outcome,
              ConnectionRegistrationOutcome::HANDLE_ALREADY_REGISTERED);
    EXPECT_EQ(gateway.registerConnection(ConnectionHandle{2}, TAKER_ID, RUN_ID).outcome,
              ConnectionRegistrationOutcome::CONNECTION_LIMIT_REACHED);
    EXPECT_EQ(gateway.inspect().connectionCount, 1U);
}

TEST_F(ApplicationEventGatewayTest, ReplacementAndDisconnectReleaseQueuedOwnershipAndAccounting) {
    startController();
    process(order("REST", MAKER_ID, false));

    ApplicationEventGateway gateway(controller_, 2, OUTBOUND_CAPACITY);
    ASSERT_EQ(gateway.registerConnection(ConnectionHandle{1}, MAKER_ID, RUN_ID).outcome,
              ConnectionRegistrationOutcome::REGISTERED);
    ASSERT_EQ(gateway.advancePrivate(), ApplicationEventAdvanceOutcome::ROUTED);
    const auto queued = gateway.inspectConnection(ConnectionHandle{1});
    ASSERT_TRUE(queued.has_value());
    EXPECT_EQ(queued->queuedMessageCount, 1U);
    EXPECT_GT(queued->queuedBytes, 0U);

    const auto replacement = gateway.registerConnection(ConnectionHandle{2}, MAKER_ID, RUN_ID);
    EXPECT_EQ(replacement.outcome, ConnectionRegistrationOutcome::REPLACED);
    EXPECT_EQ(replacement.replacedConnection, ConnectionHandle{1});
    EXPECT_FALSE(gateway.inspectConnection(ConnectionHandle{1}).has_value());
    const auto emptyReplacement = gateway.inspectConnection(ConnectionHandle{2});
    ASSERT_TRUE(emptyReplacement.has_value());
    EXPECT_EQ(emptyReplacement->queuedMessageCount, 0U);
    EXPECT_EQ(emptyReplacement->queuedBytes, 0U);

    EXPECT_TRUE(gateway.disconnect(ConnectionHandle{2}));
    EXPECT_FALSE(gateway.disconnect(ConnectionHandle{2}));
    EXPECT_EQ(gateway.inspect().connectionCount, 0U);
}

TEST_F(ApplicationEventGatewayTest, PublicBroadcastUsesRunBindingAndOneSharedImmutablePayload) {
    startController();
    processCross("ONE");

    ApplicationEventGateway gateway(controller_, 2, OUTBOUND_CAPACITY);
    ASSERT_EQ(gateway.registerConnection(ConnectionHandle{1}, MAKER_ID, RUN_ID).outcome,
              ConnectionRegistrationOutcome::REGISTERED);
    ASSERT_EQ(gateway.registerConnection(ConnectionHandle{2}, TAKER_ID, RUN_ID).outcome,
              ConnectionRegistrationOutcome::REGISTERED);
    EXPECT_EQ(gateway.advancePublic(), ApplicationEventAdvanceOutcome::ROUTED);

    ImmutableApplicationMessage makerMessage;
    ImmutableApplicationMessage takerMessage;
    ASSERT_TRUE(gateway.tryPopOutbound(ConnectionHandle{1}, makerMessage));
    ASSERT_TRUE(gateway.tryPopOutbound(ConnectionHandle{2}, takerMessage));
    ASSERT_NE(makerMessage, nullptr);
    ASSERT_NE(takerMessage, nullptr);
    EXPECT_EQ(makerMessage.get(), takerMessage.get());
    EXPECT_EQ(*makerMessage, *takerMessage);
    EXPECT_FALSE(gateway.inspect().publicInput.failure.has_value());
}

TEST_F(ApplicationEventGatewayTest, MinimumCapacityEnqueuesAndPopMaintainsExactByteAccounting) {
    startController();
    processCross("FIT");

    const std::vector<market_data::PublicTrade> expectedTrades{
        {.eventId = {domain::CommandSequence{2}, domain::EventIndex{0}, RUN_ID},
         .instrumentId = domain::InstrumentId{1},
         .executionPrice = domain::Price{100},
         .executionQuantity = domain::Quantity{1},
         .aggressorSide = domain::Side::BUY},
    };
    std::string expected;
    ASSERT_EQ(serializePublicTradesV1(RUN_ID, expectedTrades, expected), ApplicationEventJsonError::NONE);

    ApplicationEventGateway gateway(controller_, 1, OUTBOUND_CAPACITY);
    ASSERT_EQ(gateway.registerConnection(ConnectionHandle{1}, MAKER_ID, RUN_ID).outcome,
              ConnectionRegistrationOutcome::REGISTERED);
    ASSERT_EQ(gateway.advancePublic(), ApplicationEventAdvanceOutcome::ROUTED);
    const auto full = gateway.inspectConnection(ConnectionHandle{1});
    ASSERT_TRUE(full.has_value());
    EXPECT_EQ(full->queuedMessageCount, 1U);
    EXPECT_EQ(full->queuedBytes, expected.size());

    ImmutableApplicationMessage output;
    ASSERT_TRUE(gateway.tryPopOutbound(ConnectionHandle{1}, output));
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(*output, expected);
    const auto empty = gateway.inspectConnection(ConnectionHandle{1});
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty->queuedMessageCount, 0U);
    EXPECT_EQ(empty->queuedBytes, 0U);
    const auto preserved = output;
    EXPECT_FALSE(gateway.tryPopOutbound(ConnectionHandle{1}, output));
    EXPECT_EQ(output, preserved);
}

TEST_F(ApplicationEventGatewayTest, InsufficientRemainingCapacityDisconnectsBeforeCurrentPrivateMessageEnqueue) {
    constexpr std::size_t RESTING_ORDERS_PER_MATCH = 4'096;
    constexpr std::size_t MATCH_COUNT = 2;
    constexpr std::uint64_t MAX_RUN_COMMANDS = MATCH_COUNT * RESTING_ORDERS_PER_MATCH + MATCH_COUNT + 1;
    startController(8'192, 4'096, MAX_RUN_COMMANDS, 8 * 1024 * 1024);

    ApplicationEventGateway gateway(controller_, 1, OUTBOUND_CAPACITY);
    for (std::size_t match = 0; match < MATCH_COUNT; ++match) {
        for (std::size_t index = 0; index < RESTING_ORDERS_PER_MATCH; ++index) {
            process(order("SELF-REST-" + std::to_string(match) + "-" + std::to_string(index), MAKER_ID, false));
            ASSERT_EQ(gateway.advancePrivate(), ApplicationEventAdvanceOutcome::ROUTED);
        }
    }

    ASSERT_EQ(gateway.registerConnection(ConnectionHandle{1}, MAKER_ID, RUN_ID).outcome,
              ConnectionRegistrationOutcome::REGISTERED);

    process(order("SELF-TAKER-1", MAKER_ID, true, RESTING_ORDERS_PER_MATCH));
    ASSERT_EQ(gateway.advancePrivate(), ApplicationEventAdvanceOutcome::ROUTED);
    const auto queued = gateway.inspectConnection(ConnectionHandle{1});
    ASSERT_TRUE(queued.has_value());
    ASSERT_EQ(queued->queuedMessageCount, 1U);
    ASSERT_GT(queued->queuedBytes, OUTBOUND_CAPACITY / 2);
    std::vector<market_data::PublicTrade> publicBatch;
    ASSERT_TRUE(controller_.tryPopPublicTradeBatchV1(publicBatch));
    ASSERT_EQ(publicBatch.size(), RESTING_ORDERS_PER_MATCH);

    process(order("SELF-TAKER-2", MAKER_ID, true, RESTING_ORDERS_PER_MATCH));
    ASSERT_EQ(gateway.advancePrivate(), ApplicationEventAdvanceOutcome::ROUTED);

    EXPECT_FALSE(gateway.inspectConnection(ConnectionHandle{1}).has_value());
    ImmutableApplicationMessage output;
    EXPECT_FALSE(gateway.tryPopOutbound(ConnectionHandle{1}, output));
}

TEST_F(ApplicationEventGatewayTest, PublicMessagesPreserveCommandFifoWithoutDuplicatePops) {
    startController();
    processCross("ONE");
    processCross("TWO", domain::ClientId{33}, domain::ClientId{44});

    ApplicationEventGateway gateway(controller_, 1, OUTBOUND_CAPACITY);
    ASSERT_EQ(gateway.registerConnection(ConnectionHandle{1}, MAKER_ID, RUN_ID).outcome,
              ConnectionRegistrationOutcome::REGISTERED);
    ASSERT_EQ(gateway.advancePublic(), ApplicationEventAdvanceOutcome::ROUTED);
    EXPECT_EQ(controller_.inspectCommandProcessingV1().queuedPublicTradeRecords, 1U);
    ASSERT_EQ(gateway.advancePublic(), ApplicationEventAdvanceOutcome::ROUTED);
    EXPECT_EQ(controller_.inspectCommandProcessingV1().queuedPublicTradeRecords, 0U);
    EXPECT_EQ(gateway.advancePublic(), ApplicationEventAdvanceOutcome::IDLE);

    ImmutableApplicationMessage first;
    ImmutableApplicationMessage second;
    ASSERT_TRUE(gateway.tryPopOutbound(ConnectionHandle{1}, first));
    ASSERT_TRUE(gateway.tryPopOutbound(ConnectionHandle{1}, second));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first->find("\"commandSequence\":\"2\""), std::string::npos);
    EXPECT_NE(second->find("\"commandSequence\":\"4\""), std::string::npos);
    EXPECT_FALSE(gateway.tryPopOutbound(ConnectionHandle{1}, second));
}

TEST_F(ApplicationEventGatewayTest, PrivateRoutingUsesExactRecipientAndRunWhileOfflineRecipientsAreValid) {
    startController();
    processCross("PRIVATE");

    ApplicationEventGateway gateway(controller_, 1, OUTBOUND_CAPACITY);
    ASSERT_EQ(gateway.registerConnection(ConnectionHandle{1}, MAKER_ID, RUN_ID).outcome,
              ConnectionRegistrationOutcome::REGISTERED);
    ASSERT_EQ(gateway.advancePrivate(), ApplicationEventAdvanceOutcome::ROUTED);
    ImmutableApplicationMessage makerRest;
    ASSERT_TRUE(gateway.tryPopOutbound(ConnectionHandle{1}, makerRest));
    ASSERT_NE(makerRest, nullptr);
    EXPECT_NE(makerRest->find("\"recipientClientId\":\"11\""), std::string::npos);

    ASSERT_EQ(gateway.advancePrivate(), ApplicationEventAdvanceOutcome::ROUTED);
    ImmutableApplicationMessage makerTrade;
    ASSERT_TRUE(gateway.tryPopOutbound(ConnectionHandle{1}, makerTrade));
    EXPECT_NE(makerTrade->find("\"role\":\"MAKER\""), std::string::npos);
    EXPECT_FALSE(gateway.inspect().privateInput.failure.has_value());
}

TEST_F(ApplicationEventGatewayTest, PrivateFanoutIsPreparedBeforeRoutingEveryEligibleRecipient) {
    startController();
    const auto maker = order("REST", MAKER_ID, false);
    const auto taker = order("BUY", TAKER_ID, true);
    const private_result::RecipientResult rested{
        .recipient = MAKER_ID,
        .correlation =
            domain::CommandResultCorrelation{MAKER_ID, *maker.clientCommandId, domain::CommandSequence{1}, RUN_ID},
        .privateResult = {domain::OrderRested{
            .eventId = {domain::CommandSequence{1}, domain::EventIndex{0}, RUN_ID},
            .orderId = domain::OrderId{1},
            .clientId = MAKER_ID,
            .instrumentId = domain::InstrumentId{1},
            .side = domain::Side::SELL,
            .price = domain::Price{100},
            .remainingQuantity = domain::Quantity{1},
        }},
    };
    const private_result::RecipientResult makerTrade{
        .recipient = MAKER_ID,
        .correlation = std::nullopt,
        .privateResult = {private_result::PrivateTrade{
            .eventId = {domain::CommandSequence{2}, domain::EventIndex{0}, RUN_ID},
            .instrumentId = domain::InstrumentId{1},
            .orderId = domain::OrderId{1},
            .side = domain::Side::SELL,
            .role = private_result::TradeRole::MAKER,
            .executionPrice = domain::Price{100},
            .executionQuantity = domain::Quantity{1},
            .remainingQuantity = domain::Quantity{0},
        }},
    };
    const private_result::RecipientResult takerTrade{
        .recipient = TAKER_ID,
        .correlation =
            domain::CommandResultCorrelation{TAKER_ID, *taker.clientCommandId, domain::CommandSequence{2}, RUN_ID},
        .privateResult = {private_result::PrivateTrade{
            .eventId = {domain::CommandSequence{2}, domain::EventIndex{0}, RUN_ID},
            .instrumentId = domain::InstrumentId{1},
            .orderId = domain::OrderId{2},
            .side = domain::Side::BUY,
            .role = private_result::TradeRole::TAKER,
            .executionPrice = domain::Price{100},
            .executionQuantity = domain::Quantity{1},
            .remainingQuantity = domain::Quantity{0},
        }},
    };
    std::string restedJson;
    std::string makerJson;
    std::string takerJson;
    ASSERT_EQ(serializePrivateResultV1(RUN_ID, rested, restedJson), ApplicationEventJsonError::NONE);
    ASSERT_EQ(serializePrivateResultV1(RUN_ID, makerTrade, makerJson), ApplicationEventJsonError::NONE);
    ASSERT_EQ(serializePrivateResultV1(RUN_ID, takerTrade, takerJson), ApplicationEventJsonError::NONE);
    ApplicationEventGateway gateway(controller_, 2, OUTBOUND_CAPACITY);
    ASSERT_EQ(gateway.registerConnection(ConnectionHandle{1}, MAKER_ID, RUN_ID).outcome,
              ConnectionRegistrationOutcome::REGISTERED);
    ASSERT_EQ(gateway.registerConnection(ConnectionHandle{2}, TAKER_ID, RUN_ID).outcome,
              ConnectionRegistrationOutcome::REGISTERED);
    process(maker);
    ASSERT_EQ(gateway.advancePrivate(), ApplicationEventAdvanceOutcome::ROUTED);
    const auto restInspection = gateway.inspectConnection(ConnectionHandle{1});
    ASSERT_TRUE(restInspection.has_value());
    EXPECT_EQ(restInspection->queuedBytes, restedJson.size());

    process(taker);
    ASSERT_EQ(gateway.advancePrivate(), ApplicationEventAdvanceOutcome::ROUTED);
    const auto makerDelivered = gateway.inspectConnection(ConnectionHandle{1});
    ASSERT_TRUE(makerDelivered.has_value());
    EXPECT_EQ(makerDelivered->queuedMessageCount, 2U);
    EXPECT_EQ(makerDelivered->queuedBytes, restedJson.size() + makerJson.size());
    const auto delivered = gateway.inspectConnection(ConnectionHandle{2});
    ASSERT_TRUE(delivered.has_value());
    EXPECT_EQ(delivered->queuedMessageCount, 1U);
    EXPECT_EQ(delivered->queuedBytes, takerJson.size());
    ImmutableApplicationMessage output;
    ASSERT_TRUE(gateway.tryPopOutbound(ConnectionHandle{1}, output));
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(*output, restedJson);
    ASSERT_TRUE(gateway.tryPopOutbound(ConnectionHandle{1}, output));
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(*output, makerJson);
    ASSERT_TRUE(gateway.tryPopOutbound(ConnectionHandle{2}, output));
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(*output, takerJson);
}

TEST_F(ApplicationEventGatewayTest, PublicAndPrivateInputsRetainIndependentlyAndEmptyDestinationsDrain) {
    startController();
    processCross("INDEPENDENT");

    ApplicationEventGateway gateway(controller_, 1, OUTBOUND_CAPACITY);
    EXPECT_EQ(gateway.advancePublic(), ApplicationEventAdvanceOutcome::ROUTED);
    EXPECT_EQ(controller_.inspectCommandProcessingV1().queuedPublicTradeRecords, 0U);
    EXPECT_EQ(controller_.inspectCommandProcessingV1().queuedPrivateEventRecords, 3U);
    auto inspection = gateway.inspect();
    EXPECT_FALSE(inspection.publicInput.failure.has_value());
    EXPECT_EQ(gateway.advancePrivate(), ApplicationEventAdvanceOutcome::ROUTED);
    EXPECT_EQ(controller_.inspectCommandProcessingV1().queuedPrivateEventRecords, 2U);
    EXPECT_EQ(gateway.advancePrivate(), ApplicationEventAdvanceOutcome::ROUTED);
    EXPECT_EQ(gateway.advancePrivate(), ApplicationEventAdvanceOutcome::IDLE);
    inspection = gateway.inspect();
    EXPECT_FALSE(inspection.privateInput.failure.has_value());
}

} // namespace
} // namespace exchange::application_event
