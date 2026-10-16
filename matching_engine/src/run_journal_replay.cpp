#include "run_journal_replay.hpp"

#include "../../core/storage/include/run_journal_recovery.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <utility>
#include <variant>

namespace exchange::matching_engine {
namespace {

RunJournalReplayResult replayResult(const RunJournalReplayOutcome outcome,
                                    const RunJournalReplayError error = RunJournalReplayError::NONE) noexcept {
    return {outcome, error};
}

RunJournalReplayError validateConfiguration(const storage::RunHeaderV1 &header, const domain::InstrumentId instrumentId,
                                            const std::uint32_t configurationVersion) noexcept {
    const auto instrument = std::lower_bound(header.instruments.begin(), header.instruments.end(), instrumentId,
                                             [](const storage::RunHeaderInstrumentV1 &entry,
                                                const domain::InstrumentId id) { return entry.instrumentId < id; });
    if (instrument == header.instruments.end() || instrument->instrumentId != instrumentId ||
        instrument->configurationVersion != configurationVersion) {
        return RunJournalReplayError::INSTRUMENT_CONFIGURATION_MISMATCH;
    }
    return RunJournalReplayError::NONE;
}

sequencer::sequenceMessage matchingMessageFrom(const storage::JournalNewOrderV1 &command) {
    sequencer::sequenceMessage message{};
    message.orderId = domain::orderIdFrom(command.commandSequence);
    message.globalSequenceNumber = command.commandSequence;
    message.clientId = command.clientId;
    message.clientCommandId = command.clientCommandId;
    message.instrumentId = command.instrumentId;
    message.configurationVersion = command.configurationVersion;
    message.price = command.price;
    message.quantity = command.quantity;
    message.type = command.side == domain::Side::BUY ? sequencer::orderType::BUY : sequencer::orderType::SELL;
    message.tif = command.timeInForce;
    return message;
}

sequencer::sequenceMessage matchingMessageFrom(const storage::JournalCancelV1 &command) {
    sequencer::sequenceMessage message{};
    message.targetOrderId = command.targetOrderId;
    message.globalSequenceNumber = command.commandSequence;
    message.clientId = command.clientId;
    message.clientCommandId = command.clientCommandId;
    message.instrumentId = command.instrumentId;
    message.configurationVersion = command.configurationVersion;
    message.type = sequencer::orderType::CANCEL;
    return message;
}

template <typename Command>
RunJournalReplayError validateCommandContext(const storage::RunHeaderV1 &header, const Command &command,
                                             const domain::CommandSequence expectedSequence) noexcept {
    if (command.commandSequence != expectedSequence) {
        return RunJournalReplayError::COMMAND_SEQUENCE_MISMATCH;
    }
    if (command.exchangeRunId != header.exchangeRunId) {
        return RunJournalReplayError::EXCHANGE_RUN_ID_MISMATCH;
    }
    if (command.behavioralRulesVersion != header.behavioralRulesVersion) {
        return RunJournalReplayError::BEHAVIORAL_RULES_VERSION_MISMATCH;
    }
    return validateConfiguration(header, command.instrumentId, command.configurationVersion);
}

} // namespace

RunJournalReplayResult replayPreparedRunJournalV1(const storage::PreparedRunJournalV1 &prepared,
                                                  ReplayedRunJournalV1 &output) noexcept {
    const storage::LoadedRunJournalV1 &journal = prepared.journal;
    std::vector<std::byte> encodedHeader;
    if (storage::encodeRunHeaderV1(journal.header, encodedHeader) != storage::RunHeaderCodecError::NONE) {
        return replayResult(RunJournalReplayOutcome::INVALID_JOURNAL, RunJournalReplayError::HEADER_CONTEXT_MISMATCH);
    }
    if (journal.commands.size() > journal.header.maxRunCommands) {
        return replayResult(RunJournalReplayOutcome::INVALID_JOURNAL, RunJournalReplayError::COMMAND_COUNT_EXCEEDED);
    }

    try {
        ReplayedRunJournalV1 replayed;
        replayed.matchingState = std::make_unique<MatchingState>(journal.header.exchangeRunId);
        replayed.commandResults.reserve(journal.commands.size());
        std::uint64_t expectedSequence = 1;
        for (const storage::JournalCommandV1 &persistedCommand : journal.commands) {
            RunJournalReplayError contextError = RunJournalReplayError::NONE;
            sequencer::sequenceMessage message{};
            std::visit(
                [&](const auto &command) {
                    contextError =
                        validateCommandContext(journal.header, command, domain::CommandSequence{expectedSequence});
                    if (contextError == RunJournalReplayError::NONE) {
                        message = matchingMessageFrom(command);
                    }
                },
                persistedCommand);
            if (contextError != RunJournalReplayError::NONE) {
                return replayResult(RunJournalReplayOutcome::INVALID_JOURNAL, contextError);
            }

            replayed.commandResults.push_back(replayed.matchingState->processCommand(message));
            ++expectedSequence;
        }
        if (!replayed.matchingState->invariantsHold()) {
            return replayResult(RunJournalReplayOutcome::INVARIANT_FAILURE,
                                RunJournalReplayError::MATCHING_INVARIANT_FAILURE);
        }
        output = std::move(replayed);
        return replayResult(RunJournalReplayOutcome::REPLAYED);
    } catch (const std::bad_alloc &) {
        return replayResult(RunJournalReplayOutcome::INTERNAL_FAILURE);
    } catch (const std::exception &) {
        return replayResult(RunJournalReplayOutcome::INVARIANT_FAILURE,
                            RunJournalReplayError::MATCHING_INVARIANT_FAILURE);
    } catch (...) {
        return replayResult(RunJournalReplayOutcome::INTERNAL_FAILURE);
    }
}

} // namespace exchange::matching_engine
