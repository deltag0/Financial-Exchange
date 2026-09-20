#include "exchange_run_controller.hpp"

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
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <variant>

namespace exchange::core {
namespace {

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

void expectUnavailable(const admission::AdmissionDecision& decision) {
    EXPECT_EQ(decision.status, admission::AdmissionStatus::ADMISSION_UNAVAILABLE);
    EXPECT_EQ(decision.rejectionReason, domain::AdmissionRejectionReason::EXCHANGE_RUN_UNAVAILABLE);
}

void expectCorrelation(const matching_engine::ImmutableCommandResultBatch& batch,
                       const sequencer::sequenceMessage& command, const std::uint64_t sequence) {
    ASSERT_NE(batch, nullptr);
    EXPECT_EQ(batch->correlation(),
              (domain::CommandResultCorrelation{command.clientId, *command.clientCommandId,
                                                domain::CommandSequence{sequence}, domain::ExchangeRunId{1}}));
    for (std::size_t index = 0; index < batch->events().size(); ++index) {
        EXPECT_EQ(std::visit([](const auto& event) { return event.eventId; }, batch->events()[index]),
                  (domain::EventId{domain::CommandSequence{sequence},
                                   domain::EventIndex{static_cast<domain::EventIndex::Underlying>(index)},
                                   domain::ExchangeRunId{1}}));
    }
}

TEST_F(ControllerCommandProcessingTest, InstallationIsBoundedOneTimeAndPreservesRunOwners) {
    ExchangeRunController controller;
    EXPECT_FALSE(controller.installCommandProcessingV1(1, 1, 1));
    EXPECT_FALSE(controller.advanceCommandProcessingV1());
    EXPECT_FALSE(controller.inspectCommandProcessingV1().installed);
    expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, order()));
    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    const auto* state = controller.matchingState();
    const auto* writer = controller.journalWriter();
    const auto* admission = controller.admissionIndex();
    EXPECT_THROW((void)controller.installCommandProcessingV1(0, 1, 1), std::invalid_argument);
    EXPECT_THROW((void)controller.installCommandProcessingV1(1, 65'535, 1), std::invalid_argument);
    EXPECT_THROW((void)controller.installCommandProcessingV1(1, 1, 0), std::invalid_argument);
    EXPECT_FALSE(controller.inspectCommandProcessingV1().installed);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1));
    EXPECT_FALSE(controller.installCommandProcessingV1(2, 2, 2));
    EXPECT_EQ(controller.matchingState(), state);
    EXPECT_EQ(controller.journalWriter(), writer);
    EXPECT_EQ(controller.admissionIndex(), admission);
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::READY);
    EXPECT_FALSE(controller.advanceCommandProcessingV1());
}

TEST_F(ControllerCommandProcessingTest, NewRunCompletesDurableOrderAndCancelAndSuppressesDuplicates) {
    ExchangeRunController controller;
    ASSERT_EQ(controller.startupNewRunV1(catalog(), configuration()).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1));
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
        ASSERT_TRUE(producer.installCommandProcessingV1(1, 1, 1));
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
    ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1));
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::PAUSED);
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

TEST_F(ControllerCommandProcessingTest, MatchingQueueAndResultPressureRetainFifoWithoutDuplicateProcessing) {
    // Capacity one exercises sequencer pending handoff; capacity four exercises matcher pending result.
    for (const std::size_t matchingCapacity : {1U, 4U}) {
        // Separate installation directories keep run/catalog ownership exclusive.
        const auto subdirectory = catalog().parent_path() / std::to_string(matchingCapacity);
        ASSERT_TRUE(std::filesystem::create_directory(subdirectory));
        ExchangeRunController controller;
        ASSERT_EQ(controller.startupNewRunV1(subdirectory / "run-catalog-v1", configuration()).outcome,
                  NewRunStartupOutcome::READY);
        ASSERT_TRUE(controller.installCommandProcessingV1(4, matchingCapacity, 1));
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
    ASSERT_TRUE(controller.installCommandProcessingV1(4, 1, 1));
    const auto* state = controller.matchingState();
    const auto* writer = controller.journalWriter();
    const auto* admission = controller.admissionIndex();
    std::array<sequencer::sequenceMessage, 4> commands;
    for (std::size_t index = 0; index < commands.size(); ++index) {
        commands[index] = order("PAUSE-" + std::to_string(index));
        ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, commands[index]).status,
                  admission::AdmissionStatus::FIRST_SUBMISSION);
    }

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

TEST_F(ControllerCommandProcessingTest, DurabilityPrecedesMatchingAndAppendFaultsLatchEvidenceAndAllOwnedWork) {
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
        ASSERT_TRUE(controller.installCommandProcessingV1(3, 3, 1));
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
        EXPECT_EQ(controller.state(), ExchangeRunStartupState::UNAVAILABLE);
        const auto inspection = controller.inspectCommandProcessingV1();
        ASSERT_TRUE(inspection.appendFailure.has_value());
        EXPECT_EQ(inspection.appendFailure->outcome, fault == AppendFault::IMMEDIATE_WRITE
                                                         ? storage::RunJournalAppendOutcome::IO_FAILURE
                                                         : storage::RunJournalAppendOutcome::RECOVERY_REQUIRED);
        EXPECT_EQ(inspection.appendFailure->systemError, fault == AppendFault::SYNC ? EIO : ENOSPC);
        EXPECT_EQ(inspection.appendFailure->error, storage::RunJournalAppendError::NONE);
        EXPECT_EQ(inspection.appendFailure->codecError, storage::JournalCommandCodecError::NONE);
        EXPECT_EQ(inspection.internalFailure, nullptr);
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
        EXPECT_EQ(controller.resumeRunV1().outcome, RunResumeOutcome::NOT_PAUSED);
        EXPECT_FALSE(controller.installCommandProcessingV1(3, 3, 1));
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
    }
}

TEST_F(ControllerCommandProcessingTest, PauseClosesAdmissionBeforeDurableAppend) {
    AppendProbe probe{.checkClosedOnSync = true};
    ExchangeRunController controller;
    probe.controller = &controller;
    ASSERT_EQ(startProbedRun(catalog(), probe, controller).outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(controller.installCommandProcessingV1(2, 1, 1));
    const auto first = order("FIRST");
    ASSERT_EQ(controller.submitCommandV1(domain::ExchangeRunId{1}, first).status,
              admission::AdmissionStatus::FIRST_SUBMISSION);

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
    ASSERT_TRUE(controller.installCommandProcessingV1(3, 1, 1));
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
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::UNAVAILABLE);
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
    EXPECT_EQ(snapshot.activeDisposition, storage::RunCatalogDisposition::OPEN);
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
    ASSERT_TRUE(controller.installCommandProcessingV1(2, 1, 1));
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
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::UNAVAILABLE);
    expectUnavailable(controller.submitCommandV1(domain::ExchangeRunId{1}, order("AFTER-MATCH-FAIL")));
    EXPECT_FALSE(controller.advanceCommandProcessingV1());
    storage::RunCatalogSnapshotV1 snapshot;
    ASSERT_EQ(storage::loadRunCatalogV1(catalog(), snapshot).outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(snapshot.activeDisposition, storage::RunCatalogDisposition::OPEN);
}

ssize_t failPauseCatalogWrite(void*, int, const void*, std::size_t) noexcept {
    errno = EIO;
    return -1;
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
        ASSERT_TRUE(controller.installCommandProcessingV1(1, 1, 1));
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
            hooks.writeFile = failPauseCatalogWrite;
        }

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

} // namespace
} // namespace exchange::core
