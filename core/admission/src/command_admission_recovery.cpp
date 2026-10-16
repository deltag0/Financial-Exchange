#include "command_admission_recovery.hpp"

#include "../../../matching_engine/include/command_result.hpp"
#include "../../storage/include/run_journal_reader.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
#include <variant>

namespace exchange::core::admission {
namespace {

CommandAdmissionRecoveryResult recoveryResult(
    const CommandAdmissionRecoveryOutcome outcome,
    const CommandAdmissionRecoveryError error = CommandAdmissionRecoveryError::NONE) noexcept {
    return {outcome, error};
}

template <typename Command>
AdmissionKey admissionKeyFrom(const Command &command) {
    if (command.clientId.value() == 0) {
        throw std::invalid_argument("recovered command has invalid client identity");
    }
    return {.clientId = command.clientId, .clientCommandId = command.clientCommandId};
}

NormalizedBusinessCommand normalizedCommandFrom(const storage::JournalNewOrderV1 &command) {
    if (command.side != domain::Side::BUY && command.side != domain::Side::SELL) {
        throw std::invalid_argument("recovered NewOrder has invalid side");
    }
    if (command.instrumentId.value() == 0 || command.configurationVersion == 0 || command.price.value() == 0 ||
        command.quantity.value() == 0) {
        throw std::invalid_argument("recovered NewOrder has invalid normalized fields");
    }
    return NormalizedNewOrder{
        .instrumentId = command.instrumentId,
        .configurationVersion = command.configurationVersion,
        .side = command.side,
        .price = command.price,
        .quantity = command.quantity,
        .timeInForce = command.timeInForce,
    };
}

NormalizedBusinessCommand normalizedCommandFrom(const storage::JournalCancelV1 &command) {
    if (command.instrumentId.value() == 0 || command.targetOrderId.value() == 0) {
        throw std::invalid_argument("recovered Cancel has invalid normalized fields");
    }
    return NormalizedCancel{
        .instrumentId = command.instrumentId,
        .targetOrderId = command.targetOrderId,
    };
}

bool hasConfiguration(const storage::RunHeaderV1 &header, const domain::InstrumentId instrumentId,
                      const std::uint32_t configurationVersion) noexcept {
    const auto instrument = std::lower_bound(header.instruments.begin(), header.instruments.end(), instrumentId,
                                             [](const storage::RunHeaderInstrumentV1 &entry,
                                                const domain::InstrumentId id) { return entry.instrumentId < id; });
    return instrument != header.instruments.end() && instrument->instrumentId == instrumentId &&
           instrument->configurationVersion == configurationVersion;
}

template <typename Command>
CommandAdmissionRecoveryError validateCommandContext(const storage::RunHeaderV1 &header, const Command &command,
                                                     const domain::CommandSequence expectedSequence) noexcept {
    if (command.commandSequence != expectedSequence) {
        return CommandAdmissionRecoveryError::COMMAND_SEQUENCE_MISMATCH;
    }
    if (command.exchangeRunId != header.exchangeRunId ||
        command.behavioralRulesVersion != header.behavioralRulesVersion ||
        !hasConfiguration(header, command.instrumentId, command.configurationVersion)) {
        return CommandAdmissionRecoveryError::COMMAND_CONTEXT_MISMATCH;
    }
    return CommandAdmissionRecoveryError::NONE;
}

CommandAdmissionRecoveryError validateResult(const AdmissionKey &key,
                                             const matching_engine::ImmutableCommandResultBatch &result,
                                             const domain::ExchangeRunId exchangeRunId,
                                             const domain::CommandSequence expectedSequence) noexcept {
    if (result == nullptr) {
        return CommandAdmissionRecoveryError::RESULT_CORRELATION_MISMATCH;
    }

    const domain::CommandResultCorrelation &correlation = result->correlation();
    if (correlation.clientId != key.clientId || correlation.clientCommandId != key.clientCommandId ||
        correlation.commandSequence != expectedSequence || correlation.exchangeRunId != exchangeRunId) {
        return CommandAdmissionRecoveryError::RESULT_CORRELATION_MISMATCH;
    }
    if (result->events().empty()) {
        return CommandAdmissionRecoveryError::RESULT_EVENT_IDENTITY_MISMATCH;
    }
    std::size_t eventIndex = 0;
    for (const domain::BusinessEvent &event : result->events()) {
        const domain::EventId eventId = std::visit([](const auto &typed) { return typed.eventId; }, event);
        if (eventIndex > std::numeric_limits<domain::EventIndex::Underlying>::max() ||
            eventId.eventIndex != domain::EventIndex{static_cast<domain::EventIndex::Underlying>(eventIndex)}) {
            return CommandAdmissionRecoveryError::RESULT_EVENT_IDENTITY_MISMATCH;
        }
        ++eventIndex;
    }
    return CommandAdmissionRecoveryError::NONE;
}

} // namespace

CommandAdmissionRecoveryResult reconstructCommandAdmissionIndex(
    const storage::LoadedRunJournalV1 &journal,
    const std::vector<matching_engine::ImmutableCommandResultBatch> &commandResults,
    std::unique_ptr<CommandAdmissionIndex> &output) noexcept {
    if (journal.header.exchangeRunId.value() == 0) {
        return recoveryResult(CommandAdmissionRecoveryOutcome::INVALID_INPUT,
                              CommandAdmissionRecoveryError::INVALID_RUN_ID);
    }
    if (journal.commands.size() != commandResults.size()) {
        return recoveryResult(CommandAdmissionRecoveryOutcome::INVALID_INPUT,
                              CommandAdmissionRecoveryError::COMMAND_RESULT_COUNT_MISMATCH);
    }
    if (journal.commands.size() > journal.header.maxRunCommands) {
        return recoveryResult(CommandAdmissionRecoveryOutcome::CAPACITY_ERROR,
                              CommandAdmissionRecoveryError::COMMAND_COUNT_CAPACITY_EXCEEDED);
    }
    if (journal.header.maxRunCommands == 0 || journal.header.maxRunCommands > std::numeric_limits<std::size_t>::max()) {
        return recoveryResult(CommandAdmissionRecoveryOutcome::CAPACITY_ERROR,
                              CommandAdmissionRecoveryError::CAPACITY_UNREPRESENTABLE);
    }

    try {
        auto recovered = std::unique_ptr<CommandAdmissionIndex>(new CommandAdmissionIndex(
            static_cast<std::size_t>(journal.header.maxRunCommands), journal.header.exchangeRunId));
        std::uint64_t expectedSequenceValue = 1;
        for (std::size_t index = 0; index < journal.commands.size(); ++index, ++expectedSequenceValue) {
            const storage::JournalCommandV1 &persistedCommand = journal.commands[index];
            const domain::CommandSequence expectedSequence{expectedSequenceValue};
            const CommandAdmissionRecoveryError contextError = std::visit(
                [&](const auto &command) { return validateCommandContext(journal.header, command, expectedSequence); },
                persistedCommand);
            if (contextError != CommandAdmissionRecoveryError::NONE) {
                return recoveryResult(CommandAdmissionRecoveryOutcome::INVALID_INPUT, contextError);
            }
            const AdmissionKey key =
                std::visit([](const auto &command) { return admissionKeyFrom(command); }, persistedCommand);
            const NormalizedBusinessCommand normalizedCommand =
                std::visit([](const auto &command) { return normalizedCommandFrom(command); }, persistedCommand);

            const CommandAdmissionRecoveryError resultError =
                validateResult(key, commandResults[index], journal.header.exchangeRunId, expectedSequence);
            if (resultError != CommandAdmissionRecoveryError::NONE) {
                return recoveryResult(CommandAdmissionRecoveryOutcome::INVALID_INPUT, resultError);
            }

            const bool inserted = recovered->records_
                                      .emplace(key,
                                               CommandAdmissionIndex::Record{
                                                   .command = normalizedCommand,
                                                   .state = CommandAdmissionIndex::RecordState::COMPLETED,
                                                   .commandSequence = expectedSequence,
                                                   .result = commandResults[index],
                                               })
                                      .second;
            if (!inserted) {
                return recoveryResult(CommandAdmissionRecoveryOutcome::INVALID_INPUT,
                                      CommandAdmissionRecoveryError::DUPLICATE_COMMAND_KEY);
            }
        }

        output = std::move(recovered);
        return recoveryResult(CommandAdmissionRecoveryOutcome::RECONSTRUCTED);
    } catch (const std::invalid_argument &) {
        return recoveryResult(CommandAdmissionRecoveryOutcome::INVALID_INPUT,
                              CommandAdmissionRecoveryError::INVALID_NORMALIZED_COMMAND);
    } catch (const std::bad_alloc &) {
        return recoveryResult(CommandAdmissionRecoveryOutcome::INTERNAL_FAILURE,
                              CommandAdmissionRecoveryError::ALLOCATION_FAILURE);
    } catch (const std::exception &) {
        return recoveryResult(CommandAdmissionRecoveryOutcome::INVARIANT_FAILURE);
    } catch (...) {
        return recoveryResult(CommandAdmissionRecoveryOutcome::INTERNAL_FAILURE);
    }
}

} // namespace exchange::core::admission
