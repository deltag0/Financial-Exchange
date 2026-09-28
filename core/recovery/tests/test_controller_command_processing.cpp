#include "exchange_run_controller.hpp"

#include "journal_run_header_codec.hpp"
#include "new_run_journal_internal.hpp"
#include "run_journal_reader.hpp"
#include "run_journal_writer_internal.hpp"
#include "run_catalog_storage_internal.hpp"
#include "start_new_run_internal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unistd.h>
#include <variant>

namespace exchange::core {
namespace {

template <typename T>
concept ExposesAuthoritativeBatch = requires(const T& value) { value.completedResult; };

static_assert(!std::is_constructible_v<private_result::PrivateEvent, domain::Trade>);
static_assert(!ExposesAuthoritativeBatch<PrivateResultLookupResult>);

class ControllerCommandProcessingTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::array<char, 64> buffer{};
        constexpr char TEMPLATE[] = "/tmp/exchange-controller-processing-XXXXXX";
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
    std::filesystem::path catalog() const {
        return directory_ / "run-catalog-v1";
    }
    std::filesystem::path journal() const {
        return directory_ / "run-1.fxjr";
    }

private:
    std::filesystem::path directory_;
};

storage::NewRunConfigurationV1 configuration() {
    return {.behavioralRulesVersion = 1,
            .maxEventsPerCommand = 4'096,
            .maxRunCommands = 10,
            .maxRunJournalBytes = 64 * 1024,
            .instruments = {{domain::InstrumentId{1}, 1}}};
}

storage::NewRunConfigurationV1 configuration(const std::uint64_t maxCommands, const std::uint64_t maxBytes) {
    auto result = configuration();
    result.maxRunCommands = maxCommands;
    result.maxRunJournalBytes = maxBytes;
    return result;
}

sequencer::sequenceMessage order(const std::string_view id = "SELL", const bool buy = false,
                                 const std::uint64_t quantity = 10) {
    sequencer::sequenceMessage command{};
    command.clientId = domain::ClientId{buy ? 22U : 11U};
    command.clientCommandId = domain::ClientCommandId{id};
    command.instrumentId = domain::InstrumentId{1};
    command.configurationVersion = 1;
    command.type = buy ? sequencer::orderType::BUY : sequencer::orderType::SELL;
    command.price = domain::Price{100};
    command.quantity = domain::Quantity{quantity};
    command.tif = buy ? task::TimeInForce::IOC : task::TimeInForce::GTC;
    return command;
}

std::size_t encodedHeaderSize(const storage::NewRunConfigurationV1& runConfiguration) {
    storage::RunHeaderV1 header{.exchangeRunId = domain::ExchangeRunId{1},
                                .behavioralRulesVersion = runConfiguration.behavioralRulesVersion,
                                .maxEventsPerCommand = runConfiguration.maxEventsPerCommand,
                                .maxRunCommands = runConfiguration.maxRunCommands,
                                .maxRunJournalBytes = 64 * 1024,
                                .instruments = runConfiguration.instruments};
    std::vector<std::byte> encoded;
    EXPECT_EQ(storage::encodeRunHeaderV1(header, encoded), storage::RunHeaderCodecError::NONE);
    return encoded.size();
}

std::size_t encodedOrderSize(const sequencer::sequenceMessage& command, const std::uint64_t sequence = 1) {
    storage::JournalNewOrderV1 journalCommand{
        .exchangeRunId = domain::ExchangeRunId{1},
        .commandSequence = domain::CommandSequence{sequence},
        .behavioralRulesVersion = 1,
        .configurationVersion = static_cast<std::uint32_t>(command.configurationVersion),
        .clientId = command.clientId,
        .instrumentId = command.instrumentId,
        .clientCommandId = *command.clientCommandId,
        .side = command.type == sequencer::orderType::BUY ? domain::Side::BUY : domain::Side::SELL,
        .timeInForce = command.tif,
        .price = command.price,
        .quantity = command.quantity,
    };
    std::vector<std::byte> encoded;
    EXPECT_EQ(storage::encodeNewOrderV1(journalCommand, encoded), storage::JournalCommandCodecError::NONE);
    return encoded.size();
}

void expectUnavailable(const admission::AdmissionDecision& decision) {
    EXPECT_EQ(decision.status, admission::AdmissionStatus::ADMISSION_UNAVAILABLE);
    EXPECT_EQ(decision.rejectionReason, domain::AdmissionRejectionReason::EXCHANGE_RUN_UNAVAILABLE);
}

void expectCapacityReached(const admission::AdmissionDecision& decision) {
    EXPECT_EQ(decision.status, admission::AdmissionStatus::ADMISSION_UNAVAILABLE);
    EXPECT_EQ(decision.rejectionReason, domain::AdmissionRejectionReason::RUN_CAPACITY_REACHED);
}

void expectCorrelation(const matching_engine::ImmutableCommandResultBatch& batch,
                       const sequencer::sequenceMessage& command, const std::uint64_t sequence,
                       const domain::ExchangeRunId exchangeRunId = domain::ExchangeRunId{1}) {
    ASSERT_NE(batch, nullptr);
    EXPECT_EQ(batch->correlation(),
              (domain::CommandResultCorrelation{command.clientId, *command.clientCommandId,
                                                domain::CommandSequence{sequence}, exchangeRunId}));
    for (std::size_t index = 0; index < batch->events().size(); ++index) {
        EXPECT_EQ(
            std::visit([](const auto& event) { return event.eventId; }, batch->events()[index]),
            (domain::EventId{domain::CommandSequence{sequence},
                             domain::EventIndex{static_cast<domain::EventIndex::Underlying>(index)}, exchangeRunId}));
    }
}

void drainPrivateResultHandoff(ExchangeRunController& controller) {
    private_result::RecipientResults batch;
    while (controller.tryPopPrivateResultBatchV1(batch)) {
        batch.clear();
    }
}

TEST_F(ControllerCommandProcessingTest, InstallationIsBoundedOneTimeAndPreservesRunOwners) {
    ExchangeRunController controller;
    EXPECT_FALSE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    EXPECT_FALSE(controller.advanceCommandProcessingV1());
    EXPECT_FALSE(controller.inspectCommandProcessingV1().installed);
    expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, order()));
    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    const auto* state = controller.matchingState();
    const auto* writer = controller.journalWriter();
    const auto* admission = controller.admissionIndex();
    EXPECT_THROW((void)controller.installCommandProcessingV1(0, 1, 1, 16'384, 4'096), std::invalid_argument);
    EXPECT_THROW((void)controller.installCommandProcessingV1(1, 65'535, 1, 16'384, 4'096), std::invalid_argument);
    EXPECT_THROW((void)controller.installCommandProcessingV1(1, 1, 0, 16'384, 4'096), std::invalid_argument);
    EXPECT_THROW((void)controller.installCommandProcessingV1(1, 1, 1, 0, 4'096), std::invalid_argument);
    EXPECT_THROW((void)controller.installCommandProcessingV1(1, 1, 1, 8'191, 4'096), std::invalid_argument);
    EXPECT_THROW((void)controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'095), std::invalid_argument);
    EXPECT_FALSE(controller.inspectCommandProcessingV1().installed);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 8'192, 4'096));
    EXPECT_FALSE(controller.installCommandProcessingV1(2, 2, 2, 16'384, 4'096));
    EXPECT_EQ(controller.matchingState(), state);
    EXPECT_EQ(controller.journalWriter(), writer);
    EXPECT_EQ(controller.admissionIndex(), admission);
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::READY);
    EXPECT_FALSE(controller.advanceCommandProcessingV1());
}

TEST_F(ControllerCommandProcessingTest, NewRunCompletesDurableOrderAndCancelAndSuppressesDuplicates) {
    ExchangeRunController controller;
    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    const auto sell = order();
    expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{2}, sell));
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, sell).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, sell).status,
              admission::AdmissionStatus::IDENTICAL_IN_FLIGHT);
    auto conflicting = sell;
    conflicting.quantity = domain::Quantity{11};
    EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, conflicting).status,
              admission::AdmissionStatus::CONFLICTING_REUSE);
    const auto next = order("NEXT");
    const auto refused = controller.submitCommandV1(domain::ExchangeRunId{1}, next);
    EXPECT_EQ(refused.status, admission::AdmissionStatus::ADMISSION_UNAVAILABLE);
    EXPECT_EQ(refused.rejectionReason, domain::AdmissionRejectionReason::GATEWAY_BUSY);
    EXPECT_EQ(controller.admissionIndex()->size(), 1U);
    EXPECT_EQ(controller.admissionIndex()->statistics().firstSubmissions, 1U);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    const auto original = controller.admissionIndex()->completedResult(sell);
    expectCorrelation(original, sell, 1);
    ASSERT_NE(original, nullptr);
    ASSERT_EQ(original->events().size(), 1U);
    ASSERT_NE(std::get_if<domain::OrderRested>(&original->events()[0]), nullptr);
    EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, sell).originalResult, original);

    auto cancel = sell;
    cancel.clientCommandId = domain::ClientCommandId{"CANCEL"};
    cancel.type = sequencer::orderType::CANCEL;
    cancel.price = {};
    cancel.quantity = {};
    cancel.targetOrderId = domain::TargetOrderId{1};
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, cancel).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    const auto cancelled = controller.admissionIndex()->completedResult(cancel);
    expectCorrelation(cancelled, cancel, 2);
    ASSERT_NE(cancelled, nullptr);
    ASSERT_EQ(cancelled->events().size(), 1U);
    ASSERT_NE(std::get_if<domain::OrderCancelled>(&cancelled->events()[0]), nullptr);
    EXPECT_TRUE(controller.matchingState()->snapshot().activeOrders.empty());
    EXPECT_TRUE(controller.matchingState()->invariantsHold());
    EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, cancel).originalResult, cancelled);
    EXPECT_FALSE(controller.advanceCommandProcessingV1());
    // Retry the formerly refused key after space returns; refusal consumed no sequence.
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, next).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    expectCorrelation(controller.admissionIndex()->completedResult(next), next, 3);

    storage::LoadedRunJournalV1 loaded;
    ASSERT_EQ(storage::loadRunJournalV1(journal(), loaded).outcome, storage::RunJournalLoadOutcome::VALID);
    ASSERT_EQ(loaded.commands.size(), 3U);
    EXPECT_EQ(loaded.header.exchangeRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(std::get<storage::JournalNewOrderV1>(loaded.commands[0]),
              (storage::JournalNewOrderV1{.exchangeRunId = domain::ExchangeRunId{1},
                                          .commandSequence = domain::CommandSequence{1},
                                          .behavioralRulesVersion = 1,
                                          .configurationVersion = 1,
                                          .clientId = domain::ClientId{11},
                                          .instrumentId = domain::InstrumentId{1},
                                          .clientCommandId = domain::ClientCommandId{"SELL"},
                                          .side = domain::Side::SELL,
                                          .timeInForce = task::TimeInForce::GTC,
                                          .price = domain::Price{100},
                                          .quantity = domain::Quantity{10}}));
    EXPECT_EQ(std::get<storage::JournalCancelV1>(loaded.commands[1]).targetOrderId, *cancel.targetOrderId);
    EXPECT_EQ(std::get<storage::JournalNewOrderV1>(loaded.commands[2]).commandSequence, domain::CommandSequence{3});
    EXPECT_EQ(controller.journalWriter()->committedByteCount(), loaded.validCommittedByteCount);
}

TEST_F(ControllerCommandProcessingTest, RecoveredPausedInstallationAndResumeUseExactBooksResultsAndNextSequence) {
    const auto sell = order();
    const auto buy = order("BUY", true, 4);
    {
        ExchangeRunController producer;
        ASSERT_EQ(producer.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
        ASSERT_TRUE(producer.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
        ASSERT_EQ(producer.submitCommandV1(domain::ExchangeRunId{1}, sell).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_TRUE(producer.advanceCommandProcessingV1());
        ASSERT_EQ(producer.submitCommandV1(domain::ExchangeRunId{1}, buy).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_TRUE(producer.advanceCommandProcessingV1());
    }
    ExchangeRunController controller;
    ASSERT_EQ(controller.startupExistingRunV1(catalog()).outcome, ExistingRunStartupOutcome::PAUSED);
    const auto* state = controller.matchingState();
    const auto* writer = controller.journalWriter();
    const auto* admission = controller.admissionIndex();
    const auto before = state->snapshot();
    ASSERT_EQ(before.activeOrders.size(), 1U);
    EXPECT_EQ(before.activeOrders[0].remainingQuantity, domain::Quantity{6});
    const auto retained = admission->completedResult(sell);
    expectCorrelation(retained, sell, 1);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::PAUSED);
    EXPECT_EQ(controller.inspectCommandProcessingV1().queuedPrivateEventRecords, 0U);
    EXPECT_EQ(controller.inspectCommandProcessingV1().queuedPublicTradeRecords, 0U);
    EXPECT_FALSE(controller.advanceCommandProcessingV1());
    const auto live = order("LIVE", true, 6);
    expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, live));
    EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, sell).originalResult, retained);
    EXPECT_EQ(controller.matchingState()->snapshot(), before);
    EXPECT_EQ(controller.journalWriter()->nextCommandSequence(), domain::CommandSequence{3});
    ASSERT_EQ(controller.resumeRunV1().outcome, RunResumeOutcome::READY);
    EXPECT_EQ(controller.matchingState(), state);
    EXPECT_EQ(controller.journalWriter(), writer);
    EXPECT_EQ(controller.admissionIndex(), admission);
    EXPECT_EQ(controller.admissionIndex()->completedResult(sell), retained);
    EXPECT_EQ(controller.matchingState()->snapshot(), before);

    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, live).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    const auto result = controller.admissionIndex()->completedResult(live);
    expectCorrelation(result, live, 3);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->events().size(), 1U);
    const auto* trade = std::get_if<domain::Trade>(&result->events()[0]);
    ASSERT_NE(trade, nullptr);
    EXPECT_EQ(trade->makerOrderId, domain::OrderId{1});
    EXPECT_EQ(trade->makerClientId, sell.clientId);
    EXPECT_EQ(trade->takerOrderId, domain::OrderId{3});
    EXPECT_EQ(trade->executionQuantity, domain::Quantity{6});
    EXPECT_TRUE(controller.matchingState()->snapshot().activeOrders.empty());
    const auto duplicate = controller.submitCommandV1(domain::ExchangeRunId{1}, live);
    EXPECT_EQ(duplicate.status, admission::AdmissionStatus::IDENTICAL_COMPLETED);
    EXPECT_EQ(duplicate.originalResult, result);
    EXPECT_FALSE(controller.advanceCommandProcessingV1());
    EXPECT_EQ(controller.admissionIndex()->completedResult(sell), retained);
    storage::LoadedRunJournalV1 loaded;
    ASSERT_EQ(storage::loadRunJournalV1(journal(), loaded).outcome, storage::RunJournalLoadOutcome::VALID);
    ASSERT_EQ(loaded.commands.size(), 3U);
    const auto& persisted = std::get<storage::JournalNewOrderV1>(loaded.commands.back());
    EXPECT_EQ(persisted.exchangeRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(persisted.commandSequence, domain::CommandSequence{3});
    EXPECT_EQ(persisted.clientCommandId, *live.clientCommandId);
    EXPECT_EQ(persisted.quantity, live.quantity);
}

TEST_F(ControllerCommandProcessingTest, PrivateResultHandoffPreservesRecipientAndSelfTradeViewsWithoutRedelivery) {
    ExchangeRunController controller;
    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));

    const auto maker = order("PRIVATE-MAKER", false, 2);
    const auto taker = order("PRIVATE-TAKER", true, 2);
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, maker).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    EXPECT_EQ(controller.inspectCommandProcessingV1().queuedPrivateEventRecords, 1U);
    private_result::RecipientResults privateBatch;
    ASSERT_TRUE(controller.tryPopPrivateResultBatchV1(privateBatch));
    ASSERT_EQ(privateBatch.size(), 1U);
    EXPECT_EQ(privateBatch[0].recipient, maker.clientId);
    ASSERT_EQ(privateBatch[0].privateResult.size(), 1U);
    EXPECT_NE(std::get_if<domain::OrderRested>(&privateBatch[0].privateResult[0]), nullptr);

    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, taker).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    EXPECT_EQ(controller.inspectCommandProcessingV1().queuedPrivateEventRecords, 2U);
    ASSERT_TRUE(controller.tryPopPrivateResultBatchV1(privateBatch));
    ASSERT_EQ(privateBatch.size(), 2U);
    EXPECT_EQ(privateBatch[0].recipient, maker.clientId);
    EXPECT_FALSE(privateBatch[0].correlation.has_value());
    EXPECT_EQ(privateBatch[1].recipient, taker.clientId);
    ASSERT_TRUE(privateBatch[1].correlation.has_value());
    EXPECT_EQ(*privateBatch[1].correlation,
              (domain::CommandResultCorrelation{taker.clientId, *taker.clientCommandId, domain::CommandSequence{2},
                                                domain::ExchangeRunId{1}}));
    const auto* makerView = std::get_if<private_result::PrivateTrade>(&privateBatch[0].privateResult[0]);
    const auto* takerView = std::get_if<private_result::PrivateTrade>(&privateBatch[1].privateResult[0]);
    ASSERT_NE(makerView, nullptr);
    ASSERT_NE(takerView, nullptr);
    EXPECT_EQ(makerView->role, private_result::TradeRole::MAKER);
    EXPECT_EQ(takerView->role, private_result::TradeRole::TAKER);

    auto selfMaker = order("SELF-MAKER", false, 1);
    selfMaker.clientId = domain::ClientId{33};
    auto selfTaker = order("SELF-TAKER", true, 1);
    selfTaker.clientId = selfMaker.clientId;
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, selfMaker).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    ASSERT_TRUE(controller.tryPopPrivateResultBatchV1(privateBatch));
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, selfTaker).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    const auto completed = controller.admissionIndex()->completedResult(selfTaker);
    ASSERT_TRUE(controller.tryPopPrivateResultBatchV1(privateBatch));
    ASSERT_EQ(privateBatch.size(), 1U);
    ASSERT_EQ(privateBatch[0].privateResult.size(), 2U);
    const auto* selfMakerView = std::get_if<private_result::PrivateTrade>(&privateBatch[0].privateResult[0]);
    const auto* selfTakerView = std::get_if<private_result::PrivateTrade>(&privateBatch[0].privateResult[1]);
    ASSERT_NE(selfMakerView, nullptr);
    ASSERT_NE(selfTakerView, nullptr);
    EXPECT_EQ(selfMakerView->role, private_result::TradeRole::MAKER);
    EXPECT_EQ(selfTakerView->role, private_result::TradeRole::TAKER);
    EXPECT_EQ(selfMakerView->eventId, selfTakerView->eventId);

    const auto duplicate = controller.submitCommandV1(domain::ExchangeRunId{1}, selfTaker);
    EXPECT_EQ(duplicate.status, admission::AdmissionStatus::IDENTICAL_COMPLETED);
    EXPECT_EQ(duplicate.originalResult, completed);
    EXPECT_EQ(controller.lookupPrivateResultV1(catalog(), domain::ExchangeRunId{1}, selfTaker).outcome,
              CompletedResultLookupOutcome::FOUND);
    EXPECT_FALSE(controller.tryPopPrivateResultBatchV1(privateBatch));
}

TEST_F(ControllerCommandProcessingTest, PublicTradeHandoffSaturationRetainsAndRetriesOneExactBatchInFifoOrder) {
    ExchangeRunController controller;
    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration(5'000, 8 * 1024 * 1024)).outcome,
              NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));

    for (std::size_t index = 0; index < 4'097; ++index) {
        const auto sell = order("PUBLIC-SELL-" + std::to_string(index), false, 1);
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, sell).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_TRUE(controller.advanceCommandProcessingV1());
    }
    const auto firstBuy = order("PUBLIC-BUY-MAX", true, 4'096);
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, firstBuy).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    const auto secondBuy = order("PUBLIC-BUY-PENDING", true, 1);
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, secondBuy).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());

    const auto saturated = controller.inspectCommandProcessingV1();
    EXPECT_EQ(saturated.pendingPrivateEventRecords, 0U);
    EXPECT_EQ(saturated.queuedPrivateEventRecords, 12'291U);
    EXPECT_EQ(saturated.queuedPublicTradeRecords, 4'096U);
    EXPECT_EQ(saturated.pendingPublicTradeRecords, 1U);
    EXPECT_FALSE(saturated.pendingResultProjection);
    const auto secondCompleted = controller.admissionIndex()->completedResult(secondBuy);
    expectCorrelation(secondCompleted, secondBuy, 4'099);
    EXPECT_FALSE(controller.advanceCommandProcessingV1());
    EXPECT_EQ(controller.admissionIndex()->completedResult(secondBuy), secondCompleted);

    std::vector<market_data::PublicTrade> batch;
    ASSERT_TRUE(controller.tryPopPublicTradeBatchV1(batch));
    ASSERT_EQ(batch.size(), 4'096U);
    EXPECT_EQ(batch.front().eventId,
              (domain::EventId{domain::CommandSequence{4'098}, domain::EventIndex{0}, domain::ExchangeRunId{1}}));
    EXPECT_EQ(batch.back().eventId,
              (domain::EventId{domain::CommandSequence{4'098}, domain::EventIndex{4'095}, domain::ExchangeRunId{1}}));
    EXPECT_EQ(batch[0].aggressorSide, domain::Side::BUY);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    const auto retried = controller.inspectCommandProcessingV1();
    EXPECT_EQ(retried.pendingPublicTradeRecords, 0U);
    EXPECT_EQ(retried.queuedPublicTradeRecords, 1U);
    EXPECT_FALSE(controller.advanceCommandProcessingV1());

    ASSERT_TRUE(controller.tryPopPublicTradeBatchV1(batch));
    ASSERT_EQ(batch.size(), 1U);
    EXPECT_EQ(batch[0].eventId,
              (domain::EventId{domain::CommandSequence{4'099}, domain::EventIndex{0}, domain::ExchangeRunId{1}}));
    EXPECT_FALSE(controller.tryPopPublicTradeBatchV1(batch));
    EXPECT_EQ(controller.inspectCommandProcessingV1().queuedPublicTradeRecords, 0U);
}

TEST_F(ControllerCommandProcessingTest, BothResultHandoffsRetryIndependentlyWithoutRecompletionOrOvertaking) {
    ExchangeRunController controller;
    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration(5'000, 8 * 1024 * 1024)).outcome,
              NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 8'192, 4'096));

    auto survivor = order("SURVIVOR", false, 1);
    survivor.price = domain::Price{101};
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, survivor).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    for (std::size_t index = 0; index < 4'096; ++index) {
        auto selfMaker = order("SELF-MAKER-" + std::to_string(index), false, 1);
        selfMaker.clientId = domain::ClientId{33};
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, selfMaker).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_TRUE(controller.advanceCommandProcessingV1());
    }
    drainPrivateResultHandoff(controller);

    auto selfTaker = order("SELF-TAKER-MAX", true, 4'096);
    selfTaker.clientId = domain::ClientId{33};
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, selfTaker).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    auto inspection = controller.inspectCommandProcessingV1();
    EXPECT_EQ(inspection.queuedPrivateEventRecords, 8'192U);
    EXPECT_EQ(inspection.queuedPublicTradeRecords, 4'096U);

    auto finalTaker = order("FINAL-TAKER", true, 1);
    finalTaker.price = survivor.price;
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, finalTaker).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    const auto completed = controller.admissionIndex()->completedResult(finalTaker);
    inspection = controller.inspectCommandProcessingV1();
    EXPECT_EQ(inspection.pendingPrivateEventRecords, 2U);
    EXPECT_EQ(inspection.pendingPublicTradeRecords, 1U);
    EXPECT_FALSE(inspection.pendingResultProjection);
    EXPECT_FALSE(controller.advanceCommandProcessingV1());
    EXPECT_EQ(controller.admissionIndex()->completedResult(finalTaker), completed);

    std::vector<market_data::PublicTrade> publicBatch;
    ASSERT_TRUE(controller.tryPopPublicTradeBatchV1(publicBatch));
    ASSERT_EQ(publicBatch.size(), 4'096U);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    inspection = controller.inspectCommandProcessingV1();
    EXPECT_EQ(inspection.pendingPrivateEventRecords, 2U);
    EXPECT_EQ(inspection.pendingPublicTradeRecords, 0U);
    EXPECT_EQ(inspection.queuedPublicTradeRecords, 1U);

    private_result::RecipientResults privateBatch;
    ASSERT_TRUE(controller.tryPopPrivateResultBatchV1(privateBatch));
    ASSERT_EQ(privateBatch.size(), 1U);
    ASSERT_EQ(privateBatch[0].privateResult.size(), 8'192U);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    inspection = controller.inspectCommandProcessingV1();
    EXPECT_EQ(inspection.pendingPrivateEventRecords, 0U);
    EXPECT_EQ(inspection.queuedPrivateEventRecords, 2U);
    EXPECT_EQ(controller.admissionIndex()->completedResult(finalTaker), completed);
    EXPECT_EQ(controller.journalWriter()->nextCommandSequence(), domain::CommandSequence{4'100});
    EXPECT_FALSE(controller.advanceCommandProcessingV1());

    ASSERT_TRUE(controller.tryPopPrivateResultBatchV1(privateBatch));
    ASSERT_EQ(privateBatch.size(), 2U);
    EXPECT_EQ(privateBatch[0].recipient, survivor.clientId);
    EXPECT_EQ(privateBatch[1].recipient, finalTaker.clientId);
    EXPECT_FALSE(controller.tryPopPrivateResultBatchV1(privateBatch));
    ASSERT_TRUE(controller.tryPopPublicTradeBatchV1(publicBatch));
    ASSERT_EQ(publicBatch.size(), 1U);
    EXPECT_FALSE(controller.tryPopPublicTradeBatchV1(publicBatch));
}

TEST_F(ControllerCommandProcessingTest, PauseAndStopRetryAfterCallerDrainsBothResultHandoffs) {
    ExchangeRunController controller;
    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));

    const auto submitCross = [&](const std::string_view suffix) {
        const auto sell = order(std::string{"LIFECYCLE-SELL-"} + std::string{suffix}, false, 1);
        const auto buy = order(std::string{"LIFECYCLE-BUY-"} + std::string{suffix}, true, 1);
        EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, sell).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        EXPECT_TRUE(controller.advanceCommandProcessingV1());
        EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, buy).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        EXPECT_TRUE(controller.advanceCommandProcessingV1());
    };

    submitCross("PAUSE");
    EXPECT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::RESULT_HANDOFF_BACKPRESSURE);
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::READY);
    expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, order("CLOSED-AFTER-PAUSE")));
    std::vector<market_data::PublicTrade> batch;
    ASSERT_TRUE(controller.tryPopPublicTradeBatchV1(batch));
    drainPrivateResultHandoff(controller);
    ASSERT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::PAUSED);

    ASSERT_EQ(controller.resumeRunV1().outcome, RunResumeOutcome::READY);
    submitCross("STOP");
    const auto blockedStop = controller.stopRunV1();
    EXPECT_EQ(blockedStop.outcome, RunStopOutcome::RESULT_HANDOFF_BACKPRESSURE);
    ASSERT_TRUE(blockedStop.pause.has_value());
    EXPECT_EQ(blockedStop.pause->outcome, RunPauseOutcome::RESULT_HANDOFF_BACKPRESSURE);
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::READY);
    expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, order("CLOSED-AFTER-STOP")));
    ASSERT_TRUE(controller.tryPopPublicTradeBatchV1(batch));
    drainPrivateResultHandoff(controller);
    EXPECT_EQ(controller.stopRunV1().outcome, RunStopOutcome::STOPPED);
}

TEST_F(ControllerCommandProcessingTest,
       ProcessingFailureTakesPriorityAndFailedRecoveryPreservesQueuedResultHandoffsUntilDrain) {
    const detail::StartNewRunHooks startupHooks{
        nullptr,
        [](void*, domain::ExchangeRunId runId, std::uint64_t capacity,
           std::unique_ptr<matching_engine::MatchingState>& state,
           std::unique_ptr<admission::CommandAdmissionIndex>& admission) noexcept {
            const auto error = detail::constructEmptyRunStateV1(runId, capacity, state, admission);
            if (error.has_value()) {
                return error;
            }
            auto collision = order("INJECTED-COLLISION");
            collision.price = domain::Price{200};
            collision.globalSequenceNumber = domain::CommandSequence{3};
            collision.orderId = domain::OrderId{3};
            try {
                (void)state->processCommand(collision);
            } catch (...) {
                return std::optional{StartNewRunOutcome::STATE_CONSTRUCTION_FAILED};
            }
            return std::optional<StartNewRunOutcome>{};
        },
        [](void*, const std::filesystem::path& path, const storage::PreparedNewRunJournalV1& prepared) noexcept {
            return storage::activatePreparedNewRunV1(path, prepared);
        },
        nullptr};
    ExchangeRunController controller;
    ASSERT_EQ(detail::startupNewRunV1WithHooks(catalog(), configuration(), &startupHooks, controller).outcome,
              NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    const auto maker = order("PUBLIC-BEFORE-FAILURE", false, 1);
    const auto taker = order("PUBLIC-TAKER", true, 1);
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, maker).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, taker).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    ASSERT_EQ(controller.inspectCommandProcessingV1().queuedPublicTradeRecords, 1U);

    const auto failed = order("MATCHING-FAILURE", false, 1);
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, failed).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    EXPECT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::PROCESSING_FAILED);
    ASSERT_EQ(controller.state(), ExchangeRunStartupState::FAIL_STOPPED);
    const auto* writer = controller.journalWriter();
    const auto* matching = controller.matchingState();
    const auto* admission = controller.admissionIndex();
    const auto committedCommands = writer->committedCommandCount();
    const auto committedBytes = writer->committedByteCount();
    const auto nextSequence = writer->nextCommandSequence();
    const auto matchingSnapshot = matching->snapshot();
    const auto admissionSize = admission->size();
    const auto failedInspection = controller.inspectCommandProcessingV1();
    ASSERT_NE(failedInspection.internalFailure, nullptr);
    EXPECT_EQ(failedInspection.queuedPublicTradeRecords, 1U);
    ASSERT_TRUE(failedInspection.failStopCatalogReplacement.has_value());
    EXPECT_EQ(failedInspection.failStopCatalogReplacement->outcome, storage::RunCatalogReplaceOutcome::COMMITTED);

    const auto refused = controller.recoverFailedRunV1(catalog());
    ASSERT_EQ(refused.outcome, FailedRunRecoveryOutcome::RESULT_HANDOFF_BACKPRESSURE);
    ASSERT_TRUE(refused.originalProcessingInspection.has_value());
    EXPECT_EQ(refused.originalProcessingInspection->internalFailure, failedInspection.internalFailure);
    EXPECT_EQ(refused.originalProcessingInspection->queuedPublicTradeRecords, 1U);
    ASSERT_TRUE(refused.catalogLoad.has_value());
    EXPECT_EQ(refused.catalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_FALSE(refused.recovery.has_value());
    EXPECT_FALSE(refused.catalogReplacement.has_value());
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::FAIL_STOPPED);
    EXPECT_EQ(controller.journalWriter(), writer);
    EXPECT_EQ(controller.matchingState(), matching);
    EXPECT_EQ(controller.admissionIndex(), admission);
    EXPECT_EQ(writer->committedCommandCount(), committedCommands);
    EXPECT_EQ(writer->committedByteCount(), committedBytes);
    EXPECT_EQ(writer->nextCommandSequence(), nextSequence);
    EXPECT_EQ(matching->snapshot(), matchingSnapshot);
    EXPECT_EQ(admission->size(), admissionSize);
    EXPECT_EQ(controller.inspectCommandProcessingV1().internalFailure, failedInspection.internalFailure);

    std::vector<market_data::PublicTrade> publicTrades;
    ASSERT_TRUE(controller.tryPopPublicTradeBatchV1(publicTrades));
    ASSERT_EQ(publicTrades.size(), 1U);
    EXPECT_EQ(publicTrades[0].eventId,
              (domain::EventId{domain::CommandSequence{2}, domain::EventIndex{0}, domain::ExchangeRunId{1}}));
    const auto privateBlocked = controller.recoverFailedRunV1(catalog());
    ASSERT_EQ(privateBlocked.outcome, FailedRunRecoveryOutcome::RESULT_HANDOFF_BACKPRESSURE);
    ASSERT_TRUE(privateBlocked.originalProcessingInspection.has_value());
    EXPECT_EQ(privateBlocked.originalProcessingInspection->queuedPublicTradeRecords, 0U);
    EXPECT_GT(privateBlocked.originalProcessingInspection->queuedPrivateEventRecords, 0U);
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::FAIL_STOPPED);
    drainPrivateResultHandoff(controller);
    const auto recovered = controller.recoverFailedRunV1(catalog());
    ASSERT_EQ(recovered.outcome, FailedRunRecoveryOutcome::PAUSED);
    ASSERT_TRUE(recovered.originalProcessingInspection.has_value());
    EXPECT_EQ(recovered.originalProcessingInspection->internalFailure, failedInspection.internalFailure);
    EXPECT_EQ(recovered.originalProcessingInspection->queuedPublicTradeRecords, 0U);
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::PAUSED);
    EXPECT_FALSE(controller.inspectCommandProcessingV1().installed);
    expectCorrelation(controller.admissionIndex()->completedResult(failed), failed, 3);
}

TEST_F(ControllerCommandProcessingTest, MatchingQueueAndResultPressureRetainFifoWithoutDuplicateProcessing) {
    // Capacity one exercises sequencer pending handoff; capacity four exercises matcher pending result.
    for (const std::size_t matchingCapacity : {1U, 4U}) {
        // Separate installation directories keep run/catalog ownership exclusive.
        const auto subdirectory = catalog().parent_path() / std::to_string(matchingCapacity);
        ASSERT_TRUE(std::filesystem::create_directory(subdirectory));
        ExchangeRunController controller;
        ASSERT_EQ(controller.startupNewRunV1(subdirectory / "run-catalog-v1", configuration()).outcome,
                  NewRunStartupOutcome::READY);
        ASSERT_TRUE(controller.installCommandProcessingV1(4, matchingCapacity, 1, 16'384, 4'096));
        std::array<sequencer::sequenceMessage, 4> commands;
        for (std::size_t index = 0; index < commands.size(); ++index) {
            commands[index] = order("ORDER-" + std::to_string(index));
            ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, commands[index]).status,
                      admission::AdmissionStatus::FIRST_SUBMISSION);
        }
        for (std::size_t completed = 0; completed < commands.size(); ++completed) {
            ASSERT_TRUE(controller.advanceCommandProcessingV1());
            expectCorrelation(controller.admissionIndex()->completedResult(commands[completed]), commands[completed],
                              completed + 1);
            for (std::size_t later = completed + 1; later < commands.size(); ++later) {
                EXPECT_EQ(controller.admissionIndex()->completedResult(commands[later]), nullptr);
            }
            const auto inspection = controller.inspectCommandProcessingV1();
            EXPECT_FALSE(inspection.appendFailure.has_value());
            EXPECT_EQ(inspection.internalFailure, nullptr);
            if (completed + 1 < commands.size()) {
                if (matchingCapacity == 1) {
                    ASSERT_TRUE(inspection.pendingCommand.has_value());
                    EXPECT_EQ(inspection.pendingCommand->globalSequenceNumber, domain::CommandSequence{completed + 2});
                    EXPECT_FALSE(inspection.pendingMatchingResult);
                } else {
                    EXPECT_TRUE(inspection.pendingMatchingResult);
                    EXPECT_EQ(controller.matchingState()->snapshot().activeOrders.size(),
                              std::min(completed + 2, commands.size()));
                }
            }
        }
        EXPECT_FALSE(controller.advanceCommandProcessingV1());
        const auto inspection = controller.inspectCommandProcessingV1();
        EXPECT_TRUE(inspection.ingressEmpty);
        EXPECT_TRUE(inspection.matchingEmpty);
        EXPECT_FALSE(inspection.pendingCommand.has_value());
        EXPECT_FALSE(inspection.pendingMatchingResult);
        EXPECT_EQ(inspection.queuedResults, 0U);
        EXPECT_EQ(controller.matchingState()->snapshot().activeOrders.size(), 4U);
        EXPECT_TRUE(controller.matchingState()->invariantsHold());
        EXPECT_EQ(controller.admissionIndex()->statistics().firstSubmissions, 4U);
        storage::LoadedRunJournalV1 loaded;
        ASSERT_EQ(storage::loadRunJournalV1(subdirectory / "run-1.fxjr", loaded).outcome,
                  storage::RunJournalLoadOutcome::VALID);
        ASSERT_EQ(loaded.commands.size(), commands.size());
        for (std::size_t index = 0; index < commands.size(); ++index) {
            const auto& persisted = std::get<storage::JournalNewOrderV1>(loaded.commands[index]);
            EXPECT_EQ(persisted.clientCommandId, *commands[index].clientCommandId);
            EXPECT_EQ(persisted.commandSequence, domain::CommandSequence{index + 1});
        }
    }
}

TEST_F(ControllerCommandProcessingTest, PauseDrainsAcceptedWorkThroughBoundedHandoffsThenResumesSameRun) {
    ExchangeRunController controller;
    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(4, 1, 1, 16'384, 4'096));
    const auto* state = controller.matchingState();
    const auto* writer = controller.journalWriter();
    const auto* admission = controller.admissionIndex();
    std::array<sequencer::sequenceMessage, 4> commands;
    for (std::size_t index = 0; index < commands.size(); ++index) {
        commands[index] = order("PAUSE-" + std::to_string(index));
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, commands[index]).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
    }

    const auto blocked = controller.pauseRunV1();
    ASSERT_EQ(blocked.outcome, RunPauseOutcome::RESULT_HANDOFF_BACKPRESSURE);
    drainPrivateResultHandoff(controller);
    const auto paused = controller.pauseRunV1();

    ASSERT_EQ(paused.outcome, RunPauseOutcome::PAUSED);
    ASSERT_TRUE(paused.catalogReplacement.has_value());
    EXPECT_EQ(paused.catalogReplacement->outcome, storage::RunCatalogReplaceOutcome::COMMITTED);
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::PAUSED);
    const auto inspection = controller.inspectCommandProcessingV1();
    EXPECT_TRUE(inspection.ingressEmpty);
    EXPECT_TRUE(inspection.matchingEmpty);
    EXPECT_EQ(inspection.queuedResults, 0U);
    EXPECT_FALSE(inspection.pendingCommand.has_value());
    EXPECT_FALSE(inspection.pendingMatchingResult);
    EXPECT_FALSE(inspection.pendingCompletion);
    EXPECT_FALSE(inspection.appendFailure.has_value());
    EXPECT_EQ(inspection.internalFailure, nullptr);
    EXPECT_EQ(state->snapshot().activeOrders.size(), commands.size());
    EXPECT_TRUE(state->invariantsHold());
    EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{5});
    storage::LoadedRunJournalV1 loaded;
    ASSERT_EQ(storage::loadRunJournalV1(journal(), loaded).outcome, storage::RunJournalLoadOutcome::VALID);
    ASSERT_EQ(loaded.commands.size(), commands.size());
    for (std::size_t index = 0; index < commands.size(); ++index) {
        const auto result = admission->completedResult(commands[index]);
        expectCorrelation(result, commands[index], index + 1);
        EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, commands[index]).originalResult, result);
        const auto& persisted = std::get<storage::JournalNewOrderV1>(loaded.commands[index]);
        EXPECT_EQ(persisted.clientCommandId, *commands[index].clientCommandId);
        EXPECT_EQ(persisted.commandSequence, domain::CommandSequence{index + 1});
    }
    storage::RunCatalogSnapshotV1 snapshot;
    ASSERT_EQ(storage::loadRunCatalogV1(catalog(), snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(snapshot.activeRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(snapshot.activeDisposition, storage::RunCatalogDisposition::PAUSED);
    EXPECT_EQ(snapshot.generation, 3U);
    expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, order("BLOCKED")));
    EXPECT_FALSE(controller.advanceCommandProcessingV1());
    EXPECT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::NOT_READY);

    const auto original = admission->completedResult(commands[0]);
    ASSERT_EQ(controller.resumeRunV1().outcome, RunResumeOutcome::READY);
    EXPECT_EQ(controller.matchingState(), state);
    EXPECT_EQ(controller.journalWriter(), writer);
    EXPECT_EQ(controller.admissionIndex(), admission);
    EXPECT_EQ(admission->completedResult(commands[0]), original);
    EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{5});
    const auto after = order("AFTER-RESUME");
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, after).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    expectCorrelation(admission->completedResult(after), after, 5);
}

TEST_F(ControllerCommandProcessingTest, CommandCapacityAccountsForQueuedWorkAndPublishesOnlyAfterFifoDrain) {
    ExchangeRunController controller;
    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration(3, 64 * 1024)).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(4, 1, 1, 16'384, 4'096));
    const auto* state = controller.matchingState();
    const auto* writer = controller.journalWriter();
    const auto* admission = controller.admissionIndex();
    std::array<sequencer::sequenceMessage, 3> commands{order("CAP-1"), order("CAP-2"), order("CAP-3")};
    for (const auto& command : commands) {
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, command).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
    }
    const auto queued = controller.inspectCommandProcessingV1();
    EXPECT_TRUE(queued.capacityTransitionPending);
    EXPECT_EQ(writer->committedCommandCount(), 0U);
    EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{1});
    const auto journalBytes = writer->committedByteCount();
    const auto next = order("CAP-4");
    expectCapacityReached(controller.submitCommandV1(domain::ExchangeRunId{1}, next));
    EXPECT_EQ(admission->size(), commands.size());
    EXPECT_EQ(writer->committedByteCount(), journalBytes);
    EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{1});
    const auto afterRejection = controller.inspectCommandProcessingV1();
    EXPECT_FALSE(afterRejection.ingressEmpty);

    const auto duplicate = controller.submitCommandV1(domain::ExchangeRunId{1}, commands[0]);
    EXPECT_EQ(duplicate.status, admission::AdmissionStatus::IDENTICAL_IN_FLIGHT);
    auto conflict = commands[0];
    conflict.quantity = domain::Quantity{11};
    EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, conflict).status,
              admission::AdmissionStatus::CONFLICTING_REUSE);
    EXPECT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::NOT_READY);
    EXPECT_EQ(controller.resumeRunV1().outcome, RunResumeOutcome::NOT_PAUSED);

    for (std::size_t attempts = 0; attempts < 8 && controller.state() == ExchangeRunStartupState::READY; ++attempts) {
        ASSERT_TRUE(controller.advanceCommandProcessingV1());
    }
    ASSERT_EQ(controller.state(), ExchangeRunStartupState::CAPACITY_REACHED);
    EXPECT_EQ(controller.matchingState(), state);
    EXPECT_EQ(controller.journalWriter(), writer);
    EXPECT_EQ(controller.admissionIndex(), admission);
    EXPECT_EQ(writer->committedCommandCount(), commands.size());
    EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{4});
    EXPECT_EQ(state->snapshot().activeOrders.size(), commands.size());
    const auto drained = controller.inspectCommandProcessingV1();
    EXPECT_TRUE(drained.ingressEmpty);
    EXPECT_TRUE(drained.matchingEmpty);
    EXPECT_EQ(drained.queuedResults, 0U);
    EXPECT_FALSE(drained.pendingCommand.has_value());
    EXPECT_FALSE(drained.pendingMatchingResult);
    EXPECT_FALSE(drained.pendingCompletion);
    ASSERT_TRUE(drained.capacityCatalogReplacement.has_value());
    EXPECT_EQ(drained.capacityCatalogReplacement->outcome, storage::RunCatalogReplaceOutcome::COMMITTED);
    for (std::size_t index = 0; index < commands.size(); ++index) {
        expectCorrelation(admission->completedResult(commands[index]), commands[index], index + 1);
    }
    const auto completed = controller.submitCommandV1(domain::ExchangeRunId{1}, commands[0]);
    EXPECT_EQ(completed.status, admission::AdmissionStatus::IDENTICAL_COMPLETED);
    EXPECT_EQ(completed.originalResult, admission->completedResult(commands[0]));
    EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, conflict).status,
              admission::AdmissionStatus::CONFLICTING_REUSE);
    expectCapacityReached(controller.submitCommandV1(domain::ExchangeRunId{1}, next));
    EXPECT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::NOT_READY);
    EXPECT_EQ(controller.resumeRunV1().outcome, RunResumeOutcome::NOT_PAUSED);

    storage::RunCatalogSnapshotV1 snapshot;
    ASSERT_EQ(storage::loadRunCatalogV1(catalog(), snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(snapshot.generation, 3U);
    EXPECT_EQ(snapshot.activeRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(snapshot.activeDisposition, storage::RunCatalogDisposition::CAPACITY_REACHED);
    ExchangeRunController restarted;
    EXPECT_EQ(restarted.startupExistingRunV1(catalog()).outcome,
              ExistingRunStartupOutcome::EXPLICIT_OPERATION_REQUIRED);
    EXPECT_EQ(restarted.state(), ExchangeRunStartupState::UNAVAILABLE);
}

TEST_F(ControllerCommandProcessingTest, ExactFinalJournalFrameFitsAndNextUniqueCommandDoesNot) {
    const auto first = order("BYTE-ONE");
    const auto next = order("BYTE-TWO");
    auto runConfiguration = configuration(10, 64 * 1024);
    runConfiguration.maxRunJournalBytes = encodedHeaderSize(runConfiguration) + encodedOrderSize(first);
    ExchangeRunController controller;
    ASSERT_EQ(controller.startupNewRunV1(catalog(), runConfiguration).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(3, 1, 1, 16'384, 4'096));
    const std::uint64_t beforeBytes = controller.journalWriter()->committedByteCount();

    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, first).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    EXPECT_TRUE(controller.inspectCommandProcessingV1().capacityTransitionPending);
    expectCapacityReached(controller.submitCommandV1(domain::ExchangeRunId{1}, next));
    const auto queued = controller.inspectCommandProcessingV1();
    EXPECT_TRUE(queued.capacityTransitionPending);
    EXPECT_EQ(controller.admissionIndex()->size(), 1U);
    EXPECT_EQ(controller.journalWriter()->committedByteCount(), beforeBytes);
    EXPECT_EQ(controller.journalWriter()->nextCommandSequence(), domain::CommandSequence{1});

    for (std::size_t attempts = 0; attempts < 4 && controller.state() == ExchangeRunStartupState::READY; ++attempts) {
        ASSERT_TRUE(controller.advanceCommandProcessingV1());
    }
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::CAPACITY_REACHED);
    expectCorrelation(controller.admissionIndex()->completedResult(first), first, 1);
    EXPECT_EQ(controller.admissionIndex()->completedResult(next), nullptr);
    EXPECT_EQ(controller.journalWriter()->committedByteCount(), beforeBytes + encodedOrderSize(first));
    EXPECT_EQ(controller.journalWriter()->committedByteCount(), runConfiguration.maxRunJournalBytes);
    storage::LoadedRunJournalV1 loaded;
    ASSERT_EQ(storage::loadRunJournalV1(journal(), loaded).outcome, storage::RunJournalLoadOutcome::VALID);
    ASSERT_EQ(loaded.commands.size(), 1U);
    EXPECT_EQ(std::get<storage::JournalNewOrderV1>(loaded.commands[0]).clientCommandId, *first.clientCommandId);
}

enum class AppendFault { NONE, IMMEDIATE_WRITE, PARTIAL_WRITE, SYNC };

struct AppendProbe final {
    AppendFault fault{AppendFault::NONE};
    int writes{0};
    int syncs{0};
    ExchangeRunController* controller{nullptr};
    bool checkClosedOnSync{false};
    bool observedClosed{false};
};

ssize_t probeWrite(void* context, const int descriptor, const void* buffer, const std::size_t size) noexcept {
    auto& probe = *static_cast<AppendProbe*>(context);
    ++probe.writes;
    if ((probe.fault == AppendFault::IMMEDIATE_WRITE && probe.writes == 2) ||
        (probe.fault == AppendFault::PARTIAL_WRITE && probe.writes == 3)) {
        errno = ENOSPC;
        return -1;
    }
    return ::write(descriptor, buffer, probe.fault == AppendFault::PARTIAL_WRITE && probe.writes == 2 ? 7 : size);
}

int probeSync(void* context, const int descriptor) noexcept {
    auto& probe = *static_cast<AppendProbe*>(context);
    ++probe.syncs;
    // The real worker has not mutated even the earlier committed command in this sequence phase.
    EXPECT_TRUE(probe.controller->matchingState()->snapshot().activeOrders.empty());
    EXPECT_EQ(probe.controller->journalWriter()->committedCommandCount(), probe.syncs - 1U);
    EXPECT_EQ(probe.controller->admissionIndex()->completedResult(order("FIRST")), nullptr);
    if (probe.checkClosedOnSync) {
        const auto attempt = probe.controller->submitCommandV1(domain::ExchangeRunId{1}, order("AFTER-GATE"));
        probe.observedClosed = attempt.status == admission::AdmissionStatus::ADMISSION_UNAVAILABLE &&
                               attempt.rejectionReason == domain::AdmissionRejectionReason::EXCHANGE_RUN_UNAVAILABLE;
    }
    if (probe.fault == AppendFault::SYNC && probe.syncs == 2) {
        errno = EIO;
        return -1;
    }
    return ::fdatasync(descriptor);
}

storage::RunJournalCreateResult createProbedWriter(void* context, const std::filesystem::path& path,
                                                   const storage::RunHeaderV1& header,
                                                   std::unique_ptr<storage::RunJournalWriterV1>& output) noexcept {
    return storage::detail::createRunJournalWriterV1WithHooks(path, header, {context, probeWrite, probeSync}, output);
}

NewRunStartupResult startProbedRun(const std::filesystem::path& catalogPath, AppendProbe& probe,
                                   ExchangeRunController& controller) {
    const storage::detail::NewRunJournalPreparationHooks preparation{&probe, createProbedWriter};
    const detail::StartNewRunHooks startupHooks{
        nullptr,
        [](void*, domain::ExchangeRunId runId, std::uint64_t capacity,
           std::unique_ptr<matching_engine::MatchingState>& state,
           std::unique_ptr<admission::CommandAdmissionIndex>& admission) noexcept {
            return detail::constructEmptyRunStateV1(runId, capacity, state, admission);
        },
        [](void*, const std::filesystem::path& path, const storage::PreparedNewRunJournalV1& prepared) noexcept {
            return storage::activatePreparedNewRunV1(path, prepared);
        },
        &preparation};
    return detail::startupNewRunV1WithHooks(catalogPath, configuration(), &startupHooks, controller);
}

TEST_F(ControllerCommandProcessingTest, AppendFailuresPublishFailStoppedAndPreserveAllOwnedWork) {
    for (const auto fault :
         {AppendFault::NONE, AppendFault::IMMEDIATE_WRITE, AppendFault::PARTIAL_WRITE, AppendFault::SYNC}) {
        const auto subdirectory = catalog().parent_path() / std::to_string(static_cast<int>(fault));
        ASSERT_TRUE(std::filesystem::create_directory(subdirectory));
        // Hook context outlives the controller-owned writer.
        AppendProbe probe{.fault = fault};
        ExchangeRunController controller;
        probe.controller = &controller;
        ASSERT_EQ(startProbedRun(subdirectory / "run-catalog-v1", probe, controller).outcome,
                  NewRunStartupOutcome::READY);
        ASSERT_TRUE(controller.installCommandProcessingV1(3, 3, 1, 16'384, 4'096));
        const auto* state = controller.matchingState();
        const auto* writer = controller.journalWriter();
        const auto* admission = controller.admissionIndex();
        const auto before = state->snapshot();
        const auto first = order("FIRST");
        const auto failed = order("FAILED");
        const auto later = order("LATER");
        for (const auto& command : {first, failed, later}) {
            ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, command).status,
                      admission::AdmissionStatus::FIRST_SUBMISSION);
        }
        if (fault == AppendFault::NONE) {
            ASSERT_TRUE(controller.advanceCommandProcessingV1());
            EXPECT_EQ(probe.syncs, 3);
            expectCorrelation(controller.admissionIndex()->completedResult(first), first, 1);
            ASSERT_TRUE(controller.advanceCommandProcessingV1());
            ASSERT_TRUE(controller.advanceCommandProcessingV1());
            expectCorrelation(controller.admissionIndex()->completedResult(later), later, 3);
            continue;
        }
        EXPECT_FALSE(controller.advanceCommandProcessingV1());
        EXPECT_EQ(controller.state(), ExchangeRunStartupState::FAIL_STOPPED);
        const auto inspection = controller.inspectCommandProcessingV1();
        ASSERT_TRUE(inspection.appendFailure.has_value());
        EXPECT_EQ(inspection.appendFailure->outcome, fault == AppendFault::IMMEDIATE_WRITE
                                                         ? storage::RunJournalAppendOutcome::IO_FAILURE
                                                         : storage::RunJournalAppendOutcome::RECOVERY_REQUIRED);
        EXPECT_EQ(inspection.appendFailure->systemError, fault == AppendFault::SYNC ? EIO : ENOSPC);
        EXPECT_EQ(inspection.appendFailure->error, storage::RunJournalAppendError::NONE);
        EXPECT_EQ(inspection.appendFailure->codecError, storage::JournalCommandCodecError::NONE);
        EXPECT_EQ(inspection.internalFailure, nullptr);
        EXPECT_FALSE(inspection.capacityTransitionPending);
        EXPECT_FALSE(inspection.capacityCatalogReplacement.has_value());
        EXPECT_EQ(inspection.failStopCatalogPreconditionFailure, FailStopCatalogPreconditionFailure::NONE);
        ASSERT_TRUE(inspection.failStopCatalogLoad.has_value());
        EXPECT_EQ(inspection.failStopCatalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
        ASSERT_TRUE(inspection.failStopCatalogReplacement.has_value());
        EXPECT_EQ(inspection.failStopCatalogReplacement->outcome, storage::RunCatalogReplaceOutcome::COMMITTED);
        ASSERT_TRUE(inspection.pendingCommand.has_value());
        EXPECT_EQ(inspection.pendingCommand->globalSequenceNumber, domain::CommandSequence{2});
        EXPECT_EQ(inspection.pendingCommand->clientCommandId, failed.clientCommandId);
        EXPECT_FALSE(inspection.ingressEmpty);  // Third accepted command remains staged.
        EXPECT_FALSE(inspection.matchingEmpty); // First committed command remains queued.
        EXPECT_FALSE(inspection.pendingMatchingResult);
        EXPECT_EQ(inspection.queuedResults, 0U);
        EXPECT_EQ(controller.matchingState(), state);
        EXPECT_EQ(controller.journalWriter(), writer);
        EXPECT_EQ(controller.admissionIndex(), admission);
        EXPECT_EQ(state->snapshot(), before);
        EXPECT_EQ(writer->committedCommandCount(), 1U);
        EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{2});
        EXPECT_EQ(admission->completedResult(first), nullptr);
        EXPECT_EQ(admission->completedResult(failed), nullptr);
        EXPECT_EQ(admission->completedResult(later), nullptr);
        expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, order("AFTER-FAILURE")));
        EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, failed).status,
                  admission::AdmissionStatus::IDENTICAL_IN_FLIGHT);
        const int writes = probe.writes;
        const int syncs = probe.syncs;
        for (int attempt = 0; attempt < 3; ++attempt) {
            EXPECT_FALSE(controller.advanceCommandProcessingV1());
        }
        EXPECT_EQ(probe.writes, writes);
        EXPECT_EQ(probe.syncs, syncs);
        EXPECT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::NOT_READY);
        EXPECT_EQ(controller.resumeRunV1().outcome, RunResumeOutcome::NOT_PAUSED);
        EXPECT_FALSE(controller.installCommandProcessingV1(3, 3, 1, 16'384, 4'096));
        EXPECT_EQ(state->snapshot(), before);
        EXPECT_EQ(controller.inspectCommandProcessingV1().appendFailure->outcome, inspection.appendFailure->outcome);
        EXPECT_EQ(controller.inspectCommandProcessingV1().appendFailure->systemError,
                  inspection.appendFailure->systemError);
        storage::LoadedRunJournalV1 loaded;
        const auto load = storage::loadRunJournalV1(subdirectory / "run-1.fxjr", loaded);
        EXPECT_EQ(load.outcome, fault == AppendFault::PARTIAL_WRITE ? storage::RunJournalLoadOutcome::INCOMPLETE_TAIL
                                                                    : storage::RunJournalLoadOutcome::VALID);
        ASSERT_EQ(loaded.commands.size(), fault == AppendFault::SYNC ? 2U : 1U);
        EXPECT_EQ(std::get<storage::JournalNewOrderV1>(loaded.commands[0]).clientCommandId, *first.clientCommandId);
        storage::RunCatalogSnapshotV1 snapshot;
        ASSERT_EQ(storage::loadRunCatalogV1(subdirectory / "run-catalog-v1", snapshot).outcome,
                  storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_EQ(snapshot.generation, 3U);
        EXPECT_EQ(snapshot.activeRunId, domain::ExchangeRunId{1});
        EXPECT_EQ(snapshot.activeDisposition, storage::RunCatalogDisposition::FAIL_STOPPED);
        ExchangeRunController restarted;
        EXPECT_EQ(restarted.startupExistingRunV1(subdirectory / "run-catalog-v1").outcome,
                  ExistingRunStartupOutcome::EXPLICIT_OPERATION_REQUIRED);
        EXPECT_EQ(restarted.state(), ExchangeRunStartupState::UNAVAILABLE);
    }
}

TEST_F(ControllerCommandProcessingTest, PauseClosesAdmissionBeforeDurableAppend) {
    AppendProbe probe{.checkClosedOnSync = true};
    ExchangeRunController controller;
    probe.controller = &controller;
    ASSERT_EQ(startProbedRun(catalog(), probe, controller).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(2, 1, 1, 16'384, 4'096));
    const auto first = order("FIRST");
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, first).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);

    EXPECT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::RESULT_HANDOFF_BACKPRESSURE);
    drainPrivateResultHandoff(controller);
    EXPECT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::PAUSED);

    EXPECT_TRUE(probe.observedClosed);
    EXPECT_EQ(probe.syncs, 1);
    EXPECT_EQ(controller.admissionIndex()->size(), 1U);
    expectCorrelation(controller.admissionIndex()->completedResult(first), first, 1);
    expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, order("AFTER-GATE")));
}

TEST_F(ControllerCommandProcessingTest, PauseAppendFailurePreservesAcceptedFifoAndExactEvidence) {
    AppendProbe probe{.fault = AppendFault::SYNC};
    ExchangeRunController controller;
    probe.controller = &controller;
    ASSERT_EQ(startProbedRun(catalog(), probe, controller).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(3, 1, 1, 16'384, 4'096));
    const auto* state = controller.matchingState();
    const auto* writer = controller.journalWriter();
    const auto* admission = controller.admissionIndex();
    const auto before = state->snapshot();
    const auto first = order("FIRST");
    const auto failed = order("FAILED");
    const auto later = order("LATER");
    for (const auto& command : {first, failed, later}) {
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, command).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
    }

    const auto paused = controller.pauseRunV1();

    ASSERT_EQ(paused.outcome, RunPauseOutcome::PROCESSING_FAILED);
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::FAIL_STOPPED);
    const auto inspection = controller.inspectCommandProcessingV1();
    ASSERT_TRUE(inspection.appendFailure.has_value());
    EXPECT_EQ(inspection.appendFailure->outcome, storage::RunJournalAppendOutcome::RECOVERY_REQUIRED);
    EXPECT_EQ(inspection.appendFailure->systemError, EIO);
    ASSERT_TRUE(inspection.pendingCommand.has_value());
    EXPECT_EQ(inspection.pendingCommand->clientCommandId, failed.clientCommandId);
    EXPECT_FALSE(inspection.ingressEmpty);
    EXPECT_FALSE(inspection.matchingEmpty);
    EXPECT_EQ(inspection.queuedResults, 0U);
    EXPECT_EQ(state->snapshot(), before);
    EXPECT_EQ(controller.matchingState(), state);
    EXPECT_EQ(controller.journalWriter(), writer);
    EXPECT_EQ(controller.admissionIndex(), admission);
    EXPECT_EQ(writer->committedCommandCount(), 1U);
    EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{2});
    EXPECT_EQ(admission->completedResult(first), nullptr);
    expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, order("AFTER-FAILURE")));
    const int syncs = probe.syncs;
    EXPECT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::NOT_READY);
    EXPECT_FALSE(controller.advanceCommandProcessingV1());
    EXPECT_EQ(probe.syncs, syncs);
    storage::RunCatalogSnapshotV1 snapshot;
    ASSERT_EQ(storage::loadRunCatalogV1(catalog(), snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(snapshot.generation, 3U);
    EXPECT_EQ(snapshot.activeRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(snapshot.activeDisposition, storage::RunCatalogDisposition::FAIL_STOPPED);
}

TEST_F(ControllerCommandProcessingTest, PauseMatchingInvariantFailureRetainsPoppedCommandAndEvidence) {
    const detail::StartNewRunHooks startupHooks{
        nullptr,
        [](void*, domain::ExchangeRunId runId, std::uint64_t capacity,
           std::unique_ptr<matching_engine::MatchingState>& state,
           std::unique_ptr<admission::CommandAdmissionIndex>& admission) noexcept {
            const auto error = detail::constructEmptyRunStateV1(runId, capacity, state, admission);
            if (error.has_value()) {
                return error;
            }
            // Inject an impossible book/order-ID collision to exercise the fail-stop boundary.
            auto conflicting = order("INJECTED");
            conflicting.globalSequenceNumber = domain::CommandSequence{1};
            conflicting.orderId = domain::OrderId{1};
            try {
                (void)state->processCommand(conflicting);
            } catch (...) {
                return std::optional{StartNewRunOutcome::STATE_CONSTRUCTION_FAILED};
            }
            return std::optional<StartNewRunOutcome>{};
        },
        [](void*, const std::filesystem::path& path, const storage::PreparedNewRunJournalV1& prepared) noexcept {
            return storage::activatePreparedNewRunV1(path, prepared);
        },
        nullptr};
    ExchangeRunController controller;
    ASSERT_EQ(detail::startupNewRunV1WithHooks(catalog(), configuration(), &startupHooks, controller).outcome,
              NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(2, 1, 1, 16'384, 4'096));
    const auto* state = controller.matchingState();
    const auto before = state->snapshot();
    const auto first = order("FIRST");
    const auto later = order("LATER");
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, first).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, later).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);

    const auto paused = controller.pauseRunV1();

    EXPECT_EQ(paused.outcome, RunPauseOutcome::PROCESSING_FAILED);
    const auto inspection = controller.inspectCommandProcessingV1();
    ASSERT_NE(inspection.internalFailure, nullptr);
    EXPECT_FALSE(inspection.capacityTransitionPending);
    EXPECT_FALSE(inspection.capacityCatalogReplacement.has_value());
    try {
        std::rethrow_exception(inspection.internalFailure);
        FAIL() << "expected matching invariant failure";
    } catch (const std::logic_error& error) {
        EXPECT_STREQ(error.what(), "duplicate authoritative OrderId reached matching engine");
    }
    ASSERT_TRUE(inspection.inProgressMatchingCommand.has_value());
    EXPECT_EQ(inspection.inProgressMatchingCommand->clientCommandId, first.clientCommandId);
    EXPECT_EQ(inspection.inProgressMatchingCommand->globalSequenceNumber, domain::CommandSequence{1});
    ASSERT_TRUE(inspection.pendingCommand.has_value());
    EXPECT_EQ(inspection.pendingCommand->clientCommandId, later.clientCommandId);
    EXPECT_EQ(state->snapshot(), before);
    EXPECT_EQ(controller.journalWriter()->committedCommandCount(), 2U);
    EXPECT_EQ(controller.admissionIndex()->completedResult(first), nullptr);
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::FAIL_STOPPED);
    EXPECT_EQ(inspection.failStopCatalogPreconditionFailure, FailStopCatalogPreconditionFailure::NONE);
    ASSERT_TRUE(inspection.failStopCatalogLoad.has_value());
    EXPECT_EQ(inspection.failStopCatalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
    ASSERT_TRUE(inspection.failStopCatalogReplacement.has_value());
    EXPECT_EQ(inspection.failStopCatalogReplacement->outcome, storage::RunCatalogReplaceOutcome::COMMITTED);
    expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, order("AFTER-MATCH-FAIL")));
    EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, first).status,
              admission::AdmissionStatus::IDENTICAL_IN_FLIGHT);
    EXPECT_FALSE(controller.advanceCommandProcessingV1());
    storage::RunCatalogSnapshotV1 snapshot;
    ASSERT_EQ(storage::loadRunCatalogV1(catalog(), snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(snapshot.generation, 3U);
    EXPECT_EQ(snapshot.activeRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(snapshot.activeDisposition, storage::RunCatalogDisposition::FAIL_STOPPED);
}

TEST_F(ControllerCommandProcessingTest, CompletionInvariantFailurePublishesFailStoppedAndRetainsPendingResult) {
    const detail::StartNewRunHooks startupHooks{
        nullptr,
        [](void*, domain::ExchangeRunId runId, std::uint64_t capacity,
           std::unique_ptr<matching_engine::MatchingState>& state,
           std::unique_ptr<admission::CommandAdmissionIndex>& admission) noexcept {
            const auto error = detail::constructEmptyRunStateV1(runId, capacity, state, admission);
            if (error.has_value()) {
                return error;
            }
            try {
                state = std::make_unique<matching_engine::MatchingState>(domain::ExchangeRunId{runId.value() + 1});
            } catch (...) {
                return std::optional{StartNewRunOutcome::STATE_CONSTRUCTION_FAILED};
            }
            return std::optional<StartNewRunOutcome>{};
        },
        [](void*, const std::filesystem::path& path, const storage::PreparedNewRunJournalV1& prepared) noexcept {
            return storage::activatePreparedNewRunV1(path, prepared);
        },
        nullptr};
    ExchangeRunController controller;
    ASSERT_EQ(detail::startupNewRunV1WithHooks(catalog(), configuration(), &startupHooks, controller).outcome,
              NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    const auto* state = controller.matchingState();
    const auto* writer = controller.journalWriter();
    const auto* admission = controller.admissionIndex();
    const auto command = order("COMPLETION-FAILURE");
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, command).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);

    EXPECT_FALSE(controller.advanceCommandProcessingV1());

    EXPECT_EQ(controller.state(), ExchangeRunStartupState::FAIL_STOPPED);
    EXPECT_EQ(controller.matchingState(), state);
    EXPECT_EQ(controller.journalWriter(), writer);
    EXPECT_EQ(controller.admissionIndex(), admission);
    EXPECT_EQ(writer->committedCommandCount(), 1U);
    EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{2});
    EXPECT_EQ(state->snapshot().exchangeRunId, domain::ExchangeRunId{2});
    EXPECT_EQ(state->snapshot().activeOrders.size(), 1U);
    EXPECT_EQ(admission->completedResult(command), nullptr);
    const auto inspection = controller.inspectCommandProcessingV1();
    ASSERT_NE(inspection.internalFailure, nullptr);
    try {
        std::rethrow_exception(inspection.internalFailure);
        FAIL() << "expected completion invariant failure";
    } catch (const std::logic_error& error) {
        EXPECT_STREQ(error.what(), "admission completion invariant failure: CorrelationMismatch");
    }
    EXPECT_TRUE(inspection.pendingCompletion);
    EXPECT_FALSE(inspection.pendingResultProjection);
    EXPECT_EQ(inspection.pendingPrivateEventRecords, 0U);
    EXPECT_EQ(inspection.queuedPrivateEventRecords, 0U);
    EXPECT_EQ(inspection.pendingPublicTradeRecords, 0U);
    EXPECT_EQ(inspection.queuedPublicTradeRecords, 0U);
    EXPECT_FALSE(inspection.pendingMatchingResult);
    EXPECT_EQ(inspection.queuedResults, 0U);
    EXPECT_EQ(inspection.failStopCatalogPreconditionFailure, FailStopCatalogPreconditionFailure::NONE);
    ASSERT_TRUE(inspection.failStopCatalogReplacement.has_value());
    EXPECT_EQ(inspection.failStopCatalogReplacement->outcome, storage::RunCatalogReplaceOutcome::COMMITTED);

    EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, command).status,
              admission::AdmissionStatus::IDENTICAL_IN_FLIGHT);
    auto conflict = command;
    conflict.quantity = domain::Quantity{11};
    EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, conflict).status,
              admission::AdmissionStatus::CONFLICTING_REUSE);
    expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, order("AFTER-COMPLETION-FAILURE")));
    EXPECT_FALSE(controller.advanceCommandProcessingV1());
    EXPECT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::NOT_READY);
    EXPECT_EQ(controller.resumeRunV1().outcome, RunResumeOutcome::NOT_PAUSED);
    EXPECT_FALSE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));

    storage::RunCatalogSnapshotV1 snapshot;
    ASSERT_EQ(storage::loadRunCatalogV1(catalog(), snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(snapshot.generation, 3U);
    EXPECT_EQ(snapshot.activeRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(snapshot.activeDisposition, storage::RunCatalogDisposition::FAIL_STOPPED);
}

ssize_t failCatalogWrite(void*, int, const void*, std::size_t) noexcept {
    errno = EIO;
    return -1;
}

struct FailStopCatalogSyncProbe final {
    ExchangeRunController* controller{nullptr};
    sequencer::sequenceMessage retained{};
    int calls{0};
    bool closedThroughSync{true};
};

int failStopCatalogSync(void* context, const int descriptor) noexcept {
    auto& probe = *static_cast<FailStopCatalogSyncProbe*>(context);
    ++probe.calls;
    const auto rejected =
        probe.controller->submitCommandV1(domain::ExchangeRunId{1}, order("DURING-FAIL-STOP-PUBLISH"));
    const auto duplicate = probe.controller->submitCommandV1(domain::ExchangeRunId{1}, probe.retained);
    probe.closedThroughSync &=
        rejected.status == admission::AdmissionStatus::ADMISSION_UNAVAILABLE &&
        rejected.rejectionReason == domain::AdmissionRejectionReason::EXCHANGE_RUN_UNAVAILABLE &&
        duplicate.status == admission::AdmissionStatus::IDENTICAL_COMPLETED &&
        duplicate.originalResult == probe.controller->admissionIndex()->completedResult(probe.retained) &&
        probe.controller->state() == ExchangeRunStartupState::UNAVAILABLE;
    if (probe.calls == 2) {
        errno = EIO;
        return -1;
    }
    return ::fsync(descriptor);
}

TEST_F(ControllerCommandProcessingTest, FailStopCatalogFailuresRemainUnavailableWithExactNestedEvidence) {
    for (const bool uncertain : {false, true}) {
        const auto subdirectory = catalog().parent_path() / (uncertain ? "fail-stop-uncertain" : "fail-stop-definite");
        ASSERT_TRUE(std::filesystem::create_directory(subdirectory));
        const auto catalogPath = subdirectory / "run-catalog-v1";
        AppendProbe appendProbe{.fault = AppendFault::IMMEDIATE_WRITE};
        ExchangeRunController controller;
        appendProbe.controller = &controller;
        ASSERT_EQ(startProbedRun(catalogPath, appendProbe, controller).outcome, NewRunStartupOutcome::READY);
        ASSERT_TRUE(controller.installCommandProcessingV1(2, 2, 1, 16'384, 4'096));
        const auto* state = controller.matchingState();
        const auto* writer = controller.journalWriter();
        const auto* admission = controller.admissionIndex();
        const auto first = order("FIRST");
        const auto failed = order("FAIL-STOP-CATALOG");
        const auto later = order("LATER");
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, first).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_TRUE(controller.advanceCommandProcessingV1());
        const auto completed = admission->completedResult(first);
        expectCorrelation(completed, first, 1);
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, failed).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, later).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        FailStopCatalogSyncProbe catalogProbe{.controller = &controller, .retained = first};
        auto hooks = storage::detail::systemRunCatalogStorageHooks();
        if (uncertain) {
            hooks.context = &catalogProbe;
            hooks.syncFile = failStopCatalogSync;
        } else {
            hooks.writeFile = failCatalogWrite;
        }

        EXPECT_FALSE(detail::advanceCommandProcessingV1WithHooks(hooks, controller));

        EXPECT_EQ(controller.state(), ExchangeRunStartupState::UNAVAILABLE);
        EXPECT_EQ(controller.matchingState(), state);
        EXPECT_EQ(controller.journalWriter(), writer);
        EXPECT_EQ(controller.admissionIndex(), admission);
        EXPECT_EQ(state->snapshot().activeOrders.size(), 1U);
        EXPECT_EQ(writer->committedCommandCount(), 1U);
        EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{2});
        EXPECT_EQ(admission->completedResult(first), completed);
        const auto inspection = controller.inspectCommandProcessingV1();
        ASSERT_TRUE(inspection.appendFailure.has_value());
        EXPECT_EQ(inspection.appendFailure->outcome, storage::RunJournalAppendOutcome::IO_FAILURE);
        EXPECT_EQ(inspection.appendFailure->systemError, ENOSPC);
        ASSERT_TRUE(inspection.pendingCommand.has_value());
        EXPECT_EQ(inspection.pendingCommand->clientCommandId, failed.clientCommandId);
        EXPECT_FALSE(inspection.ingressEmpty);
        EXPECT_TRUE(inspection.matchingEmpty);
        EXPECT_EQ(inspection.failStopCatalogPreconditionFailure, FailStopCatalogPreconditionFailure::NONE);
        ASSERT_TRUE(inspection.failStopCatalogLoad.has_value());
        EXPECT_EQ(inspection.failStopCatalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
        ASSERT_TRUE(inspection.failStopCatalogReplacement.has_value());
        EXPECT_EQ(inspection.failStopCatalogReplacement->outcome,
                  uncertain ? storage::RunCatalogReplaceOutcome::UNCERTAIN
                            : storage::RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE);
        EXPECT_EQ(inspection.failStopCatalogReplacement->systemError, EIO);

        EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, failed).status,
                  admission::AdmissionStatus::IDENTICAL_IN_FLIGHT);
        const auto repeated = controller.submitCommandV1(domain::ExchangeRunId{1}, first);
        EXPECT_EQ(repeated.status, admission::AdmissionStatus::IDENTICAL_COMPLETED);
        EXPECT_EQ(repeated.originalResult, completed);
        auto conflict = failed;
        conflict.quantity = domain::Quantity{11};
        EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, conflict).status,
                  admission::AdmissionStatus::CONFLICTING_REUSE);
        expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, order("AFTER-FAIL-STOP-CATALOG")));
        const int writes = appendProbe.writes;
        const int syncs = appendProbe.syncs;
        EXPECT_FALSE(controller.advanceCommandProcessingV1());
        EXPECT_EQ(appendProbe.writes, writes);
        EXPECT_EQ(appendProbe.syncs, syncs);
        EXPECT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::NOT_READY);
        EXPECT_EQ(controller.resumeRunV1().outcome, RunResumeOutcome::NOT_PAUSED);
        EXPECT_FALSE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));

        storage::RunCatalogSnapshotV1 snapshot;
        ASSERT_EQ(storage::loadRunCatalogV1(catalogPath, snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_EQ(snapshot.generation, uncertain ? 3U : 2U);
        EXPECT_EQ(snapshot.activeRunId, domain::ExchangeRunId{1});
        EXPECT_EQ(snapshot.activeDisposition,
                  uncertain ? storage::RunCatalogDisposition::FAIL_STOPPED : storage::RunCatalogDisposition::OPEN);
        if (uncertain) {
            EXPECT_EQ(catalogProbe.calls, 2);
            EXPECT_TRUE(catalogProbe.closedThroughSync);
            ASSERT_TRUE(inspection.failStopCatalogReplacement->observedSnapshot.has_value());
            EXPECT_EQ(*inspection.failStopCatalogReplacement->observedSnapshot, snapshot);
            ExchangeRunController restarted;
            EXPECT_EQ(restarted.startupExistingRunV1(catalogPath).outcome,
                      ExistingRunStartupOutcome::EXPLICIT_OPERATION_REQUIRED);
            EXPECT_EQ(restarted.state(), ExchangeRunStartupState::UNAVAILABLE);
        }
    }
}

enum class FailStopCatalogPrecondition { MISSING, RUN_ID_MISMATCH, NOT_OPEN, GENERATION_EXHAUSTED };

void overwriteCatalog(const std::filesystem::path& path, const storage::RunCatalogSnapshotV1& snapshot) {
    storage::RunCatalogV1Bytes encoded{};
    ASSERT_EQ(storage::encodeRunCatalogV1(snapshot, encoded), storage::RunCatalogCodecError::NONE);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    output.flush();
    ASSERT_TRUE(output.good());
}

TEST_F(ControllerCommandProcessingTest, FailStopCatalogPreconditionFailuresRetainOwnersAndExactEvidence) {
    for (const auto condition :
         {FailStopCatalogPrecondition::MISSING, FailStopCatalogPrecondition::RUN_ID_MISMATCH,
          FailStopCatalogPrecondition::NOT_OPEN, FailStopCatalogPrecondition::GENERATION_EXHAUSTED}) {
        const auto subdirectory = catalog().parent_path() / std::to_string(static_cast<int>(condition));
        ASSERT_TRUE(std::filesystem::create_directory(subdirectory));
        const auto catalogPath = subdirectory / "run-catalog-v1";
        AppendProbe appendProbe{.fault = AppendFault::IMMEDIATE_WRITE};
        ExchangeRunController controller;
        appendProbe.controller = &controller;
        ASSERT_EQ(startProbedRun(catalogPath, appendProbe, controller).outcome, NewRunStartupOutcome::READY);
        ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
        const auto* state = controller.matchingState();
        const auto* writer = controller.journalWriter();
        const auto* admission = controller.admissionIndex();
        const auto completedCommand = order("FIRST");
        const auto failed = order("PRECONDITION-FAILURE");
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, completedCommand).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_TRUE(controller.advanceCommandProcessingV1());
        const auto completed = admission->completedResult(completedCommand);
        expectCorrelation(completed, completedCommand, 1);

        storage::RunCatalogSnapshotV1 expected{};
        if (condition == FailStopCatalogPrecondition::MISSING) {
            ASSERT_TRUE(std::filesystem::remove(catalogPath));
        } else {
            expected = {.generation = condition == FailStopCatalogPrecondition::GENERATION_EXHAUSTED
                                          ? std::numeric_limits<std::uint64_t>::max()
                                          : 3,
                        .lastReservedRunId = condition == FailStopCatalogPrecondition::RUN_ID_MISMATCH
                                                 ? domain::ExchangeRunId{2}
                                                 : domain::ExchangeRunId{1},
                        .activeRunId = condition == FailStopCatalogPrecondition::RUN_ID_MISMATCH
                                           ? domain::ExchangeRunId{2}
                                           : domain::ExchangeRunId{1},
                        .activeDisposition = condition == FailStopCatalogPrecondition::NOT_OPEN
                                                 ? storage::RunCatalogDisposition::PAUSED
                                                 : storage::RunCatalogDisposition::OPEN,
                        .retainedStoppedRunId = std::nullopt};
            overwriteCatalog(catalogPath, expected);
        }
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, failed).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);

        EXPECT_FALSE(controller.advanceCommandProcessingV1());

        EXPECT_EQ(controller.state(), ExchangeRunStartupState::UNAVAILABLE);
        EXPECT_EQ(controller.matchingState(), state);
        EXPECT_EQ(controller.journalWriter(), writer);
        EXPECT_EQ(controller.admissionIndex(), admission);
        EXPECT_EQ(admission->completedResult(completedCommand), completed);
        EXPECT_EQ(writer->committedCommandCount(), 1U);
        EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{2});
        EXPECT_EQ(state->snapshot().activeOrders.size(), 1U);
        const auto inspection = controller.inspectCommandProcessingV1();
        ASSERT_TRUE(inspection.appendFailure.has_value());
        EXPECT_EQ(inspection.appendFailure->outcome, storage::RunJournalAppendOutcome::IO_FAILURE);
        ASSERT_TRUE(inspection.pendingCommand.has_value());
        EXPECT_EQ(inspection.pendingCommand->clientCommandId, failed.clientCommandId);
        ASSERT_TRUE(inspection.failStopCatalogLoad.has_value());
        EXPECT_EQ(inspection.failStopCatalogPreconditionFailure,
                  condition == FailStopCatalogPrecondition::RUN_ID_MISMATCH
                      ? FailStopCatalogPreconditionFailure::ACTIVE_RUN_ID_MISMATCH
                  : condition == FailStopCatalogPrecondition::NOT_OPEN
                      ? FailStopCatalogPreconditionFailure::CATALOG_NOT_OPEN
                  : condition == FailStopCatalogPrecondition::GENERATION_EXHAUSTED
                      ? FailStopCatalogPreconditionFailure::GENERATION_EXHAUSTED
                      : FailStopCatalogPreconditionFailure::NONE);
        EXPECT_EQ(inspection.failStopCatalogLoad->outcome, condition == FailStopCatalogPrecondition::MISSING
                                                               ? storage::RunCatalogLoadOutcome::MISSING
                                                               : storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_FALSE(inspection.failStopCatalogReplacement.has_value());
        const auto repeated = controller.submitCommandV1(domain::ExchangeRunId{1}, completedCommand);
        EXPECT_EQ(repeated.status, admission::AdmissionStatus::IDENTICAL_COMPLETED);
        EXPECT_EQ(repeated.originalResult, completed);
        expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, order("AFTER-PRECONDITION-FAILURE")));
        EXPECT_FALSE(controller.advanceCommandProcessingV1());
        EXPECT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::NOT_READY);
        EXPECT_EQ(controller.resumeRunV1().outcome, RunResumeOutcome::NOT_PAUSED);
        EXPECT_FALSE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));

        storage::RunCatalogSnapshotV1 persisted;
        const auto load = storage::loadRunCatalogV1(catalogPath, persisted);
        if (condition == FailStopCatalogPrecondition::MISSING) {
            EXPECT_EQ(load.outcome, storage::RunCatalogLoadOutcome::MISSING);
        } else {
            ASSERT_EQ(load.outcome, storage::RunCatalogLoadOutcome::LOADED);
            EXPECT_EQ(persisted, expected);
        }
    }
}

struct PauseCatalogSyncProbe final {
    ExchangeRunController* controller{nullptr};
    sequencer::sequenceMessage completed{};
    int calls{0};
    bool closedThroughSync{true};
};

int pauseCatalogSync(void* context, const int descriptor) noexcept {
    auto& probe = *static_cast<PauseCatalogSyncProbe*>(context);
    ++probe.calls;
    const auto rejected = probe.controller->submitCommandV1(domain::ExchangeRunId{1}, order("DURING-PUBLISH"));
    probe.closedThroughSync &= rejected.status == admission::AdmissionStatus::ADMISSION_UNAVAILABLE &&
                               rejected.rejectionReason == domain::AdmissionRejectionReason::EXCHANGE_RUN_UNAVAILABLE &&
                               probe.controller->state() == ExchangeRunStartupState::READY &&
                               probe.controller->admissionIndex()->completedResult(probe.completed) != nullptr;
    if (probe.calls == 2) {
        errno = EIO;
        return -1;
    }
    return ::fsync(descriptor);
}

TEST_F(ControllerCommandProcessingTest, PauseCatalogFailuresKeepGateClosedAndRetainCompletedWork) {
    for (const bool uncertain : {false, true}) {
        const auto subdirectory = catalog().parent_path() / (uncertain ? "uncertain" : "definite");
        ASSERT_TRUE(std::filesystem::create_directory(subdirectory));
        const auto catalogPath = subdirectory / "run-catalog-v1";
        ExchangeRunController controller;
        ASSERT_EQ(controller.startupNewRunV1(catalogPath, configuration()).outcome, NewRunStartupOutcome::READY);
        ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
        const auto command = order("CATALOG");
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, command).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        const auto* state = controller.matchingState();
        const auto* writer = controller.journalWriter();
        const auto* admission = controller.admissionIndex();
        PauseCatalogSyncProbe probe{.controller = &controller, .completed = command};
        auto hooks = storage::detail::systemRunCatalogStorageHooks();
        if (uncertain) {
            hooks.context = &probe;
            hooks.syncFile = pauseCatalogSync;
        } else {
            hooks.writeFile = failCatalogWrite;
        }

        ASSERT_EQ(detail::pauseRunV1WithHooks(hooks, controller).outcome, RunPauseOutcome::RESULT_HANDOFF_BACKPRESSURE);
        drainPrivateResultHandoff(controller);
        const auto paused = detail::pauseRunV1WithHooks(hooks, controller);

        ASSERT_EQ(paused.outcome, RunPauseOutcome::CATALOG_REPLACE_FAILED);
        ASSERT_TRUE(paused.catalogReplacement.has_value());
        EXPECT_EQ(paused.catalogReplacement->outcome,
                  uncertain ? storage::RunCatalogReplaceOutcome::UNCERTAIN
                            : storage::RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE);
        EXPECT_EQ(paused.catalogReplacement->systemError, EIO);
        EXPECT_EQ(controller.state(), ExchangeRunStartupState::UNAVAILABLE);
        EXPECT_EQ(controller.matchingState(), state);
        EXPECT_EQ(controller.journalWriter(), writer);
        EXPECT_EQ(controller.admissionIndex(), admission);
        expectCorrelation(admission->completedResult(command), command, 1);
        EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{2});
        EXPECT_EQ(state->snapshot().activeOrders.size(), 1U);
        expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, order("AFTER-CATALOG-FAIL")));
        EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, command).originalResult,
                  admission->completedResult(command));
        EXPECT_FALSE(controller.advanceCommandProcessingV1());
        EXPECT_EQ(controller.resumeRunV1().outcome, RunResumeOutcome::NOT_PAUSED);
        storage::RunCatalogSnapshotV1 snapshot;
        ASSERT_EQ(storage::loadRunCatalogV1(catalogPath, snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_EQ(snapshot.activeDisposition,
                  uncertain ? storage::RunCatalogDisposition::PAUSED : storage::RunCatalogDisposition::OPEN);
        if (uncertain) {
            EXPECT_EQ(probe.calls, 2);
            EXPECT_TRUE(probe.closedThroughSync);
            ASSERT_TRUE(paused.catalogReplacement->observedSnapshot.has_value());
            EXPECT_EQ(*paused.catalogReplacement->observedSnapshot, snapshot);
        }
    }
}

struct CapacityCatalogSyncProbe final {
    ExchangeRunController* controller{nullptr};
    sequencer::sequenceMessage completed{};
    int calls{0};
    bool closedThroughSync{true};
};

int capacityCatalogSync(void* context, const int descriptor) noexcept {
    auto& probe = *static_cast<CapacityCatalogSyncProbe*>(context);
    ++probe.calls;
    const auto rejected = probe.controller->submitCommandV1(domain::ExchangeRunId{1}, order("DURING-CAPACITY"));
    probe.closedThroughSync &= rejected.status == admission::AdmissionStatus::ADMISSION_UNAVAILABLE &&
                               rejected.rejectionReason == domain::AdmissionRejectionReason::RUN_CAPACITY_REACHED &&
                               probe.controller->state() == ExchangeRunStartupState::READY &&
                               probe.controller->admissionIndex()->completedResult(probe.completed) != nullptr;
    if (probe.calls == 2) {
        errno = EIO;
        return -1;
    }
    return ::fsync(descriptor);
}

TEST_F(ControllerCommandProcessingTest, CapacityCatalogFailuresRetainOwnersResultsAndExactEvidence) {
    for (const bool uncertain : {false, true}) {
        const auto subdirectory = catalog().parent_path() / (uncertain ? "capacity-uncertain" : "capacity-definite");
        ASSERT_TRUE(std::filesystem::create_directory(subdirectory));
        const auto catalogPath = subdirectory / "run-catalog-v1";
        ExchangeRunController controller;
        ASSERT_EQ(controller.startupNewRunV1(catalogPath, configuration(1, 64 * 1024)).outcome,
                  NewRunStartupOutcome::READY);
        ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
        const auto command = order("FINAL");
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, command).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        const auto* state = controller.matchingState();
        const auto* writer = controller.journalWriter();
        const auto* admission = controller.admissionIndex();
        CapacityCatalogSyncProbe probe{.controller = &controller, .completed = command};
        auto hooks = storage::detail::systemRunCatalogStorageHooks();
        if (uncertain) {
            hooks.context = &probe;
            hooks.syncFile = capacityCatalogSync;
        } else {
            hooks.writeFile = failCatalogWrite;
        }

        EXPECT_FALSE(detail::advanceCommandProcessingV1WithHooks(hooks, controller));

        EXPECT_EQ(controller.state(), ExchangeRunStartupState::UNAVAILABLE);
        EXPECT_EQ(controller.matchingState(), state);
        EXPECT_EQ(controller.journalWriter(), writer);
        EXPECT_EQ(controller.admissionIndex(), admission);
        expectCorrelation(admission->completedResult(command), command, 1);
        EXPECT_EQ(writer->committedCommandCount(), 1U);
        EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{2});
        EXPECT_EQ(state->snapshot().activeOrders.size(), 1U);
        const auto inspection = controller.inspectCommandProcessingV1();
        EXPECT_TRUE(inspection.capacityTransitionPending);
        ASSERT_TRUE(inspection.capacityCatalogLoad.has_value());
        EXPECT_EQ(inspection.capacityCatalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
        ASSERT_TRUE(inspection.capacityCatalogReplacement.has_value());
        EXPECT_EQ(inspection.capacityCatalogReplacement->outcome,
                  uncertain ? storage::RunCatalogReplaceOutcome::UNCERTAIN
                            : storage::RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE);
        EXPECT_EQ(inspection.capacityCatalogReplacement->systemError, EIO);
        expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, order("AFTER-CAPACITY-FAIL")));
        const auto duplicate = controller.submitCommandV1(domain::ExchangeRunId{1}, command);
        EXPECT_EQ(duplicate.status, admission::AdmissionStatus::IDENTICAL_COMPLETED);
        EXPECT_EQ(duplicate.originalResult, admission->completedResult(command));
        EXPECT_FALSE(controller.advanceCommandProcessingV1());
        EXPECT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::NOT_READY);
        EXPECT_EQ(controller.resumeRunV1().outcome, RunResumeOutcome::NOT_PAUSED);
        storage::RunCatalogSnapshotV1 snapshot;
        ASSERT_EQ(storage::loadRunCatalogV1(catalogPath, snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_EQ(snapshot.activeDisposition,
                  uncertain ? storage::RunCatalogDisposition::CAPACITY_REACHED : storage::RunCatalogDisposition::OPEN);
        if (uncertain) {
            EXPECT_EQ(probe.calls, 2);
            EXPECT_TRUE(probe.closedThroughSync);
            ASSERT_TRUE(inspection.capacityCatalogReplacement->observedSnapshot.has_value());
            EXPECT_EQ(*inspection.capacityCatalogReplacement->observedSnapshot, snapshot);
        }
    }
}

void corruptLastJournalByte(const std::filesystem::path& path) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(stream.is_open());
    stream.seekg(-1, std::ios::end);
    char value{};
    stream.read(&value, 1);
    ASSERT_TRUE(stream.good());
    value = static_cast<char>(value ^ 0x5A);
    stream.seekp(-1, std::ios::end);
    stream.write(&value, 1);
    stream.flush();
    ASSERT_TRUE(stream.good());
}

TEST_F(ControllerCommandProcessingTest, ExplicitFailedRunRecoveryUsesOnlyAuthoritativeJournalThenResumesSeparately) {
    for (const auto fault : {AppendFault::IMMEDIATE_WRITE, AppendFault::PARTIAL_WRITE, AppendFault::SYNC}) {
        const auto subdirectory = catalog().parent_path() / ("recover-" + std::to_string(static_cast<int>(fault)));
        ASSERT_TRUE(std::filesystem::create_directory(subdirectory));
        const auto catalogPath = subdirectory / "run-catalog-v1";
        const auto journalPath = subdirectory / "run-1.fxjr";
        AppendProbe probe{.fault = fault};
        ExchangeRunController controller;
        probe.controller = &controller;
        ASSERT_EQ(startProbedRun(catalogPath, probe, controller).outcome, NewRunStartupOutcome::READY);
        ASSERT_TRUE(controller.installCommandProcessingV1(3, 3, 1, 16'384, 4'096));
        const auto first = order("FIRST");
        const auto failed = order("FAILED");
        const auto queued = order("QUEUED");
        for (const auto& command : {first, failed, queued}) {
            ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, command).status,
                      admission::AdmissionStatus::FIRST_SUBMISSION);
        }
        ASSERT_FALSE(controller.advanceCommandProcessingV1());
        ASSERT_EQ(controller.state(), ExchangeRunStartupState::FAIL_STOPPED);
        const auto failedInspection = controller.inspectCommandProcessingV1();
        ASSERT_TRUE(failedInspection.appendFailure.has_value());
        ASSERT_TRUE(failedInspection.pendingCommand.has_value());
        EXPECT_EQ(failedInspection.pendingCommand->clientCommandId, failed.clientCommandId);
        EXPECT_FALSE(failedInspection.ingressEmpty);

        const auto recovered = controller.recoverFailedRunV1(catalogPath);

        ASSERT_EQ(recovered.outcome, FailedRunRecoveryOutcome::PAUSED);
        ASSERT_TRUE(recovered.originalProcessingInspection.has_value());
        ASSERT_TRUE(recovered.originalProcessingInspection->appendFailure.has_value());
        EXPECT_EQ(recovered.originalProcessingInspection->appendFailure->outcome,
                  failedInspection.appendFailure->outcome);
        EXPECT_EQ(recovered.originalProcessingInspection->appendFailure->systemError,
                  failedInspection.appendFailure->systemError);
        ASSERT_TRUE(recovered.originalProcessingInspection->pendingCommand.has_value());
        EXPECT_EQ(recovered.originalProcessingInspection->pendingCommand->clientCommandId,
                  failedInspection.pendingCommand->clientCommandId);
        EXPECT_EQ(recovered.originalProcessingInspection->pendingCommand->globalSequenceNumber,
                  failedInspection.pendingCommand->globalSequenceNumber);
        ASSERT_TRUE(recovered.catalogLoad.has_value());
        EXPECT_EQ(recovered.catalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
        ASSERT_TRUE(recovered.recovery.has_value());
        EXPECT_EQ(recovered.recovery->outcome, recovery::RecoveredRunOutcome::RECOVERED);
        EXPECT_EQ(recovered.recovery->preparation.validation.outcome,
                  fault == AppendFault::PARTIAL_WRITE ? storage::RunJournalLoadOutcome::INCOMPLETE_TAIL
                                                      : storage::RunJournalLoadOutcome::VALID);
        EXPECT_EQ(recovered.recovery->preparation.preservedTailBytes.size(),
                  fault == AppendFault::PARTIAL_WRITE ? 7U : 0U);
        ASSERT_TRUE(recovered.catalogReplacement.has_value());
        EXPECT_EQ(recovered.catalogReplacement->outcome, storage::RunCatalogReplaceOutcome::COMMITTED);
        EXPECT_EQ(controller.state(), ExchangeRunStartupState::PAUSED);
        EXPECT_FALSE(controller.inspectCommandProcessingV1().installed);
        ASSERT_NE(controller.journalWriter(), nullptr);
        ASSERT_NE(controller.matchingState(), nullptr);
        ASSERT_NE(controller.admissionIndex(), nullptr);
        const std::size_t authoritativeCommands = fault == AppendFault::SYNC ? 2U : 1U;
        EXPECT_EQ(controller.journalWriter()->committedCommandCount(), authoritativeCommands);
        EXPECT_EQ(controller.journalWriter()->nextCommandSequence(),
                  domain::CommandSequence{authoritativeCommands + 1});
        EXPECT_EQ(controller.admissionIndex()->size(), authoritativeCommands);
        EXPECT_EQ(controller.matchingState()->snapshot().activeOrders.size(), authoritativeCommands);
        expectCorrelation(controller.admissionIndex()->completedResult(first), first, 1);
        if (fault == AppendFault::SYNC) {
            expectCorrelation(controller.admissionIndex()->completedResult(failed), failed, 2);
        } else {
            EXPECT_EQ(controller.admissionIndex()->completedResult(failed), nullptr);
        }
        EXPECT_EQ(controller.admissionIndex()->completedResult(queued), nullptr);
        EXPECT_TRUE(controller.matchingState()->invariantsHold());

        storage::LoadedRunJournalV1 loaded;
        ASSERT_EQ(storage::loadRunJournalV1(journalPath, loaded).outcome, storage::RunJournalLoadOutcome::VALID);
        ASSERT_EQ(loaded.commands.size(), authoritativeCommands);
        EXPECT_EQ(std::get<storage::JournalNewOrderV1>(loaded.commands[0]).clientCommandId, *first.clientCommandId);
        if (fault == AppendFault::SYNC) {
            EXPECT_EQ(std::get<storage::JournalNewOrderV1>(loaded.commands[1]).clientCommandId,
                      *failed.clientCommandId);
        }
        storage::RunCatalogSnapshotV1 snapshot;
        ASSERT_EQ(storage::loadRunCatalogV1(catalogPath, snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_EQ(snapshot.generation, 4U);
        EXPECT_EQ(snapshot.activeRunId, domain::ExchangeRunId{1});
        EXPECT_EQ(snapshot.activeDisposition, storage::RunCatalogDisposition::PAUSED);

        ASSERT_TRUE(controller.installCommandProcessingV1(3, 3, 1, 16'384, 4'096));
        EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, first).status,
                  admission::AdmissionStatus::IDENTICAL_COMPLETED);
        expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, queued));
        ASSERT_EQ(controller.resumeRunV1().outcome, RunResumeOutcome::READY);
        EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, failed).status,
                  fault == AppendFault::SYNC ? admission::AdmissionStatus::IDENTICAL_COMPLETED
                                             : admission::AdmissionStatus::FIRST_SUBMISSION);
        if (fault != AppendFault::SYNC) {
            ASSERT_TRUE(controller.advanceCommandProcessingV1());
            expectCorrelation(controller.admissionIndex()->completedResult(failed), failed, 2);
        }
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, queued).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_TRUE(controller.advanceCommandProcessingV1());
        expectCorrelation(controller.admissionIndex()->completedResult(queued), queued, 3);
        EXPECT_EQ(controller.recoverFailedRunV1(catalogPath).outcome, FailedRunRecoveryOutcome::NOT_RECOVERABLE);
    }
}

TEST_F(ControllerCommandProcessingTest, CorruptFailedRunJournalPublishesRecoveryFailedAndExposesNoState) {
    AppendProbe probe{.fault = AppendFault::IMMEDIATE_WRITE};
    ExchangeRunController controller;
    probe.controller = &controller;
    ASSERT_EQ(startProbedRun(catalog(), probe, controller).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(2, 2, 1, 16'384, 4'096));
    const auto first = order("FIRST");
    const auto failed = order("FAILED");
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, first).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, failed).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_FALSE(controller.advanceCommandProcessingV1());
    ASSERT_EQ(controller.state(), ExchangeRunStartupState::FAIL_STOPPED);
    corruptLastJournalByte(journal());

    const auto recovered = controller.recoverFailedRunV1(catalog());

    ASSERT_EQ(recovered.outcome, FailedRunRecoveryOutcome::RECOVERY_FAILED);
    ASSERT_TRUE(recovered.originalProcessingInspection.has_value());
    ASSERT_TRUE(recovered.originalProcessingInspection->appendFailure.has_value());
    EXPECT_EQ(recovered.originalProcessingInspection->appendFailure->outcome,
              storage::RunJournalAppendOutcome::IO_FAILURE);
    ASSERT_TRUE(recovered.recovery.has_value());
    EXPECT_EQ(recovered.recovery->outcome, recovery::RecoveredRunOutcome::PREPARATION_FAILED);
    EXPECT_EQ(recovered.recovery->preparation.outcome, storage::RunJournalRecoveryOutcome::CORRUPTION);
    EXPECT_EQ(recovered.recovery->preparation.validation.outcome, storage::RunJournalLoadOutcome::CORRUPTION);
    ASSERT_TRUE(recovered.catalogReplacement.has_value());
    EXPECT_EQ(recovered.catalogReplacement->outcome, storage::RunCatalogReplaceOutcome::COMMITTED);
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::RECOVERY_FAILED);
    EXPECT_EQ(controller.journalWriter(), nullptr);
    EXPECT_EQ(controller.matchingState(), nullptr);
    EXPECT_EQ(controller.admissionIndex(), nullptr);
    EXPECT_FALSE(controller.inspectCommandProcessingV1().installed);
    expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, first));
    EXPECT_FALSE(controller.advanceCommandProcessingV1());
    EXPECT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::NOT_READY);
    EXPECT_EQ(controller.resumeRunV1().outcome, RunResumeOutcome::NOT_PAUSED);
    EXPECT_FALSE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    storage::RunCatalogSnapshotV1 snapshot;
    ASSERT_EQ(storage::loadRunCatalogV1(catalog(), snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(snapshot.generation, 4U);
    EXPECT_EQ(snapshot.activeRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(snapshot.activeDisposition, storage::RunCatalogDisposition::RECOVERY_FAILED);

    const auto repeated = controller.recoverFailedRunV1(catalog());
    EXPECT_EQ(repeated.outcome, FailedRunRecoveryOutcome::RECOVERY_FAILED);
    ASSERT_TRUE(repeated.recovery.has_value());
    EXPECT_EQ(repeated.recovery->preparation.outcome, storage::RunJournalRecoveryOutcome::CORRUPTION);
    EXPECT_FALSE(repeated.catalogReplacement.has_value());
    storage::RunCatalogSnapshotV1 unchanged;
    ASSERT_EQ(storage::loadRunCatalogV1(catalog(), unchanged).outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(unchanged, snapshot);

    ExchangeRunController restarted;
    EXPECT_EQ(restarted.startupExistingRunV1(catalog()).outcome,
              ExistingRunStartupOutcome::EXPLICIT_OPERATION_REQUIRED);
}

enum class FailedRunRecoveryPrecondition {
    MISSING,
    CATALOG_BINDING_MISMATCH,
    RUN_ID_MISMATCH,
    NOT_FAILED,
    GENERATION_EXHAUSTED
};

TEST_F(ControllerCommandProcessingTest, FailedRunRecoveryCatalogPreflightRefusesWithoutReleasingOwners) {
    for (const auto condition :
         {FailedRunRecoveryPrecondition::MISSING, FailedRunRecoveryPrecondition::CATALOG_BINDING_MISMATCH,
          FailedRunRecoveryPrecondition::RUN_ID_MISMATCH, FailedRunRecoveryPrecondition::NOT_FAILED,
          FailedRunRecoveryPrecondition::GENERATION_EXHAUSTED}) {
        const auto subdirectory =
            catalog().parent_path() / ("recovery-preflight-" + std::to_string(static_cast<int>(condition)));
        ASSERT_TRUE(std::filesystem::create_directory(subdirectory));
        const auto catalogPath = subdirectory / "run-catalog-v1";
        const auto journalPath = subdirectory / "run-1.fxjr";
        AppendProbe probe{.fault = AppendFault::IMMEDIATE_WRITE};
        ExchangeRunController controller;
        probe.controller = &controller;
        ASSERT_EQ(startProbedRun(catalogPath, probe, controller).outcome, NewRunStartupOutcome::READY);
        ASSERT_TRUE(controller.installCommandProcessingV1(2, 2, 1, 16'384, 4'096));
        const auto first = order("FIRST");
        const auto failed = order("FAILED");
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, first).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, failed).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_FALSE(controller.advanceCommandProcessingV1());
        ASSERT_EQ(controller.state(), ExchangeRunStartupState::FAIL_STOPPED);
        const auto* state = controller.matchingState();
        const auto* writer = controller.journalWriter();
        const auto* admission = controller.admissionIndex();
        const auto journalBytes = std::filesystem::file_size(journalPath);

        if (condition == FailedRunRecoveryPrecondition::MISSING) {
            ASSERT_TRUE(std::filesystem::remove(catalogPath));
        } else {
            overwriteCatalog(
                catalogPath,
                {.generation = condition == FailedRunRecoveryPrecondition::GENERATION_EXHAUSTED
                                   ? std::numeric_limits<std::uint64_t>::max()
                                   : 4,
                 .lastReservedRunId = condition == FailedRunRecoveryPrecondition::RUN_ID_MISMATCH
                                          ? domain::ExchangeRunId{2}
                                          : domain::ExchangeRunId{1},
                 .activeRunId = condition == FailedRunRecoveryPrecondition::RUN_ID_MISMATCH ? domain::ExchangeRunId{2}
                                                                                            : domain::ExchangeRunId{1},
                 .activeDisposition = condition == FailedRunRecoveryPrecondition::NOT_FAILED
                                          ? storage::RunCatalogDisposition::PAUSED
                                          : storage::RunCatalogDisposition::FAIL_STOPPED,
                 .retainedStoppedRunId = std::nullopt});
        }

        const auto recovered =
            controller.recoverFailedRunV1(condition == FailedRunRecoveryPrecondition::CATALOG_BINDING_MISMATCH
                                              ? subdirectory / "different-run-catalog-v1"
                                              : catalogPath);

        EXPECT_EQ(recovered.outcome, condition == FailedRunRecoveryPrecondition::MISSING
                                         ? FailedRunRecoveryOutcome::CATALOG_LOAD_FAILED
                                     : condition == FailedRunRecoveryPrecondition::CATALOG_BINDING_MISMATCH
                                         ? FailedRunRecoveryOutcome::CATALOG_BINDING_MISMATCH
                                     : condition == FailedRunRecoveryPrecondition::RUN_ID_MISMATCH
                                         ? FailedRunRecoveryOutcome::ACTIVE_RUN_ID_MISMATCH
                                     : condition == FailedRunRecoveryPrecondition::NOT_FAILED
                                         ? FailedRunRecoveryOutcome::CATALOG_NOT_FAILED
                                         : FailedRunRecoveryOutcome::GENERATION_EXHAUSTED);
        ASSERT_TRUE(recovered.originalProcessingInspection.has_value());
        ASSERT_TRUE(recovered.originalProcessingInspection->appendFailure.has_value());
        EXPECT_EQ(controller.state(), ExchangeRunStartupState::FAIL_STOPPED);
        EXPECT_EQ(controller.matchingState(), state);
        EXPECT_EQ(controller.journalWriter(), writer);
        EXPECT_EQ(controller.admissionIndex(), admission);
        EXPECT_TRUE(controller.inspectCommandProcessingV1().installed);
        EXPECT_EQ(std::filesystem::file_size(journalPath), journalBytes);
        EXPECT_FALSE(recovered.recovery.has_value());
        EXPECT_FALSE(recovered.catalogReplacement.has_value());
    }
}

struct RecoveryCatalogSyncProbe final {
    ExchangeRunController* controller{nullptr};
    int calls{0};
    bool unavailableWithoutOwners{true};
};

int recoveryCatalogSync(void* context, const int descriptor) noexcept {
    auto& probe = *static_cast<RecoveryCatalogSyncProbe*>(context);
    ++probe.calls;
    probe.unavailableWithoutOwners &=
        probe.controller->state() == ExchangeRunStartupState::UNAVAILABLE &&
        probe.controller->journalWriter() == nullptr && probe.controller->matchingState() == nullptr &&
        probe.controller->admissionIndex() == nullptr && !probe.controller->inspectCommandProcessingV1().installed;
    if (probe.calls == 2) {
        errno = EIO;
        return -1;
    }
    return ::fsync(descriptor);
}

TEST_F(ControllerCommandProcessingTest, FailedRunRecoveryPublicationFailuresRetainBothLayersOfEvidence) {
    for (const bool corrupt : {false, true}) {
        for (const bool uncertain : {false, true}) {
            const auto subdirectory =
                catalog().parent_path() / (std::string{"recovery-publish-"} + (corrupt ? "corrupt-" : "valid-") +
                                           (uncertain ? "uncertain" : "definite"));
            ASSERT_TRUE(std::filesystem::create_directory(subdirectory));
            const auto catalogPath = subdirectory / "run-catalog-v1";
            const auto journalPath = subdirectory / "run-1.fxjr";
            AppendProbe appendProbe{.fault = AppendFault::IMMEDIATE_WRITE};
            ExchangeRunController controller;
            appendProbe.controller = &controller;
            ASSERT_EQ(startProbedRun(catalogPath, appendProbe, controller).outcome, NewRunStartupOutcome::READY);
            ASSERT_TRUE(controller.installCommandProcessingV1(2, 2, 1, 16'384, 4'096));
            const auto first = order("FIRST");
            const auto failed = order("FAILED");
            ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, first).status,
                      admission::AdmissionStatus::FIRST_SUBMISSION);
            ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, failed).status,
                      admission::AdmissionStatus::FIRST_SUBMISSION);
            ASSERT_FALSE(controller.advanceCommandProcessingV1());
            ASSERT_EQ(controller.state(), ExchangeRunStartupState::FAIL_STOPPED);
            if (corrupt) {
                corruptLastJournalByte(journalPath);
            }
            RecoveryCatalogSyncProbe syncProbe{.controller = &controller};
            auto hooks = storage::detail::systemRunCatalogStorageHooks();
            if (uncertain) {
                hooks.context = &syncProbe;
                hooks.syncFile = recoveryCatalogSync;
            } else {
                hooks.writeFile = failCatalogWrite;
            }

            const auto recovered = detail::recoverFailedRunV1WithHooks(catalogPath, hooks, controller);

            EXPECT_EQ(recovered.outcome, FailedRunRecoveryOutcome::CATALOG_REPLACE_FAILED);
            ASSERT_TRUE(recovered.originalProcessingInspection.has_value());
            ASSERT_TRUE(recovered.originalProcessingInspection->appendFailure.has_value());
            ASSERT_TRUE(recovered.recovery.has_value());
            EXPECT_EQ(recovered.recovery->outcome, corrupt ? recovery::RecoveredRunOutcome::PREPARATION_FAILED
                                                           : recovery::RecoveredRunOutcome::RECOVERED);
            ASSERT_TRUE(recovered.catalogReplacement.has_value());
            EXPECT_EQ(recovered.catalogReplacement->outcome,
                      uncertain ? storage::RunCatalogReplaceOutcome::UNCERTAIN
                                : storage::RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE);
            EXPECT_EQ(recovered.catalogReplacement->systemError, EIO);
            EXPECT_EQ(controller.state(), ExchangeRunStartupState::UNAVAILABLE);
            EXPECT_EQ(controller.journalWriter(), nullptr);
            EXPECT_EQ(controller.matchingState(), nullptr);
            EXPECT_EQ(controller.admissionIndex(), nullptr);
            EXPECT_FALSE(controller.inspectCommandProcessingV1().installed);
            expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, first));
            storage::RunCatalogSnapshotV1 snapshot;
            ASSERT_EQ(storage::loadRunCatalogV1(catalogPath, snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
            EXPECT_EQ(snapshot.generation, uncertain ? 4U : 3U);
            EXPECT_EQ(snapshot.activeRunId, domain::ExchangeRunId{1});
            EXPECT_EQ(snapshot.activeDisposition, uncertain ? (corrupt ? storage::RunCatalogDisposition::RECOVERY_FAILED
                                                                       : storage::RunCatalogDisposition::PAUSED)
                                                            : storage::RunCatalogDisposition::FAIL_STOPPED);
            if (uncertain) {
                EXPECT_EQ(syncProbe.calls, 2);
                EXPECT_TRUE(syncProbe.unavailableWithoutOwners);
                ASSERT_TRUE(recovered.catalogReplacement->observedSnapshot.has_value());
                EXPECT_EQ(*recovered.catalogReplacement->observedSnapshot, snapshot);
            }
        }
    }
}

TEST_F(ControllerCommandProcessingTest, RecoveryFailedRetryRemainsUnavailableWhenPausedPublicationDoesNotCommit) {
    for (const bool uncertain : {false, true}) {
        const auto subdirectory = catalog().parent_path() /
                                  (uncertain ? "recovery-failed-pause-uncertain" : "recovery-failed-pause-definite");
        ASSERT_TRUE(std::filesystem::create_directory(subdirectory));
        const auto catalogPath = subdirectory / "run-catalog-v1";
        const auto journalPath = subdirectory / "run-1.fxjr";
        const auto first = order("FIRST");
        const auto failed = order("FAILED");
        {
            AppendProbe probe{.fault = AppendFault::IMMEDIATE_WRITE};
            ExchangeRunController producer;
            probe.controller = &producer;
            ASSERT_EQ(startProbedRun(catalogPath, probe, producer).outcome, NewRunStartupOutcome::READY);
            ASSERT_TRUE(producer.installCommandProcessingV1(2, 2, 1, 16'384, 4'096));
            ASSERT_EQ(producer.submitCommandV1(domain::ExchangeRunId{1}, first).status,
                      admission::AdmissionStatus::FIRST_SUBMISSION);
            ASSERT_EQ(producer.submitCommandV1(domain::ExchangeRunId{1}, failed).status,
                      admission::AdmissionStatus::FIRST_SUBMISSION);
            ASSERT_FALSE(producer.advanceCommandProcessingV1());
        }
        corruptLastJournalByte(journalPath);
        {
            ExchangeRunController firstAttempt;
            ASSERT_EQ(firstAttempt.recoverFailedRunV1(catalogPath).outcome, FailedRunRecoveryOutcome::RECOVERY_FAILED);
        }
        corruptLastJournalByte(journalPath); // Restore the complete authoritative frame for the retry.

        ExchangeRunController retry;
        RecoveryCatalogSyncProbe probe{.controller = &retry};
        auto hooks = storage::detail::systemRunCatalogStorageHooks();
        if (uncertain) {
            hooks.context = &probe;
            hooks.syncFile = recoveryCatalogSync;
        } else {
            hooks.writeFile = failCatalogWrite;
        }

        const auto recovered = detail::recoverFailedRunV1WithHooks(catalogPath, hooks, retry);

        EXPECT_EQ(recovered.outcome, FailedRunRecoveryOutcome::CATALOG_REPLACE_FAILED);
        EXPECT_FALSE(recovered.originalProcessingInspection.has_value());
        ASSERT_TRUE(recovered.recovery.has_value());
        EXPECT_EQ(recovered.recovery->outcome, recovery::RecoveredRunOutcome::RECOVERED);
        ASSERT_TRUE(recovered.catalogReplacement.has_value());
        EXPECT_EQ(recovered.catalogReplacement->outcome,
                  uncertain ? storage::RunCatalogReplaceOutcome::UNCERTAIN
                            : storage::RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE);
        EXPECT_EQ(recovered.catalogReplacement->systemError, EIO);
        EXPECT_EQ(retry.state(), ExchangeRunStartupState::UNAVAILABLE);
        EXPECT_EQ(retry.journalWriter(), nullptr);
        EXPECT_EQ(retry.matchingState(), nullptr);
        EXPECT_EQ(retry.admissionIndex(), nullptr);

        storage::RunCatalogSnapshotV1 snapshot;
        ASSERT_EQ(storage::loadRunCatalogV1(catalogPath, snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_EQ(snapshot.generation, uncertain ? 5U : 4U);
        EXPECT_EQ(snapshot.activeDisposition,
                  uncertain ? storage::RunCatalogDisposition::PAUSED : storage::RunCatalogDisposition::RECOVERY_FAILED);
        if (uncertain) {
            EXPECT_EQ(probe.calls, 2);
            EXPECT_TRUE(probe.unavailableWithoutOwners);
            ASSERT_TRUE(recovered.catalogReplacement->observedSnapshot.has_value());
            EXPECT_EQ(*recovered.catalogReplacement->observedSnapshot, snapshot);
        }
    }
}

TEST_F(ControllerCommandProcessingTest, FailedRunRecoveryAcceptsFreshControllerAndRefusesActiveRuntimeStates) {
    ExchangeRunController unavailable;
    EXPECT_EQ(unavailable.recoverFailedRunV1(catalog()).outcome, FailedRunRecoveryOutcome::CATALOG_LOAD_FAILED);

    ExchangeRunController ready;
    ASSERT_EQ(ready.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    const auto* readyWriter = ready.journalWriter();
    EXPECT_EQ(ready.recoverFailedRunV1(catalog()).outcome, FailedRunRecoveryOutcome::NOT_RECOVERABLE);
    EXPECT_EQ(ready.journalWriter(), readyWriter);
    ASSERT_TRUE(ready.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    ASSERT_EQ(ready.pauseRunV1().outcome, RunPauseOutcome::PAUSED);
    const auto* pausedWriter = ready.journalWriter();
    EXPECT_EQ(ready.recoverFailedRunV1(catalog()).outcome, FailedRunRecoveryOutcome::NOT_RECOVERABLE);
    EXPECT_EQ(ready.journalWriter(), pausedWriter);

    const auto capacityDirectory = catalog().parent_path() / "capacity-state";
    ASSERT_TRUE(std::filesystem::create_directory(capacityDirectory));
    ExchangeRunController capacity;
    ASSERT_EQ(capacity.startupNewRunV1(capacityDirectory / "run-catalog-v1", configuration(1, 64 * 1024)).outcome,
              NewRunStartupOutcome::READY);
    ASSERT_TRUE(capacity.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    const auto final = order("FINAL");
    ASSERT_EQ(capacity.submitCommandV1(domain::ExchangeRunId{1}, final).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(capacity.advanceCommandProcessingV1());
    ASSERT_EQ(capacity.state(), ExchangeRunStartupState::CAPACITY_REACHED);
    const auto* capacityWriter = capacity.journalWriter();
    EXPECT_EQ(capacity.recoverFailedRunV1(capacityDirectory / "run-catalog-v1").outcome,
              FailedRunRecoveryOutcome::NOT_RECOVERABLE);
    EXPECT_EQ(capacity.journalWriter(), capacityWriter);
}

TEST_F(ControllerCommandProcessingTest, FreshControllerExplicitlyRecoversCatalogSelectedFailStoppedRun) {
    const auto catalogPath = catalog();
    const auto first = order("FIRST");
    const auto failed = order("FAILED");
    {
        AppendProbe probe{.fault = AppendFault::IMMEDIATE_WRITE};
        ExchangeRunController producer;
        probe.controller = &producer;
        ASSERT_EQ(startProbedRun(catalogPath, probe, producer).outcome, NewRunStartupOutcome::READY);
        ASSERT_TRUE(producer.installCommandProcessingV1(2, 2, 1, 16'384, 4'096));
        ASSERT_EQ(producer.submitCommandV1(domain::ExchangeRunId{1}, first).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_EQ(producer.submitCommandV1(domain::ExchangeRunId{1}, failed).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_FALSE(producer.advanceCommandProcessingV1());
        ASSERT_EQ(producer.state(), ExchangeRunStartupState::FAIL_STOPPED);
    }

    ExchangeRunController restarted;
    EXPECT_EQ(restarted.startupExistingRunV1(catalogPath).outcome,
              ExistingRunStartupOutcome::EXPLICIT_OPERATION_REQUIRED);

    const auto recovered = restarted.recoverFailedRunV1(catalogPath);

    ASSERT_EQ(recovered.outcome, FailedRunRecoveryOutcome::PAUSED);
    EXPECT_FALSE(recovered.originalProcessingInspection.has_value());
    ASSERT_TRUE(recovered.catalogLoad.has_value());
    EXPECT_EQ(recovered.catalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
    ASSERT_TRUE(recovered.recovery.has_value());
    EXPECT_EQ(recovered.recovery->outcome, recovery::RecoveredRunOutcome::RECOVERED);
    ASSERT_TRUE(recovered.catalogReplacement.has_value());
    EXPECT_EQ(recovered.catalogReplacement->outcome, storage::RunCatalogReplaceOutcome::COMMITTED);
    EXPECT_EQ(restarted.state(), ExchangeRunStartupState::PAUSED);
    EXPECT_FALSE(restarted.inspectCommandProcessingV1().installed);
    ASSERT_NE(restarted.journalWriter(), nullptr);
    ASSERT_NE(restarted.matchingState(), nullptr);
    ASSERT_NE(restarted.admissionIndex(), nullptr);
    EXPECT_EQ(restarted.journalWriter()->nextCommandSequence(), domain::CommandSequence{2});
    EXPECT_EQ(restarted.matchingState()->snapshot().activeOrders.size(), 1U);
    expectCorrelation(restarted.admissionIndex()->completedResult(first), first, 1);
    EXPECT_EQ(restarted.admissionIndex()->completedResult(failed), nullptr);

    storage::RunCatalogSnapshotV1 snapshot;
    ASSERT_EQ(storage::loadRunCatalogV1(catalogPath, snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(snapshot.generation, 4U);
    EXPECT_EQ(snapshot.activeRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(snapshot.activeDisposition, storage::RunCatalogDisposition::PAUSED);
}

TEST_F(ControllerCommandProcessingTest, FreshControllerRetriesRecoveryFailedWithoutRedundantFailurePublication) {
    for (const bool repairBeforeRetry : {false, true}) {
        const auto subdirectory = catalog().parent_path() / (repairBeforeRetry ? "retry-success" : "retry-failure");
        ASSERT_TRUE(std::filesystem::create_directory(subdirectory));
        const auto catalogPath = subdirectory / "run-catalog-v1";
        const auto journalPath = subdirectory / "run-1.fxjr";
        const auto first = order("FIRST");
        const auto failed = order("FAILED");
        {
            AppendProbe probe{.fault = AppendFault::IMMEDIATE_WRITE};
            ExchangeRunController producer;
            probe.controller = &producer;
            ASSERT_EQ(startProbedRun(catalogPath, probe, producer).outcome, NewRunStartupOutcome::READY);
            ASSERT_TRUE(producer.installCommandProcessingV1(2, 2, 1, 16'384, 4'096));
            ASSERT_EQ(producer.submitCommandV1(domain::ExchangeRunId{1}, first).status,
                      admission::AdmissionStatus::FIRST_SUBMISSION);
            ASSERT_EQ(producer.submitCommandV1(domain::ExchangeRunId{1}, failed).status,
                      admission::AdmissionStatus::FIRST_SUBMISSION);
            ASSERT_FALSE(producer.advanceCommandProcessingV1());
            ASSERT_EQ(producer.state(), ExchangeRunStartupState::FAIL_STOPPED);
        }
        corruptLastJournalByte(journalPath);
        {
            ExchangeRunController firstAttempt;
            const auto failedRecovery = firstAttempt.recoverFailedRunV1(catalogPath);
            ASSERT_EQ(failedRecovery.outcome, FailedRunRecoveryOutcome::RECOVERY_FAILED);
            EXPECT_FALSE(failedRecovery.originalProcessingInspection.has_value());
            ASSERT_TRUE(failedRecovery.recovery.has_value());
            EXPECT_EQ(failedRecovery.recovery->preparation.outcome, storage::RunJournalRecoveryOutcome::CORRUPTION);
            ASSERT_TRUE(failedRecovery.catalogReplacement.has_value());
            EXPECT_EQ(failedRecovery.catalogReplacement->outcome, storage::RunCatalogReplaceOutcome::COMMITTED);
            EXPECT_EQ(firstAttempt.state(), ExchangeRunStartupState::RECOVERY_FAILED);
        }

        storage::RunCatalogSnapshotV1 recoveryFailedSnapshot;
        ASSERT_EQ(storage::loadRunCatalogV1(catalogPath, recoveryFailedSnapshot).outcome,
                  storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_EQ(recoveryFailedSnapshot.generation, 4U);
        EXPECT_EQ(recoveryFailedSnapshot.activeDisposition, storage::RunCatalogDisposition::RECOVERY_FAILED);
        if (repairBeforeRetry) {
            corruptLastJournalByte(journalPath); // XOR restores the original committed frame.
        }

        ExchangeRunController retry;
        EXPECT_EQ(retry.startupExistingRunV1(catalogPath).outcome,
                  ExistingRunStartupOutcome::EXPLICIT_OPERATION_REQUIRED);
        const auto retried = retry.recoverFailedRunV1(catalogPath);
        EXPECT_FALSE(retried.originalProcessingInspection.has_value());
        ASSERT_TRUE(retried.recovery.has_value());
        if (repairBeforeRetry) {
            EXPECT_EQ(retried.outcome, FailedRunRecoveryOutcome::PAUSED);
            EXPECT_EQ(retried.recovery->outcome, recovery::RecoveredRunOutcome::RECOVERED);
            ASSERT_TRUE(retried.catalogReplacement.has_value());
            EXPECT_EQ(retried.catalogReplacement->outcome, storage::RunCatalogReplaceOutcome::COMMITTED);
            EXPECT_EQ(retry.state(), ExchangeRunStartupState::PAUSED);
            ASSERT_NE(retry.admissionIndex(), nullptr);
            expectCorrelation(retry.admissionIndex()->completedResult(first), first, 1);
        } else {
            EXPECT_EQ(retried.outcome, FailedRunRecoveryOutcome::RECOVERY_FAILED);
            EXPECT_EQ(retried.recovery->preparation.outcome, storage::RunJournalRecoveryOutcome::CORRUPTION);
            EXPECT_FALSE(retried.catalogReplacement.has_value());
            EXPECT_EQ(retry.state(), ExchangeRunStartupState::RECOVERY_FAILED);
        }

        storage::RunCatalogSnapshotV1 finalSnapshot;
        ASSERT_EQ(storage::loadRunCatalogV1(catalogPath, finalSnapshot).outcome,
                  storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_EQ(finalSnapshot.generation, repairBeforeRetry ? 5U : 4U);
        EXPECT_EQ(finalSnapshot.activeDisposition, repairBeforeRetry ? storage::RunCatalogDisposition::PAUSED
                                                                     : storage::RunCatalogDisposition::RECOVERY_FAILED);
    }
}

TEST_F(ControllerCommandProcessingTest, FailedRunRecoveryRebuildsAfterMatchingAndCompletionInvariants) {
    using ConstructState = std::optional<StartNewRunOutcome> (*)(
        void*, domain::ExchangeRunId, std::uint64_t, std::unique_ptr<matching_engine::MatchingState>&,
        std::unique_ptr<admission::CommandAdmissionIndex>&) noexcept;
    const ConstructState matchingFailure = [](void*, domain::ExchangeRunId runId, std::uint64_t capacity,
                                              std::unique_ptr<matching_engine::MatchingState>& state,
                                              std::unique_ptr<admission::CommandAdmissionIndex>& admission) noexcept {
        const auto error = detail::constructEmptyRunStateV1(runId, capacity, state, admission);
        if (error.has_value()) {
            return error;
        }
        auto conflicting = order("INJECTED");
        conflicting.globalSequenceNumber = domain::CommandSequence{1};
        conflicting.orderId = domain::OrderId{1};
        try {
            (void)state->processCommand(conflicting);
        } catch (...) {
            return std::optional{StartNewRunOutcome::STATE_CONSTRUCTION_FAILED};
        }
        return std::optional<StartNewRunOutcome>{};
    };
    const ConstructState completionFailure = [](void*, domain::ExchangeRunId runId, std::uint64_t capacity,
                                                std::unique_ptr<matching_engine::MatchingState>& state,
                                                std::unique_ptr<admission::CommandAdmissionIndex>& admission) noexcept {
        const auto error = detail::constructEmptyRunStateV1(runId, capacity, state, admission);
        if (error.has_value()) {
            return error;
        }
        try {
            state = std::make_unique<matching_engine::MatchingState>(domain::ExchangeRunId{runId.value() + 1});
        } catch (...) {
            return std::optional{StartNewRunOutcome::STATE_CONSTRUCTION_FAILED};
        }
        return std::optional<StartNewRunOutcome>{};
    };

    for (const bool completion : {false, true}) {
        const auto subdirectory = catalog().parent_path() / (completion ? "recover-completion" : "recover-matching");
        ASSERT_TRUE(std::filesystem::create_directory(subdirectory));
        const auto catalogPath = subdirectory / "run-catalog-v1";
        const detail::StartNewRunHooks startupHooks{
            nullptr, completion ? completionFailure : matchingFailure,
            [](void*, const std::filesystem::path& path, const storage::PreparedNewRunJournalV1& prepared) noexcept {
                return storage::activatePreparedNewRunV1(path, prepared);
            },
            nullptr};
        ExchangeRunController controller;
        ASSERT_EQ(detail::startupNewRunV1WithHooks(catalogPath, configuration(), &startupHooks, controller).outcome,
                  NewRunStartupOutcome::READY);
        ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
        const auto command = order(completion ? "COMPLETION-RECOVERY" : "MATCHING-RECOVERY");
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, command).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_FALSE(controller.advanceCommandProcessingV1());
        ASSERT_EQ(controller.state(), ExchangeRunStartupState::FAIL_STOPPED);
        const auto failed = controller.inspectCommandProcessingV1();
        ASSERT_NE(failed.internalFailure, nullptr);
        EXPECT_EQ(failed.pendingCompletion, completion);
        EXPECT_EQ(failed.inProgressMatchingCommand.has_value(), !completion);

        const auto recovered = controller.recoverFailedRunV1(catalogPath);

        ASSERT_EQ(recovered.outcome, FailedRunRecoveryOutcome::PAUSED);
        ASSERT_TRUE(recovered.originalProcessingInspection.has_value());
        EXPECT_EQ(recovered.originalProcessingInspection->internalFailure, failed.internalFailure);
        ASSERT_TRUE(recovered.recovery.has_value());
        EXPECT_EQ(recovered.recovery->outcome, recovery::RecoveredRunOutcome::RECOVERED);
        EXPECT_EQ(controller.state(), ExchangeRunStartupState::PAUSED);
        EXPECT_FALSE(controller.inspectCommandProcessingV1().installed);
        ASSERT_NE(controller.matchingState(), nullptr);
        ASSERT_NE(controller.admissionIndex(), nullptr);
        ASSERT_NE(controller.journalWriter(), nullptr);
        EXPECT_EQ(controller.matchingState()->snapshot().exchangeRunId, domain::ExchangeRunId{1});
        EXPECT_EQ(controller.matchingState()->snapshot().activeOrders.size(), 1U);
        EXPECT_TRUE(controller.matchingState()->invariantsHold());
        EXPECT_EQ(controller.admissionIndex()->size(), 1U);
        expectCorrelation(controller.admissionIndex()->completedResult(command), command, 1);
        EXPECT_EQ(controller.journalWriter()->nextCommandSequence(), domain::CommandSequence{2});
    }
}

std::filesystem::path retainedJournalPath(const std::filesystem::path& catalogPath, const domain::ExchangeRunId runId) {
    return catalogPath.parent_path() / ("run-" + std::to_string(runId.value()) + ".fxjr");
}

std::vector<char> fileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(input.is_open());
    if (!input.is_open()) {
        return {};
    }
    const auto size = input.tellg();
    EXPECT_GE(size, 0);
    if (size < 0) {
        return {};
    }
    std::vector<char> result(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    EXPECT_TRUE(input.good() || input.eof());
    return result;
}

void seedRetainedStoppedRun(const std::filesystem::path& catalogPath) {
    const storage::RunCatalogSnapshotV1 snapshot{
        .generation = 1,
        .lastReservedRunId = domain::ExchangeRunId{1},
        .activeRunId = std::nullopt,
        .activeDisposition = storage::RunCatalogDisposition::NONE,
        .retainedStoppedRunId = domain::ExchangeRunId{1},
    };
    ASSERT_EQ(storage::replaceRunCatalogV1(catalogPath, snapshot).outcome,
              storage::RunCatalogReplaceOutcome::COMMITTED);
    std::ofstream journal(retainedJournalPath(catalogPath, domain::ExchangeRunId{1}), std::ios::binary);
    ASSERT_TRUE(journal.is_open());
    journal.write("retained", 8);
    journal.flush();
    ASSERT_TRUE(journal.good());
}

struct ReadyStopSyncProbe final {
    ExchangeRunController* controller{nullptr};
    sequencer::sequenceMessage first{};
    sequencer::sequenceMessage second{};
    int calls{0};
    bool closedAndCompleted{true};
};

int readyStopSync(void* context, const int descriptor) noexcept {
    auto& probe = *static_cast<ReadyStopSyncProbe*>(context);
    ++probe.calls;
    const auto unseen = probe.controller->submitCommandV1(domain::ExchangeRunId{1}, order("DURING-STOP"));
    const auto first = probe.controller->submitCommandV1(domain::ExchangeRunId{1}, probe.first);
    const auto second = probe.controller->submitCommandV1(domain::ExchangeRunId{1}, probe.second);
    const auto inspection = probe.controller->inspectCommandProcessingV1();
    probe.closedAndCompleted &= unseen.status == admission::AdmissionStatus::ADMISSION_UNAVAILABLE &&
                                unseen.rejectionReason == domain::AdmissionRejectionReason::EXCHANGE_RUN_UNAVAILABLE &&
                                first.status == admission::AdmissionStatus::IDENTICAL_COMPLETED &&
                                second.status == admission::AdmissionStatus::IDENTICAL_COMPLETED &&
                                inspection.ingressEmpty && inspection.matchingEmpty && inspection.queuedResults == 0 &&
                                !inspection.pendingMatchingResult && !inspection.pendingCompletion &&
                                probe.controller->state() == (probe.calls <= 2 ? ExchangeRunStartupState::READY
                                                                               : ExchangeRunStartupState::PAUSED);
    return ::fsync(descriptor);
}

TEST_F(ControllerCommandProcessingTest, StopFromReadyReusesPauseDrainAndPublishesOneRetainedRun) {
    ExchangeRunController controller;
    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(2, 1, 1, 16'384, 4'096));
    const auto first = order("FIRST");
    const auto second = order("SECOND");
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, first).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, second).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ReadyStopSyncProbe probe{.controller = &controller, .first = first, .second = second};
    auto hooks = storage::detail::systemRunCatalogStorageHooks();
    hooks.context = &probe;
    hooks.syncFile = readyStopSync;

    ASSERT_EQ(controller.stopRunV1().outcome, RunStopOutcome::RESULT_HANDOFF_BACKPRESSURE);
    drainPrivateResultHandoff(controller);
    const auto stopped = detail::stopRunV1WithHooks(std::nullopt, hooks, controller);

    ASSERT_EQ(stopped.outcome, RunStopOutcome::STOPPED);
    ASSERT_TRUE(stopped.pause.has_value());
    EXPECT_EQ(stopped.pause->outcome, RunPauseOutcome::PAUSED);
    ASSERT_TRUE(stopped.catalogReplacement.has_value());
    EXPECT_EQ(stopped.catalogReplacement->outcome, storage::RunCatalogReplaceOutcome::COMMITTED);
    EXPECT_EQ(stopped.cleanup.outcome, StoppedRunCleanupOutcome::NOT_REQUIRED);
    EXPECT_EQ(probe.calls, 4);
    EXPECT_TRUE(probe.closedAndCompleted);
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::STOPPED);
    EXPECT_EQ(controller.journalWriter(), nullptr);
    EXPECT_EQ(controller.matchingState(), nullptr);
    EXPECT_EQ(controller.admissionIndex(), nullptr);
    EXPECT_FALSE(controller.inspectCommandProcessingV1().installed);
    EXPECT_TRUE(std::filesystem::exists(journal()));

    storage::LoadedRunJournalV1 loaded;
    ASSERT_EQ(storage::loadRunJournalV1(journal(), loaded).outcome, storage::RunJournalLoadOutcome::VALID);
    ASSERT_EQ(loaded.commands.size(), 2U);
    EXPECT_EQ(std::get<storage::JournalNewOrderV1>(loaded.commands[0]).clientCommandId, first.clientCommandId);
    EXPECT_EQ(std::get<storage::JournalNewOrderV1>(loaded.commands[1]).clientCommandId, second.clientCommandId);
    storage::RunCatalogSnapshotV1 snapshot;
    ASSERT_EQ(storage::loadRunCatalogV1(catalog(), snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(snapshot.generation, 4U);
    EXPECT_FALSE(snapshot.activeRunId.has_value());
    EXPECT_EQ(snapshot.activeDisposition, storage::RunCatalogDisposition::NONE);
    EXPECT_EQ(snapshot.retainedStoppedRunId, domain::ExchangeRunId{1});
}

TEST_F(ControllerCommandProcessingTest, StopFromPausedAndCapacityReachedUsesTheirDurableDisposition) {
    {
        const auto directory = catalog().parent_path() / "paused-stop";
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        const auto catalogPath = directory / "run-catalog-v1";
        {
            ExchangeRunController producer;
            ASSERT_EQ(producer.startupNewRunV1(catalogPath, configuration()).outcome, NewRunStartupOutcome::READY);
            ASSERT_TRUE(producer.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
            ASSERT_EQ(producer.pauseRunV1().outcome, RunPauseOutcome::PAUSED);
        }
        ExchangeRunController paused;
        ASSERT_EQ(paused.startupExistingRunV1(catalogPath).outcome, ExistingRunStartupOutcome::PAUSED);
        EXPECT_FALSE(paused.inspectCommandProcessingV1().installed);

        const auto stopped = paused.stopRunV1();

        EXPECT_EQ(stopped.outcome, RunStopOutcome::STOPPED);
        EXPECT_FALSE(stopped.pause.has_value());
        EXPECT_EQ(paused.state(), ExchangeRunStartupState::STOPPED);
        storage::RunCatalogSnapshotV1 snapshot;
        ASSERT_EQ(storage::loadRunCatalogV1(catalogPath, snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_EQ(snapshot.generation, 5U);
        EXPECT_FALSE(snapshot.activeRunId.has_value());
        EXPECT_EQ(snapshot.retainedStoppedRunId, domain::ExchangeRunId{1});
    }
    {
        const auto directory = catalog().parent_path() / "capacity-stop";
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        const auto catalogPath = directory / "run-catalog-v1";
        ExchangeRunController capacity;
        ASSERT_EQ(capacity.startupNewRunV1(catalogPath, configuration(1, 64 * 1024)).outcome,
                  NewRunStartupOutcome::READY);
        ASSERT_TRUE(capacity.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
        ASSERT_EQ(capacity.submitCommandV1(domain::ExchangeRunId{1}, order("FINAL")).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_TRUE(capacity.advanceCommandProcessingV1());
        ASSERT_EQ(capacity.state(), ExchangeRunStartupState::CAPACITY_REACHED);
        drainPrivateResultHandoff(capacity);

        const auto stopped = capacity.stopRunV1();

        EXPECT_EQ(stopped.outcome, RunStopOutcome::STOPPED);
        EXPECT_FALSE(stopped.pause.has_value());
        EXPECT_EQ(capacity.state(), ExchangeRunStartupState::STOPPED);
        storage::RunCatalogSnapshotV1 snapshot;
        ASSERT_EQ(storage::loadRunCatalogV1(catalogPath, snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_EQ(snapshot.generation, 4U);
        EXPECT_FALSE(snapshot.activeRunId.has_value());
        EXPECT_EQ(snapshot.retainedStoppedRunId, domain::ExchangeRunId{1});
    }
}

enum class StopCatalogPrecondition { ACTIVE_RUN_ID, DISPOSITION, GENERATION };

TEST_F(ControllerCommandProcessingTest, StopPreflightFailuresPreserveCatalogOwnersAndOpenAdmission) {
    for (const auto condition : {StopCatalogPrecondition::ACTIVE_RUN_ID, StopCatalogPrecondition::DISPOSITION,
                                 StopCatalogPrecondition::GENERATION}) {
        const auto directory =
            catalog().parent_path() / ("stop-preflight-" + std::to_string(static_cast<int>(condition)));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        const auto catalogPath = directory / "run-catalog-v1";
        ExchangeRunController controller;
        ASSERT_EQ(controller.startupNewRunV1(catalogPath, configuration()).outcome, NewRunStartupOutcome::READY);
        ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
        const auto* state = controller.matchingState();
        const auto* writer = controller.journalWriter();
        const auto* admission = controller.admissionIndex();
        storage::RunCatalogSnapshotV1 snapshot;
        ASSERT_EQ(storage::loadRunCatalogV1(catalogPath, snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
        if (condition == StopCatalogPrecondition::ACTIVE_RUN_ID) {
            snapshot.lastReservedRunId = domain::ExchangeRunId{2};
            snapshot.activeRunId = domain::ExchangeRunId{2};
        } else if (condition == StopCatalogPrecondition::DISPOSITION) {
            snapshot.activeDisposition = storage::RunCatalogDisposition::PAUSED;
        } else {
            snapshot.generation = std::numeric_limits<std::uint64_t>::max() - 1;
        }
        overwriteCatalog(catalogPath, snapshot);
        const auto originalCatalog = fileBytes(catalogPath);

        const auto stopped = controller.stopRunV1();

        EXPECT_EQ(stopped.outcome,
                  condition == StopCatalogPrecondition::ACTIVE_RUN_ID ? RunStopOutcome::ACTIVE_RUN_ID_MISMATCH
                  : condition == StopCatalogPrecondition::DISPOSITION ? RunStopOutcome::CATALOG_DISPOSITION_MISMATCH
                                                                      : RunStopOutcome::GENERATION_EXHAUSTED);
        EXPECT_EQ(controller.state(), ExchangeRunStartupState::READY);
        EXPECT_EQ(controller.matchingState(), state);
        EXPECT_EQ(controller.journalWriter(), writer);
        EXPECT_EQ(controller.admissionIndex(), admission);
        EXPECT_EQ(fileBytes(catalogPath), originalCatalog);
        EXPECT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, order("STILL-OPEN")).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
    }
}

struct StopCleanupProbe final {
    ExchangeRunController* controller{nullptr};
    std::filesystem::path catalogPath{};
    std::filesystem::path expectedRemovedPath{};
    int syncCalls{0};
    int removeCalls{0};
    bool failRemove{false};
    bool failDirectorySync{false};
    bool committedBeforeRemove{true};
    bool borrowersAndOwnersReleased{true};
};

int stopCleanupSync(void* context, const int descriptor) noexcept {
    auto& probe = *static_cast<StopCleanupProbe*>(context);
    ++probe.syncCalls;
    if (probe.failDirectorySync && probe.syncCalls == 3) {
        errno = EIO;
        return -1;
    }
    return ::fsync(descriptor);
}

int stopCleanupRemove(void* context, const char* path) noexcept {
    auto& probe = *static_cast<StopCleanupProbe*>(context);
    ++probe.removeCalls;
    storage::RunCatalogSnapshotV1 snapshot;
    const auto load = storage::loadRunCatalogV1(probe.catalogPath, snapshot);
    probe.committedBeforeRemove &= load.outcome == storage::RunCatalogLoadOutcome::LOADED &&
                                   !snapshot.activeRunId.has_value() &&
                                   snapshot.activeDisposition == storage::RunCatalogDisposition::NONE &&
                                   snapshot.retainedStoppedRunId == domain::ExchangeRunId{2} &&
                                   std::filesystem::path{path} == probe.expectedRemovedPath;
    probe.borrowersAndOwnersReleased &=
        probe.controller->state() == ExchangeRunStartupState::STOPPED &&
        !probe.controller->inspectCommandProcessingV1().installed && probe.controller->journalWriter() == nullptr &&
        probe.controller->matchingState() == nullptr && probe.controller->admissionIndex() == nullptr;
    if (probe.failRemove) {
        errno = EACCES;
        return -1;
    }
    return ::unlink(path);
}

TEST_F(ControllerCommandProcessingTest, RetentionReplacementRequiresConfirmationAndDeletesOnlyAfterCommit) {
    seedRetainedStoppedRun(catalog());
    ExchangeRunController controller;
    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    const auto* state = controller.matchingState();
    const auto* writer = controller.journalWriter();
    const auto* admission = controller.admissionIndex();
    const auto originalCatalog = fileBytes(catalog());
    const auto oldJournal = retainedJournalPath(catalog(), domain::ExchangeRunId{1});
    const auto activeJournal = retainedJournalPath(catalog(), domain::ExchangeRunId{2});
    const auto oldJournalBytes = fileBytes(oldJournal);
    const auto activeJournalBytes = fileBytes(activeJournal);

    EXPECT_EQ(controller.stopRunV1().outcome, RunStopOutcome::RETAINED_RUN_CONFIRMATION_REQUIRED);
    EXPECT_EQ(controller.stopRunV1(domain::ExchangeRunId{9}).outcome,
              RunStopOutcome::RETAINED_RUN_CONFIRMATION_MISMATCH);
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::READY);
    EXPECT_EQ(controller.matchingState(), state);
    EXPECT_EQ(controller.journalWriter(), writer);
    EXPECT_EQ(controller.admissionIndex(), admission);
    EXPECT_EQ(fileBytes(catalog()), originalCatalog);
    EXPECT_TRUE(std::filesystem::exists(oldJournal));
    EXPECT_TRUE(std::filesystem::exists(activeJournal));
    EXPECT_EQ(fileBytes(oldJournal), oldJournalBytes);
    EXPECT_EQ(fileBytes(activeJournal), activeJournalBytes);

    StopCleanupProbe probe{.controller = &controller, .catalogPath = catalog(), .expectedRemovedPath = oldJournal};
    auto hooks = storage::detail::systemRunCatalogStorageHooks();
    hooks.context = &probe;
    hooks.syncFile = stopCleanupSync;
    hooks.removeFile = stopCleanupRemove;
    const auto stopped = detail::stopRunV1WithHooks(domain::ExchangeRunId{1}, hooks, controller);

    EXPECT_EQ(stopped.outcome, RunStopOutcome::STOPPED);
    EXPECT_EQ(stopped.cleanup.outcome, StoppedRunCleanupOutcome::COMMITTED);
    EXPECT_EQ(stopped.cleanup.replacedRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(probe.syncCalls, 5);
    EXPECT_EQ(probe.removeCalls, 1);
    EXPECT_TRUE(probe.committedBeforeRemove);
    EXPECT_TRUE(probe.borrowersAndOwnersReleased);
    EXPECT_FALSE(std::filesystem::exists(oldJournal));
    EXPECT_TRUE(std::filesystem::exists(activeJournal));
    storage::RunCatalogSnapshotV1 snapshot;
    ASSERT_EQ(storage::loadRunCatalogV1(catalog(), snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(snapshot.generation, 5U);
    EXPECT_FALSE(snapshot.activeRunId.has_value());
    EXPECT_EQ(snapshot.retainedStoppedRunId, domain::ExchangeRunId{2});
}

struct StopPublicationFailureProbe final {
    int syncCalls{0};
    int removeCalls{0};
};

int stopPublicationSync(void* context, const int descriptor) noexcept {
    auto& probe = *static_cast<StopPublicationFailureProbe*>(context);
    ++probe.syncCalls;
    if (probe.syncCalls == 2) {
        errno = EIO;
        return -1;
    }
    return ::fsync(descriptor);
}

int countStopRemove(void* context, const char* path) noexcept {
    auto& probe = *static_cast<StopPublicationFailureProbe*>(context);
    ++probe.removeCalls;
    return ::unlink(path);
}

TEST_F(ControllerCommandProcessingTest, StopCatalogFailureAndUncertaintyNeverDeleteEitherJournal) {
    for (const bool uncertain : {false, true}) {
        const auto directory = catalog().parent_path() / (uncertain ? "stop-uncertain" : "stop-definite");
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        const auto catalogPath = directory / "run-catalog-v1";
        seedRetainedStoppedRun(catalogPath);
        ExchangeRunController controller;
        ASSERT_EQ(controller.startupNewRunV1(catalogPath, configuration()).outcome, NewRunStartupOutcome::READY);
        ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
        ASSERT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::PAUSED);
        const auto* state = controller.matchingState();
        const auto* writer = controller.journalWriter();
        const auto* admission = controller.admissionIndex();
        StopPublicationFailureProbe probe;
        auto hooks = storage::detail::systemRunCatalogStorageHooks();
        hooks.context = &probe;
        hooks.removeFile = countStopRemove;
        if (uncertain) {
            hooks.syncFile = stopPublicationSync;
        } else {
            hooks.writeFile = failCatalogWrite;
        }

        const auto stopped = detail::stopRunV1WithHooks(domain::ExchangeRunId{1}, hooks, controller);

        ASSERT_EQ(stopped.outcome, RunStopOutcome::CATALOG_REPLACE_FAILED);
        ASSERT_TRUE(stopped.catalogReplacement.has_value());
        EXPECT_EQ(stopped.catalogReplacement->outcome,
                  uncertain ? storage::RunCatalogReplaceOutcome::UNCERTAIN
                            : storage::RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE);
        EXPECT_EQ(controller.state(), ExchangeRunStartupState::UNAVAILABLE);
        EXPECT_EQ(controller.matchingState(), state);
        EXPECT_EQ(controller.journalWriter(), writer);
        EXPECT_EQ(controller.admissionIndex(), admission);
        EXPECT_EQ(probe.removeCalls, 0);
        EXPECT_TRUE(std::filesystem::exists(retainedJournalPath(catalogPath, domain::ExchangeRunId{1})));
        EXPECT_TRUE(std::filesystem::exists(retainedJournalPath(catalogPath, domain::ExchangeRunId{2})));
    }
}

TEST_F(ControllerCommandProcessingTest, CleanupFailureKeepsCommittedStoppedStateAndExactEvidence) {
    for (const bool failDirectorySync : {false, true}) {
        const auto directory = catalog().parent_path() / (failDirectorySync ? "cleanup-sync" : "cleanup-remove");
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        const auto catalogPath = directory / "run-catalog-v1";
        seedRetainedStoppedRun(catalogPath);
        ExchangeRunController controller;
        ASSERT_EQ(controller.startupNewRunV1(catalogPath, configuration()).outcome, NewRunStartupOutcome::READY);
        ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
        ASSERT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::PAUSED);
        const auto oldJournal = retainedJournalPath(catalogPath, domain::ExchangeRunId{1});
        const auto activeJournal = retainedJournalPath(catalogPath, domain::ExchangeRunId{2});
        StopCleanupProbe probe{.controller = &controller,
                               .catalogPath = catalogPath,
                               .expectedRemovedPath = oldJournal,
                               .failRemove = !failDirectorySync,
                               .failDirectorySync = failDirectorySync};
        auto hooks = storage::detail::systemRunCatalogStorageHooks();
        hooks.context = &probe;
        hooks.syncFile = stopCleanupSync;
        hooks.removeFile = stopCleanupRemove;

        const auto stopped = detail::stopRunV1WithHooks(domain::ExchangeRunId{1}, hooks, controller);

        EXPECT_EQ(stopped.outcome, RunStopOutcome::STOPPED_CLEANUP_FAILED);
        EXPECT_EQ(stopped.cleanup.outcome, failDirectorySync ? StoppedRunCleanupOutcome::DIRECTORY_SYNC_FAILED
                                                             : StoppedRunCleanupOutcome::REMOVE_FAILED);
        EXPECT_EQ(stopped.cleanup.replacedRunId, domain::ExchangeRunId{1});
        EXPECT_EQ(stopped.cleanup.systemError, failDirectorySync ? EIO : EACCES);
        ASSERT_TRUE(stopped.catalogReplacement.has_value());
        EXPECT_EQ(stopped.catalogReplacement->outcome, storage::RunCatalogReplaceOutcome::COMMITTED);
        EXPECT_EQ(controller.state(), ExchangeRunStartupState::STOPPED);
        EXPECT_EQ(controller.journalWriter(), nullptr);
        EXPECT_EQ(controller.matchingState(), nullptr);
        EXPECT_EQ(controller.admissionIndex(), nullptr);
        EXPECT_EQ(probe.syncCalls, failDirectorySync ? 3 : 2);
        EXPECT_EQ(probe.removeCalls, 1);
        EXPECT_TRUE(probe.committedBeforeRemove);
        EXPECT_TRUE(probe.borrowersAndOwnersReleased);
        EXPECT_EQ(std::filesystem::exists(oldJournal), !failDirectorySync);
        EXPECT_TRUE(std::filesystem::exists(activeJournal));
        storage::RunCatalogSnapshotV1 snapshot;
        ASSERT_EQ(storage::loadRunCatalogV1(catalogPath, snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_EQ(snapshot.generation, 5U);
        EXPECT_FALSE(snapshot.activeRunId.has_value());
        EXPECT_EQ(snapshot.retainedStoppedRunId, domain::ExchangeRunId{2});
    }
}

TEST_F(ControllerCommandProcessingTest, RetainedLookupIsLazyInstallsNoPartialViewAndReusesOneReadOnlyView) {
    const auto command = order("RETAINED");
    std::optional<matching_engine::CommandResultBatch> expected;
    {
        ExchangeRunController controller;
        ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
        ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, command).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_TRUE(controller.advanceCommandProcessingV1());
        const auto completed = controller.admissionIndex()->completedResult(command);
        expectCorrelation(completed, command, 1);
        expected.emplace(*completed);
        drainPrivateResultHandoff(controller);
        ASSERT_EQ(controller.stopRunV1().outcome, RunStopOutcome::STOPPED);
        EXPECT_EQ(controller.journalWriter(), nullptr);
        EXPECT_EQ(controller.matchingState(), nullptr);
        EXPECT_EQ(controller.admissionIndex(), nullptr);
        EXPECT_FALSE(controller.inspectCommandProcessingV1().installed);
    }

    // If stop had eagerly replayed, later journal corruption could not make the first lookup fail.
    corruptLastJournalByte(journal());
    ExchangeRunController restarted;
    const auto corrupt = restarted.lookupCompletedResultV1(catalog(), domain::ExchangeRunId{1}, command);
    ASSERT_EQ(corrupt.outcome, CompletedResultLookupOutcome::RECOVERY_FAILED);
    ASSERT_TRUE(corrupt.catalogLoad.has_value());
    EXPECT_EQ(corrupt.catalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
    ASSERT_TRUE(corrupt.recovery.has_value());
    EXPECT_EQ(corrupt.recovery->outcome, recovery::RecoveredRunOutcome::PREPARATION_FAILED);
    EXPECT_EQ(corrupt.recovery->preparation.outcome, storage::RunJournalRecoveryOutcome::CORRUPTION);
    EXPECT_EQ(corrupt.completedResult, nullptr);

    corruptLastJournalByte(journal()); // Restore the authoritative journal after the failed materialization.
    const auto first = restarted.lookupCompletedResultV1(catalog(), domain::ExchangeRunId{1}, command);
    ASSERT_EQ(first.outcome, CompletedResultLookupOutcome::FOUND);
    ASSERT_TRUE(first.catalogLoad.has_value());
    ASSERT_TRUE(first.recovery.has_value());
    EXPECT_EQ(first.recovery->outcome, recovery::RecoveredRunOutcome::RECOVERED);
    expectCorrelation(first.completedResult, command, 1);
    ASSERT_TRUE(expected.has_value());
    EXPECT_EQ(first.completedResult->result(), expected->result());
    EXPECT_EQ(first.completedResult->events(), expected->events());
    EXPECT_EQ(restarted.journalWriter(), nullptr);
    EXPECT_EQ(restarted.matchingState(), nullptr);
    EXPECT_EQ(restarted.admissionIndex(), nullptr);
    EXPECT_FALSE(restarted.inspectCommandProcessingV1().installed);
    expectUnavailable(restarted.submitCommandV1(domain::ExchangeRunId{1}, order("NO-WRITE")));
    EXPECT_FALSE(restarted.advanceCommandProcessingV1());

    ASSERT_TRUE(std::filesystem::remove(journal()));
    const auto repeated = restarted.lookupCompletedResultV1(catalog(), domain::ExchangeRunId{1}, command);
    EXPECT_EQ(repeated.outcome, CompletedResultLookupOutcome::FOUND);
    EXPECT_EQ(repeated.completedResult, first.completedResult);
    ASSERT_TRUE(repeated.catalogLoad.has_value());
    EXPECT_EQ(repeated.catalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_FALSE(repeated.recovery.has_value());
}

TEST_F(ControllerCommandProcessingTest, ActivePrivateLookupReturnsOnlyTheCrossingTakerViewWithoutMutation) {
    ExchangeRunController controller;
    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    const auto sell = order("PRIVATE-MAKER");
    const auto buy = order("PRIVATE-TAKER", true, 4);
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, sell).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, buy).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    const auto matchingBefore = controller.matchingState()->snapshot();
    const auto admissionSizeBefore = controller.admissionIndex()->size();
    const auto admissionStatisticsBefore = controller.admissionIndex()->statistics();
    const auto journalBytesBefore = controller.journalWriter()->committedByteCount();
    const auto nextSequenceBefore = controller.journalWriter()->nextCommandSequence();

    const auto lookup = controller.lookupPrivateResultV1(catalog(), domain::ExchangeRunId{1}, buy);

    ASSERT_EQ(lookup.outcome, CompletedResultLookupOutcome::FOUND);
    ASSERT_TRUE(lookup.privateResult.has_value());
    EXPECT_EQ(lookup.privateResult->recipient, buy.clientId);
    ASSERT_TRUE(lookup.privateResult->correlation.has_value());
    EXPECT_EQ(*lookup.privateResult->correlation,
              (domain::CommandResultCorrelation{buy.clientId, *buy.clientCommandId, domain::CommandSequence{2},
                                                domain::ExchangeRunId{1}}));
    ASSERT_EQ(lookup.privateResult->privateResult.size(), 1U);
    const auto* trade = std::get_if<private_result::PrivateTrade>(&lookup.privateResult->privateResult[0]);
    ASSERT_NE(trade, nullptr);
    EXPECT_EQ(trade->eventId,
              (domain::EventId{domain::CommandSequence{2}, domain::EventIndex{0}, domain::ExchangeRunId{1}}));
    EXPECT_EQ(trade->instrumentId, buy.instrumentId);
    EXPECT_EQ(trade->orderId, domain::OrderId{2});
    EXPECT_EQ(trade->side, domain::Side::BUY);
    EXPECT_EQ(trade->role, private_result::TradeRole::TAKER);
    EXPECT_EQ(trade->executionPrice, sell.price);
    EXPECT_EQ(trade->executionQuantity, buy.quantity);
    EXPECT_EQ(trade->remainingQuantity, domain::Quantity{0});
    EXPECT_FALSE(lookup.catalogLoad.has_value());
    EXPECT_FALSE(lookup.recovery.has_value());
    EXPECT_EQ(controller.matchingState()->snapshot(), matchingBefore);
    EXPECT_EQ(controller.admissionIndex()->size(), admissionSizeBefore);
    EXPECT_EQ(controller.admissionIndex()->statistics(), admissionStatisticsBefore);
    EXPECT_EQ(controller.journalWriter()->committedByteCount(), journalBytesBefore);
    EXPECT_EQ(controller.journalWriter()->nextCommandSequence(), nextSequenceBefore);

    auto conflict = buy;
    conflict.quantity = domain::Quantity{5};
    const auto conflicting = controller.lookupPrivateResultV1(catalog(), domain::ExchangeRunId{1}, conflict);
    EXPECT_EQ(conflicting.outcome, CompletedResultLookupOutcome::NOT_FOUND);
    EXPECT_FALSE(conflicting.privateResult.has_value());
    auto mismatchedIdentity = buy;
    mismatchedIdentity.clientId = domain::ClientId{33};
    const auto mismatched = controller.lookupPrivateResultV1(catalog(), domain::ExchangeRunId{1}, mismatchedIdentity);
    EXPECT_EQ(mismatched.outcome, CompletedResultLookupOutcome::NOT_FOUND);
    EXPECT_FALSE(mismatched.privateResult.has_value());
    EXPECT_EQ(controller.matchingState()->snapshot(), matchingBefore);
    EXPECT_EQ(controller.admissionIndex()->size(), admissionSizeBefore);
    EXPECT_EQ(controller.admissionIndex()->statistics(), admissionStatisticsBefore);
    EXPECT_EQ(controller.journalWriter()->committedByteCount(), journalBytesBefore);
    EXPECT_EQ(controller.journalWriter()->nextCommandSequence(), nextSequenceBefore);
}

TEST_F(ControllerCommandProcessingTest, RetainedPrivateLookupPreservesIdentityAndReusesTheMaterializedView) {
    ExchangeRunController controller;
    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    const auto sell = order("RETAINED-PRIVATE-MAKER");
    const auto buy = order("RETAINED-PRIVATE-TAKER", true, 4);
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, sell).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, buy).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    std::vector<market_data::PublicTrade> publicTrades;
    ASSERT_TRUE(controller.tryPopPublicTradeBatchV1(publicTrades));
    drainPrivateResultHandoff(controller);
    ASSERT_EQ(controller.stopRunV1().outcome, RunStopOutcome::STOPPED);
    EXPECT_EQ(controller.matchingState(), nullptr);
    EXPECT_EQ(controller.admissionIndex(), nullptr);
    EXPECT_EQ(controller.journalWriter(), nullptr);

    const auto first = controller.lookupPrivateResultV1(catalog(), domain::ExchangeRunId{1}, buy);

    ASSERT_EQ(first.outcome, CompletedResultLookupOutcome::FOUND);
    ASSERT_TRUE(first.privateResult.has_value());
    EXPECT_EQ(first.privateResult->recipient, buy.clientId);
    ASSERT_TRUE(first.privateResult->correlation.has_value());
    EXPECT_EQ(*first.privateResult->correlation,
              (domain::CommandResultCorrelation{buy.clientId, *buy.clientCommandId, domain::CommandSequence{2},
                                                domain::ExchangeRunId{1}}));
    ASSERT_TRUE(first.catalogLoad.has_value());
    EXPECT_EQ(first.catalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
    ASSERT_TRUE(first.recovery.has_value());
    EXPECT_EQ(first.recovery->outcome, recovery::RecoveredRunOutcome::RECOVERED);
    ASSERT_EQ(first.privateResult->privateResult.size(), 1U);
    const auto* trade = std::get_if<private_result::PrivateTrade>(&first.privateResult->privateResult[0]);
    ASSERT_NE(trade, nullptr);
    EXPECT_EQ(trade->eventId,
              (domain::EventId{domain::CommandSequence{2}, domain::EventIndex{0}, domain::ExchangeRunId{1}}));
    EXPECT_EQ(trade->orderId, domain::OrderId{2});
    EXPECT_EQ(trade->remainingQuantity, domain::Quantity{0});

    ASSERT_TRUE(std::filesystem::remove(journal()));
    const auto repeated = controller.lookupPrivateResultV1(catalog(), domain::ExchangeRunId{1}, buy);
    EXPECT_EQ(repeated.outcome, CompletedResultLookupOutcome::FOUND);
    EXPECT_EQ(repeated.privateResult, first.privateResult);
    ASSERT_TRUE(repeated.catalogLoad.has_value());
    EXPECT_EQ(repeated.catalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_FALSE(repeated.recovery.has_value());
    EXPECT_EQ(controller.matchingState(), nullptr);
    EXPECT_EQ(controller.admissionIndex(), nullptr);
    EXPECT_EQ(controller.journalWriter(), nullptr);
    EXPECT_FALSE(controller.inspectCommandProcessingV1().installed);
}

TEST_F(ControllerCommandProcessingTest, PrivateLookupPreservesDelegatedRefusalAndRecoveryEvidence) {
    const auto missingCatalog = catalog().parent_path() / "missing-catalog" / "run-catalog-v1";
    ExchangeRunController missing;
    const auto catalogFailure =
        missing.lookupPrivateResultV1(missingCatalog, domain::ExchangeRunId{1}, order("CATALOG-MISSING"));
    EXPECT_EQ(catalogFailure.outcome, CompletedResultLookupOutcome::CATALOG_LOAD_FAILED);
    EXPECT_FALSE(catalogFailure.privateResult.has_value());
    ASSERT_TRUE(catalogFailure.catalogLoad.has_value());
    EXPECT_EQ(catalogFailure.catalogLoad->outcome, storage::RunCatalogLoadOutcome::MISSING);
    EXPECT_FALSE(catalogFailure.recovery.has_value());

    {
        ExchangeRunController owner;
        ASSERT_EQ(owner.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    }
    ExchangeRunController unowned;
    const auto active = unowned.lookupPrivateResultV1(catalog(), domain::ExchangeRunId{1}, order());
    EXPECT_EQ(active.outcome, CompletedResultLookupOutcome::ACTIVE_RUN_NOT_OWNED);
    EXPECT_FALSE(active.privateResult.has_value());
    ASSERT_TRUE(active.catalogLoad.has_value());
    EXPECT_EQ(active.catalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_FALSE(active.recovery.has_value());
    const auto unknown = unowned.lookupPrivateResultV1(catalog(), domain::ExchangeRunId{99}, order("UNKNOWN-PRIVATE"));
    EXPECT_EQ(unknown.outcome, CompletedResultLookupOutcome::RUN_NOT_RETAINED);
    EXPECT_FALSE(unknown.privateResult.has_value());
    ASSERT_TRUE(unknown.catalogLoad.has_value());
    EXPECT_EQ(unknown.catalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_FALSE(unknown.recovery.has_value());

    const auto ownedDirectory = catalog().parent_path() / "owned-private";
    ASSERT_TRUE(std::filesystem::create_directory(ownedDirectory));
    const auto ownedCatalog = ownedDirectory / "run-catalog-v1";
    ExchangeRunController owned;
    ASSERT_EQ(owned.startupNewRunV1(ownedCatalog, configuration()).outcome, NewRunStartupOutcome::READY);
    const auto notFound = owned.lookupPrivateResultV1(ownedCatalog, domain::ExchangeRunId{1}, order("NOT-COMPLETED"));
    EXPECT_EQ(notFound.outcome, CompletedResultLookupOutcome::NOT_FOUND);
    EXPECT_FALSE(notFound.privateResult.has_value());
    EXPECT_FALSE(notFound.catalogLoad.has_value());
    EXPECT_FALSE(notFound.recovery.has_value());

    const auto retainedDirectory = catalog().parent_path() / "missing-private-retained";
    ASSERT_TRUE(std::filesystem::create_directory(retainedDirectory));
    const auto retainedCatalog = retainedDirectory / "run-catalog-v1";
    ASSERT_EQ(storage::replaceRunCatalogV1(retainedCatalog,
                                           storage::RunCatalogSnapshotV1{
                                               .generation = 1,
                                               .lastReservedRunId = domain::ExchangeRunId{1},
                                               .activeRunId = std::nullopt,
                                               .activeDisposition = storage::RunCatalogDisposition::NONE,
                                               .retainedStoppedRunId = domain::ExchangeRunId{1},
                                           })
                  .outcome,
              storage::RunCatalogReplaceOutcome::COMMITTED);
    ExchangeRunController retained;
    const auto recoveryFailure =
        retained.lookupPrivateResultV1(retainedCatalog, domain::ExchangeRunId{1}, order("MISSING-PRIVATE"));
    EXPECT_EQ(recoveryFailure.outcome, CompletedResultLookupOutcome::RECOVERY_FAILED);
    EXPECT_FALSE(recoveryFailure.privateResult.has_value());
    ASSERT_TRUE(recoveryFailure.catalogLoad.has_value());
    EXPECT_EQ(recoveryFailure.catalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
    ASSERT_TRUE(recoveryFailure.recovery.has_value());
    EXPECT_EQ(recoveryFailure.recovery->outcome, recovery::RecoveredRunOutcome::PREPARATION_FAILED);
    EXPECT_EQ(recoveryFailure.recovery->preparation.outcome, storage::RunJournalRecoveryOutcome::MISSING);
}

TEST_F(ControllerCommandProcessingTest, RetainedLookupPreservesMissingAndWrongRunRecoveryEvidence) {
    const auto missingDirectory = catalog().parent_path() / "missing-retained";
    ASSERT_TRUE(std::filesystem::create_directory(missingDirectory));
    const auto missingCatalog = missingDirectory / "run-catalog-v1";
    const storage::RunCatalogSnapshotV1 missingSnapshot{
        .generation = 1,
        .lastReservedRunId = domain::ExchangeRunId{1},
        .activeRunId = std::nullopt,
        .activeDisposition = storage::RunCatalogDisposition::NONE,
        .retainedStoppedRunId = domain::ExchangeRunId{1},
    };
    ASSERT_EQ(storage::replaceRunCatalogV1(missingCatalog, missingSnapshot).outcome,
              storage::RunCatalogReplaceOutcome::COMMITTED);
    ExchangeRunController missingLookup;
    const auto missing =
        missingLookup.lookupCompletedResultV1(missingCatalog, domain::ExchangeRunId{1}, order("MISSING"));
    ASSERT_EQ(missing.outcome, CompletedResultLookupOutcome::RECOVERY_FAILED);
    ASSERT_TRUE(missing.recovery.has_value());
    EXPECT_EQ(missing.recovery->preparation.outcome, storage::RunJournalRecoveryOutcome::MISSING);
    EXPECT_EQ(missingLookup.journalWriter(), nullptr);

    const auto wrongRunDirectory = catalog().parent_path() / "wrong-retained-run";
    ASSERT_TRUE(std::filesystem::create_directory(wrongRunDirectory));
    const auto wrongRunCatalog = wrongRunDirectory / "run-catalog-v1";
    {
        ExchangeRunController source;
        ASSERT_EQ(source.startupNewRunV1(wrongRunCatalog, configuration()).outcome, NewRunStartupOutcome::READY);
    }
    const auto runOneJournal = retainedJournalPath(wrongRunCatalog, domain::ExchangeRunId{1});
    const auto runTwoJournal = retainedJournalPath(wrongRunCatalog, domain::ExchangeRunId{2});
    ASSERT_TRUE(std::filesystem::copy_file(runOneJournal, runTwoJournal));
    const storage::RunCatalogSnapshotV1 wrongRunSnapshot{
        .generation = 3,
        .lastReservedRunId = domain::ExchangeRunId{2},
        .activeRunId = std::nullopt,
        .activeDisposition = storage::RunCatalogDisposition::NONE,
        .retainedStoppedRunId = domain::ExchangeRunId{2},
    };
    ASSERT_EQ(storage::replaceRunCatalogV1(wrongRunCatalog, wrongRunSnapshot).outcome,
              storage::RunCatalogReplaceOutcome::COMMITTED);
    ExchangeRunController wrongRunLookup;
    const auto wrongRun =
        wrongRunLookup.lookupCompletedResultV1(wrongRunCatalog, domain::ExchangeRunId{2}, order("WRONG-RUN"));
    ASSERT_EQ(wrongRun.outcome, CompletedResultLookupOutcome::RECOVERY_FAILED);
    ASSERT_TRUE(wrongRun.recovery.has_value());
    EXPECT_EQ(wrongRun.recovery->preparation.outcome, storage::RunJournalRecoveryOutcome::CORRUPTION);
    EXPECT_EQ(wrongRun.recovery->preparation.validation.error, storage::RunJournalLoadError::EXCHANGE_RUN_ID_MISMATCH);
    EXPECT_EQ(wrongRun.completedResult, nullptr);
    EXPECT_EQ(wrongRunLookup.journalWriter(), nullptr);
    EXPECT_EQ(wrongRunLookup.matchingState(), nullptr);
    EXPECT_EQ(wrongRunLookup.admissionIndex(), nullptr);
}

TEST_F(ControllerCommandProcessingTest, ActiveAndRetainedLookupsCoexistAndCompareExactNormalizedCommands) {
    ExchangeRunController controller;
    const auto retainedCommand = order("RETAINED");
    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, retainedCommand).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    drainPrivateResultHandoff(controller);
    ASSERT_EQ(controller.stopRunV1().outcome, RunStopOutcome::STOPPED);
    const auto retained = controller.lookupCompletedResultV1(catalog(), domain::ExchangeRunId{1}, retainedCommand);
    ASSERT_EQ(retained.outcome, CompletedResultLookupOutcome::FOUND);
    ASSERT_NE(retained.completedResult, nullptr);

    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    const auto activeCommand = order("ACTIVE", true, 3);
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{2}, activeCommand).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    const auto activeIndexResult = controller.admissionIndex()->completedResult(activeCommand);

    const auto active = controller.lookupCompletedResultV1(catalog(), domain::ExchangeRunId{2}, activeCommand);
    ASSERT_EQ(active.outcome, CompletedResultLookupOutcome::FOUND);
    EXPECT_EQ(active.completedResult, activeIndexResult);
    EXPECT_FALSE(active.catalogLoad.has_value());
    EXPECT_FALSE(active.recovery.has_value());
    expectCorrelation(active.completedResult, activeCommand, 1, domain::ExchangeRunId{2});

    const auto retainedAgain = controller.lookupCompletedResultV1(catalog(), domain::ExchangeRunId{1}, retainedCommand);
    EXPECT_EQ(retainedAgain.outcome, CompletedResultLookupOutcome::FOUND);
    EXPECT_EQ(retainedAgain.completedResult, retained.completedResult);
    ASSERT_TRUE(retainedAgain.catalogLoad.has_value());
    EXPECT_EQ(retainedAgain.catalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_FALSE(retainedAgain.recovery.has_value());

    auto mismatched = retainedCommand;
    mismatched.quantity = domain::Quantity{retainedCommand.quantity.value() + 1};
    const auto mismatch = controller.lookupCompletedResultV1(catalog(), domain::ExchangeRunId{1}, mismatched);
    EXPECT_EQ(mismatch.outcome, CompletedResultLookupOutcome::NOT_FOUND);
    EXPECT_EQ(mismatch.completedResult, nullptr);
    EXPECT_EQ(controller.matchingState()->snapshot().activeOrders.size(), 0U);
    EXPECT_EQ(controller.admissionIndex()->size(), 1U);
}

TEST_F(ControllerCommandProcessingTest, LookupRejectsUnownedActiveAndUnknownRunsBeforeOpeningTheirJournalNames) {
    {
        ExchangeRunController owner;
        ASSERT_EQ(owner.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    }

    ExchangeRunController lookup;
    const auto active = lookup.lookupCompletedResultV1(catalog(), domain::ExchangeRunId{1}, order());
    EXPECT_EQ(active.outcome, CompletedResultLookupOutcome::ACTIVE_RUN_NOT_OWNED);
    ASSERT_TRUE(active.catalogLoad.has_value());
    EXPECT_EQ(active.catalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_FALSE(active.recovery.has_value());

    const auto unknown = lookup.lookupCompletedResultV1(catalog(), domain::ExchangeRunId{99}, order("UNKNOWN"));
    EXPECT_EQ(unknown.outcome, CompletedResultLookupOutcome::RUN_NOT_RETAINED);
    EXPECT_FALSE(unknown.recovery.has_value());
}

TEST_F(ControllerCommandProcessingTest, CommittedRetentionReplacementInvalidatesTheOlderCachedView) {
    ExchangeRunController controller;
    const auto oldCommand = order("OLD");
    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, oldCommand).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    drainPrivateResultHandoff(controller);
    ASSERT_EQ(controller.stopRunV1().outcome, RunStopOutcome::STOPPED);
    ASSERT_EQ(controller.lookupCompletedResultV1(catalog(), domain::ExchangeRunId{1}, oldCommand).outcome,
              CompletedResultLookupOutcome::FOUND);

    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    const auto newCommand = order("NEW");
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{2}, newCommand).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    drainPrivateResultHandoff(controller);
    ASSERT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::PAUSED);
    ASSERT_EQ(controller.stopRunV1(domain::ExchangeRunId{1}).outcome, RunStopOutcome::STOPPED);
    EXPECT_FALSE(std::filesystem::exists(retainedJournalPath(catalog(), domain::ExchangeRunId{1})));

    const auto replaced = controller.lookupCompletedResultV1(catalog(), domain::ExchangeRunId{1}, oldCommand);
    EXPECT_EQ(replaced.outcome, CompletedResultLookupOutcome::RUN_NOT_RETAINED);
    EXPECT_FALSE(replaced.recovery.has_value());
    const auto privateReplaced = controller.lookupPrivateResultV1(catalog(), domain::ExchangeRunId{1}, oldCommand);
    EXPECT_EQ(privateReplaced.outcome, CompletedResultLookupOutcome::RUN_NOT_RETAINED);
    EXPECT_FALSE(privateReplaced.privateResult.has_value());
    ASSERT_TRUE(privateReplaced.catalogLoad.has_value());
    EXPECT_EQ(privateReplaced.catalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_FALSE(privateReplaced.recovery.has_value());
    const auto newlyRetained = controller.lookupCompletedResultV1(catalog(), domain::ExchangeRunId{2}, newCommand);
    EXPECT_EQ(newlyRetained.outcome, CompletedResultLookupOutcome::FOUND);
    expectCorrelation(newlyRetained.completedResult, newCommand, 1, domain::ExchangeRunId{2});
}

TEST_F(ControllerCommandProcessingTest, CleanupFailureStillInvalidatesTheOlderCachedView) {
    ExchangeRunController controller;
    const auto oldCommand = order("OLD");
    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, oldCommand).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    drainPrivateResultHandoff(controller);
    ASSERT_EQ(controller.stopRunV1().outcome, RunStopOutcome::STOPPED);
    ASSERT_EQ(controller.lookupCompletedResultV1(catalog(), domain::ExchangeRunId{1}, oldCommand).outcome,
              CompletedResultLookupOutcome::FOUND);

    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    const auto newCommand = order("NEW");
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{2}, newCommand).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(controller.advanceCommandProcessingV1());
    drainPrivateResultHandoff(controller);
    ASSERT_EQ(controller.pauseRunV1().outcome, RunPauseOutcome::PAUSED);

    const auto oldJournal = retainedJournalPath(catalog(), domain::ExchangeRunId{1});
    StopCleanupProbe probe{
        .controller = &controller, .catalogPath = catalog(), .expectedRemovedPath = oldJournal, .failRemove = true};
    auto hooks = storage::detail::systemRunCatalogStorageHooks();
    hooks.context = &probe;
    hooks.syncFile = stopCleanupSync;
    hooks.removeFile = stopCleanupRemove;
    const auto stopped = detail::stopRunV1WithHooks(domain::ExchangeRunId{1}, hooks, controller);
    ASSERT_EQ(stopped.outcome, RunStopOutcome::STOPPED_CLEANUP_FAILED);
    ASSERT_EQ(stopped.cleanup.outcome, StoppedRunCleanupOutcome::REMOVE_FAILED);
    ASSERT_TRUE(std::filesystem::exists(oldJournal));

    const auto replaced = controller.lookupCompletedResultV1(catalog(), domain::ExchangeRunId{1}, oldCommand);
    EXPECT_EQ(replaced.outcome, CompletedResultLookupOutcome::RUN_NOT_RETAINED);
    EXPECT_FALSE(replaced.recovery.has_value());
    const auto retained = controller.lookupCompletedResultV1(catalog(), domain::ExchangeRunId{2}, newCommand);
    EXPECT_EQ(retained.outcome, CompletedResultLookupOutcome::FOUND);
    expectCorrelation(retained.completedResult, newCommand, 1, domain::ExchangeRunId{2});
}

TEST_F(ControllerCommandProcessingTest, StopRefusesUnavailableFailedRecoveryFailedAndAlreadyStoppedStates) {
    ExchangeRunController unavailable;
    EXPECT_EQ(unavailable.stopRunV1().outcome, RunStopOutcome::NOT_STOPPABLE);

    const auto failedDirectory = catalog().parent_path() / "stop-fail-stopped";
    ASSERT_TRUE(std::filesystem::create_directory(failedDirectory));
    const auto failedCatalog = failedDirectory / "run-catalog-v1";
    AppendProbe failedProbe{.fault = AppendFault::IMMEDIATE_WRITE};
    ExchangeRunController failed;
    failedProbe.controller = &failed;
    ASSERT_EQ(startProbedRun(failedCatalog, failedProbe, failed).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(failed.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    ASSERT_EQ(failed.submitCommandV1(domain::ExchangeRunId{1}, order("FIRST")).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(failed.advanceCommandProcessingV1());
    drainPrivateResultHandoff(failed);
    ASSERT_EQ(failed.submitCommandV1(domain::ExchangeRunId{1}, order("FAIL")).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_FALSE(failed.advanceCommandProcessingV1());
    ASSERT_EQ(failed.state(), ExchangeRunStartupState::FAIL_STOPPED);
    EXPECT_EQ(failed.stopRunV1().outcome, RunStopOutcome::NOT_STOPPABLE);

    corruptLastJournalByte(failedDirectory / "run-1.fxjr");
    ASSERT_EQ(failed.recoverFailedRunV1(failedCatalog).outcome, FailedRunRecoveryOutcome::RECOVERY_FAILED);
    ASSERT_EQ(failed.state(), ExchangeRunStartupState::RECOVERY_FAILED);
    EXPECT_EQ(failed.stopRunV1().outcome, RunStopOutcome::NOT_STOPPABLE);

    const auto stoppedDirectory = catalog().parent_path() / "already-stopped";
    ASSERT_TRUE(std::filesystem::create_directory(stoppedDirectory));
    ExchangeRunController stopped;
    ASSERT_EQ(stopped.startupNewRunV1(stoppedDirectory / "run-catalog-v1", configuration()).outcome,
              NewRunStartupOutcome::READY);
    ASSERT_TRUE(stopped.installCommandProcessingV1(1, 1, 1, 16'384, 4'096));
    ASSERT_EQ(stopped.pauseRunV1().outcome, RunPauseOutcome::PAUSED);
    ASSERT_EQ(stopped.stopRunV1().outcome, RunStopOutcome::STOPPED);
    EXPECT_EQ(stopped.stopRunV1().outcome, RunStopOutcome::NOT_STOPPABLE);
}

} // namespace
} // namespace exchange::core
