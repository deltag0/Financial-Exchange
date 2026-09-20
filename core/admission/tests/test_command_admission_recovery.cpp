#include "command_admission_recovery.hpp"
#include "admission_test_access.hpp"

#include "../../storage/include/run_journal_recovery.hpp"
#include "command_result.hpp"
#include "run_journal_replay.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace exchange::core::admission {
namespace {

constexpr domain::ExchangeRunId RUN_ID{84};

storage::RunHeaderV1 recoveryHeader(const std::uint64_t maxRunCommands = 8) {
    return {
        .exchangeRunId = RUN_ID,
        .behavioralRulesVersion = 1,
        .maxEventsPerCommand = 4'096,
        .maxRunCommands = maxRunCommands,
        .maxRunJournalBytes = 1'000'000,
        .instruments = {{domain::InstrumentId{1}, 1}},
    };
}

storage::JournalNewOrderV1 recoveryOrder(const std::uint64_t sequence, const domain::ClientId clientId,
                                         const std::string &clientCommandId, const domain::Side side,
                                         const std::uint64_t quantity,
                                         const task::TimeInForce timeInForce = task::TimeInForce::GTC) {
    return {
        .exchangeRunId = RUN_ID,
        .commandSequence = domain::CommandSequence{sequence},
        .behavioralRulesVersion = 1,
        .configurationVersion = 1,
        .clientId = clientId,
        .instrumentId = domain::InstrumentId{1},
        .clientCommandId = domain::ClientCommandId{clientCommandId},
        .side = side,
        .timeInForce = timeInForce,
        .price = domain::Price{100},
        .quantity = domain::Quantity{quantity},
    };
}

storage::JournalCancelV1 recoveryCancel(const std::uint64_t sequence, const domain::ClientId clientId,
                                        const std::string &clientCommandId, const std::uint64_t targetOrderId) {
    return {
        .exchangeRunId = RUN_ID,
        .commandSequence = domain::CommandSequence{sequence},
        .behavioralRulesVersion = 1,
        .configurationVersion = 1,
        .clientId = clientId,
        .instrumentId = domain::InstrumentId{1},
        .clientCommandId = domain::ClientCommandId{clientCommandId},
        .targetOrderId = domain::TargetOrderId{targetOrderId},
    };
}

storage::LoadedRunJournalV1 recoveryJournal(std::vector<storage::JournalCommandV1> commands,
                                            const std::uint64_t maxRunCommands = 8) {
    return {
        .header = recoveryHeader(maxRunCommands),
        .commands = std::move(commands),
        .validCommittedByteCount = 1,
    };
}

matching_engine::ReplayedRunJournalV1 replay(const storage::LoadedRunJournalV1 &journal) {
    storage::PreparedRunJournalV1 prepared;
    prepared.journal = journal;
    matching_engine::ReplayedRunJournalV1 replayed;
    EXPECT_EQ(matching_engine::replayPreparedRunJournalV1(prepared, replayed).outcome,
              matching_engine::RunJournalReplayOutcome::REPLAYED);
    return replayed;
}

matching_engine::ImmutableCommandResultBatch syntheticResult(
    const domain::ClientId clientId, const std::string &clientCommandId, const domain::CommandSequence sequence,
    const domain::ExchangeRunId runId, const bool includeEvent = true, const domain::EventIndex eventIndex = {}) {
    std::vector<domain::BusinessEvent> events;
    if (includeEvent) {
        events.emplace_back(domain::OrderRested{
            .eventId = {.commandSequence = sequence, .eventIndex = eventIndex, .exchangeRunId = runId},
            .orderId = domain::orderIdFrom(sequence),
            .clientId = clientId,
            .instrumentId = domain::InstrumentId{1},
            .side = domain::Side::BUY,
            .price = domain::Price{100},
            .remainingQuantity = domain::Quantity{1},
        });
    }
    return std::make_shared<const matching_engine::CommandResultBatch>(
        domain::CommandResultCorrelation{
            .clientId = clientId,
            .clientCommandId = domain::ClientCommandId{clientCommandId},
            .commandSequence = sequence,
            .exchangeRunId = runId,
        },
        matching_engine::ProcessingResult::APPLIED, std::move(events));
}

void expectRecoveryFailure(const storage::LoadedRunJournalV1 &journal,
                           const std::vector<matching_engine::ImmutableCommandResultBatch> &results,
                           const CommandAdmissionRecoveryOutcome expectedOutcome,
                           const CommandAdmissionRecoveryError expectedError) {
    auto output = std::make_unique<CommandAdmissionIndex>(7);
    CommandAdmissionIndex *const original = output.get();

    const CommandAdmissionRecoveryResult recovery = reconstructCommandAdmissionIndex(journal, results, output);

    EXPECT_EQ(recovery.outcome, expectedOutcome);
    EXPECT_EQ(recovery.error, expectedError);
    EXPECT_EQ(output.get(), original);
    EXPECT_EQ(output->capacity(), 7U);
    EXPECT_EQ(output->size(), 0U);
}

TEST(CommandAdmissionRecoveryTest, EmptyJournalCreatesRunBoundIndexWithZeroLiveStatistics) {
    const storage::LoadedRunJournalV1 journal = recoveryJournal({});
    const matching_engine::ReplayedRunJournalV1 replayed = replay(journal);
    std::unique_ptr<CommandAdmissionIndex> recovered;

    const CommandAdmissionRecoveryResult result =
        reconstructCommandAdmissionIndex(journal, replayed.commandResults, recovered);

    EXPECT_EQ(result.outcome, CommandAdmissionRecoveryOutcome::RECONSTRUCTED);
    EXPECT_EQ(result.error, CommandAdmissionRecoveryError::NONE);
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->capacity(), journal.header.maxRunCommands);
    EXPECT_EQ(recovered->size(), 0U);
    EXPECT_EQ(recovered->statistics(), AdmissionStatistics{});
}

TEST(CommandAdmissionRecoveryTest, MixedJournalRestoresExactCompletedLookupAndExistingConflictBehavior) {
    std::vector<storage::JournalCommandV1> commands;
    commands.emplace_back(recoveryOrder(1, domain::ClientId{101}, "BUY-1", domain::Side::BUY, 10));
    commands.emplace_back(
        recoveryOrder(2, domain::ClientId{202}, "SELL-1", domain::Side::SELL, 4, task::TimeInForce::IOC));
    commands.emplace_back(recoveryCancel(3, domain::ClientId{101}, "CANCEL-1", 1));
    const storage::LoadedRunJournalV1 journal = recoveryJournal(std::move(commands));
    const matching_engine::ReplayedRunJournalV1 replayed = replay(journal);
    std::unique_ptr<CommandAdmissionIndex> recovered;
    ASSERT_EQ(reconstructCommandAdmissionIndex(journal, replayed.commandResults, recovered).outcome,
              CommandAdmissionRecoveryOutcome::RECONSTRUCTED);

    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->size(), journal.commands.size());
    EXPECT_EQ(recovered->statistics(), AdmissionStatistics{});

    sequencer::sequenceMessage buy{};
    buy.clientId = domain::ClientId{101};
    buy.clientCommandId = domain::ClientCommandId{"BUY-1"};
    buy.instrumentId = domain::InstrumentId{1};
    buy.configurationVersion = 1;
    buy.price = domain::Price{100};
    buy.quantity = domain::Quantity{10};
    buy.type = sequencer::orderType::BUY;
    buy.tif = task::TimeInForce::GTC;

    sequencer::sequenceMessage sell{};
    sell.clientId = domain::ClientId{202};
    sell.clientCommandId = domain::ClientCommandId{"SELL-1"};
    sell.instrumentId = domain::InstrumentId{1};
    sell.configurationVersion = 1;
    sell.price = domain::Price{100};
    sell.quantity = domain::Quantity{4};
    sell.type = sequencer::orderType::SELL;
    sell.tif = task::TimeInForce::IOC;

    sequencer::sequenceMessage cancel{};
    cancel.targetOrderId = domain::TargetOrderId{1};
    cancel.clientId = domain::ClientId{101};
    cancel.clientCommandId = domain::ClientCommandId{"CANCEL-1"};
    cancel.instrumentId = domain::InstrumentId{1};
    cancel.configurationVersion = 1;
    cancel.type = sequencer::orderType::CANCEL;

    EXPECT_EQ(recovered->completedResult(buy), replayed.commandResults[0]);
    EXPECT_EQ(recovered->completedResult(sell), replayed.commandResults[1]);
    EXPECT_EQ(recovered->completedResult(cancel), replayed.commandResults[2]);

    const AdmissionDecision duplicate = recovered->reserve(buy);
    EXPECT_EQ(duplicate.status, AdmissionStatus::IDENTICAL_COMPLETED);
    EXPECT_EQ(duplicate.originalResult, replayed.commandResults[0]);

    sequencer::sequenceMessage conflict = buy;
    conflict.price = domain::Price{101};
    const AdmissionDecision conflictingReuse = recovered->reserve(conflict);
    EXPECT_EQ(conflictingReuse.status, AdmissionStatus::CONFLICTING_REUSE);
    EXPECT_EQ(conflictingReuse.rejectionReason, std::optional<domain::AdmissionRejectionReason>{
                                                    domain::AdmissionRejectionReason::DUPLICATE_COMMAND_CONFLICT});
    EXPECT_EQ(recovered->completedResult(buy), replayed.commandResults[0]);
}

TEST(CommandAdmissionRecoveryTest, RunBoundCompletionRejectsWrongRunWithoutChangingSequencedRecord) {
    const storage::LoadedRunJournalV1 journal = recoveryJournal({});
    std::unique_ptr<CommandAdmissionIndex> recovered;
    ASSERT_EQ(reconstructCommandAdmissionIndex(journal, {}, recovered).outcome,
              CommandAdmissionRecoveryOutcome::RECONSTRUCTED);
    ASSERT_NE(recovered, nullptr);

    sequencer::sequenceMessage command{};
    command.globalSequenceNumber = domain::CommandSequence{1};
    command.clientId = domain::ClientId{101};
    command.clientCommandId = domain::ClientCommandId{"NEW-1"};
    command.instrumentId = domain::InstrumentId{1};
    command.configurationVersion = 1;
    command.price = domain::Price{100};
    command.quantity = domain::Quantity{1};
    command.type = sequencer::orderType::BUY;
    command.tif = task::TimeInForce::GTC;

    EXPECT_EQ(recovered->reserve(command).status, AdmissionStatus::ADMISSION_UNAVAILABLE);
    CommandAdmissionIndexTestAccess::setAdmissionOpen(*recovered, true);
    ASSERT_EQ(recovered->reserve(command).status, AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_EQ(recovered->markSequenced(command), MarkSequencedStatus::SEQUENCED);
    const matching_engine::ImmutableCommandResultBatch wrongRun = syntheticResult(
        command.clientId, "NEW-1", command.globalSequenceNumber, domain::ExchangeRunId{RUN_ID.value() + 1});
    EXPECT_EQ(recovered->complete(wrongRun), CompletionStatus::CORRELATION_MISMATCH);
    EXPECT_EQ(recovered->completedResult(command), nullptr);
    EXPECT_EQ(recovered->size(), 1U);

    const matching_engine::ImmutableCommandResultBatch correctRun =
        syntheticResult(command.clientId, "NEW-1", command.globalSequenceNumber, RUN_ID);
    EXPECT_EQ(recovered->complete(correctRun), CompletionStatus::COMPLETED);
    EXPECT_EQ(recovered->completedResult(command), correctRun);
}

TEST(CommandAdmissionRecoveryTest, RejectsMalformedCommandAndResultPairingsWithoutReplacingOutput) {
    const storage::LoadedRunJournalV1 validJournal =
        recoveryJournal({recoveryOrder(1, domain::ClientId{101}, "BUY-1", domain::Side::BUY, 10)});
    const matching_engine::ReplayedRunJournalV1 replayed = replay(validJournal);

    expectRecoveryFailure(validJournal, {}, CommandAdmissionRecoveryOutcome::INVALID_INPUT,
                          CommandAdmissionRecoveryError::COMMAND_RESULT_COUNT_MISMATCH);

    storage::LoadedRunJournalV1 gap = validJournal;
    std::get<storage::JournalNewOrderV1>(gap.commands[0]).commandSequence = domain::CommandSequence{2};
    expectRecoveryFailure(gap, replayed.commandResults, CommandAdmissionRecoveryOutcome::INVALID_INPUT,
                          CommandAdmissionRecoveryError::COMMAND_SEQUENCE_MISMATCH);

    storage::LoadedRunJournalV1 wrongContext = validJournal;
    std::get<storage::JournalNewOrderV1>(wrongContext.commands[0]).exchangeRunId = domain::ExchangeRunId{85};
    expectRecoveryFailure(wrongContext, replayed.commandResults, CommandAdmissionRecoveryOutcome::INVALID_INPUT,
                          CommandAdmissionRecoveryError::COMMAND_CONTEXT_MISMATCH);

    expectRecoveryFailure(
        validJournal, {syntheticResult(domain::ClientId{999}, "BUY-1", domain::CommandSequence{1}, RUN_ID)},
        CommandAdmissionRecoveryOutcome::INVALID_INPUT, CommandAdmissionRecoveryError::RESULT_CORRELATION_MISMATCH);
    expectRecoveryFailure(
        validJournal, {syntheticResult(domain::ClientId{101}, "OTHER", domain::CommandSequence{1}, RUN_ID)},
        CommandAdmissionRecoveryOutcome::INVALID_INPUT, CommandAdmissionRecoveryError::RESULT_CORRELATION_MISMATCH);
    expectRecoveryFailure(
        validJournal,
        {syntheticResult(domain::ClientId{101}, "BUY-1", domain::CommandSequence{1}, domain::ExchangeRunId{85})},
        CommandAdmissionRecoveryOutcome::INVALID_INPUT, CommandAdmissionRecoveryError::RESULT_CORRELATION_MISMATCH);
    expectRecoveryFailure(
        validJournal, {syntheticResult(domain::ClientId{101}, "BUY-1", domain::CommandSequence{1}, RUN_ID, false)},
        CommandAdmissionRecoveryOutcome::INVALID_INPUT, CommandAdmissionRecoveryError::RESULT_EVENT_IDENTITY_MISMATCH);
    expectRecoveryFailure(validJournal,
                          {syntheticResult(domain::ClientId{101}, "BUY-1", domain::CommandSequence{1}, RUN_ID, true,
                                           domain::EventIndex{1})},
                          CommandAdmissionRecoveryOutcome::INVALID_INPUT,
                          CommandAdmissionRecoveryError::RESULT_EVENT_IDENTITY_MISMATCH);
    expectRecoveryFailure(validJournal, {nullptr}, CommandAdmissionRecoveryOutcome::INVALID_INPUT,
                          CommandAdmissionRecoveryError::RESULT_CORRELATION_MISMATCH);
}

TEST(CommandAdmissionRecoveryTest, RejectsDuplicateCommandKeysAndInvalidNormalizedCommands) {
    std::vector<storage::JournalCommandV1> duplicateCommands;
    duplicateCommands.emplace_back(recoveryOrder(1, domain::ClientId{101}, "SAME", domain::Side::BUY, 10));
    duplicateCommands.emplace_back(recoveryOrder(2, domain::ClientId{101}, "SAME", domain::Side::SELL, 1));
    const storage::LoadedRunJournalV1 duplicateJournal = recoveryJournal(std::move(duplicateCommands));
    const matching_engine::ReplayedRunJournalV1 duplicateReplay = replay(duplicateJournal);
    expectRecoveryFailure(duplicateJournal, duplicateReplay.commandResults,
                          CommandAdmissionRecoveryOutcome::INVALID_INPUT,
                          CommandAdmissionRecoveryError::DUPLICATE_COMMAND_KEY);

    storage::LoadedRunJournalV1 invalidCommand =
        recoveryJournal({recoveryOrder(1, domain::ClientId{101}, "BUY-1", domain::Side::BUY, 10)});
    const matching_engine::ReplayedRunJournalV1 validReplay = replay(invalidCommand);
    std::get<storage::JournalNewOrderV1>(invalidCommand.commands[0]).side = static_cast<domain::Side>(255);
    expectRecoveryFailure(invalidCommand, validReplay.commandResults, CommandAdmissionRecoveryOutcome::INVALID_INPUT,
                          CommandAdmissionRecoveryError::INVALID_NORMALIZED_COMMAND);
}

TEST(CommandAdmissionRecoveryTest, EnforcesJournalCommandCapacityWithoutReplacingOutput) {
    const storage::LoadedRunJournalV1 validJournal =
        recoveryJournal({recoveryOrder(1, domain::ClientId{101}, "BUY-1", domain::Side::BUY, 10),
                         recoveryOrder(2, domain::ClientId{102}, "BUY-2", domain::Side::BUY, 10)},
                        2);
    const matching_engine::ReplayedRunJournalV1 replayed = replay(validJournal);
    storage::LoadedRunJournalV1 overCapacity = validJournal;
    overCapacity.header.maxRunCommands = 1;

    expectRecoveryFailure(overCapacity, replayed.commandResults, CommandAdmissionRecoveryOutcome::CAPACITY_ERROR,
                          CommandAdmissionRecoveryError::COMMAND_COUNT_CAPACITY_EXCEEDED);

    storage::LoadedRunJournalV1 zeroCapacity = recoveryJournal({});
    zeroCapacity.header.maxRunCommands = 0;
    expectRecoveryFailure(zeroCapacity, {}, CommandAdmissionRecoveryOutcome::CAPACITY_ERROR,
                          CommandAdmissionRecoveryError::CAPACITY_UNREPRESENTABLE);
}

} // namespace
} // namespace exchange::core::admission
