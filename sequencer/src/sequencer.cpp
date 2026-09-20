#include "../include/sequencer.hpp"
#include "../../core/task/include/adaptive_idle.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

const char* markSequencedStatusName(const exchange::core::admission::MarkSequencedStatus status) {
    using exchange::core::admission::MarkSequencedStatus;
    switch (status) {
        case MarkSequencedStatus::SEQUENCED:
            return "Sequenced";
        case MarkSequencedStatus::IDEMPOTENT:
            return "Idempotent";
        case MarkSequencedStatus::INVALID_SEQUENCE:
            return "InvalidSequence";
        case MarkSequencedStatus::UNKNOWN_RESERVATION:
            return "UnknownReservation";
        case MarkSequencedStatus::COMMAND_MISMATCH:
            return "CommandMismatch";
        case MarkSequencedStatus::WRONG_STATE:
            return "WrongState";
        case MarkSequencedStatus::SEQUENCE_MISMATCH:
            return "SequenceMismatch";
    }
    return "UnknownMarkSequencedStatus";
}

} // namespace

exchange::sequencer::Sequencer::Sequencer(core::SharedQueue<sequenceMessage>& sequencingIngressQueue,
                                          core::SharedQueue<sequenceMessage>& matchingEngineQueue,
                                          core::admission::CommandAdmissionIndex& admissionIndex,
                                          storage::RunJournalWriterV1& journalWriter)
    : sequencingIngressQueue_(sequencingIngressQueue),
      matchingEngineQueue_(matchingEngineQueue),
      admissionIndex_(&admissionIndex),
      journalWriter_(&journalWriter) {
    if (admissionIndex.exchangeRunId() != journalWriter.header().exchangeRunId) {
        throw std::invalid_argument("journal-backed sequencer requires admission bound to the writer's run");
    }
}

void exchange::sequencer::Sequencer::run() {
    std::cout << "[Sequencer] Thread started" << std::endl;
    core::task::AdaptiveIdle idle;
    while (true) {
        if (advance()) {
            idle.reset();
        } else {
            idle.wait();
        }
    }
}

bool exchange::sequencer::Sequencer::advance() {
    return drainAvailable();
}

bool exchange::sequencer::Sequencer::drainAvailable() {
    bool progressed = false;
    while (processNext()) {
        progressed = true;
    }
    return progressed;
}

bool exchange::sequencer::Sequencer::processNext() {
    if (pendingCommand_.has_value()) {
        return pendingReadyForMatching_ ? handoffPendingCommand() : false;
    }

    if (journalWriter_ == nullptr &&
        lastAssignedSequence_.value() == std::numeric_limits<domain::CommandSequence::Underlying>::max()) {
        if (sequencingIngressQueue_.empty()) {
            return false;
        }
        throw std::overflow_error("CommandSequence exhausted");
    }

    sequenceMessage message{};
    if (!sequencingIngressQueue_.pop(message)) {
        return false;
    }

    pendingCommand_ = message;
    auto& pending = *pendingCommand_;
    pending.globalSequenceNumber =
        journalWriter_ == nullptr ? nextCommandSequence() : journalWriter_->nextCommandSequence();
    if (journalWriter_ != nullptr) {
        const auto result = appendCommand(pending);
        if (result.outcome != storage::RunJournalAppendOutcome::COMMITTED) {
            journalAppendFailure_ = result;
            return false;
        }
    }
    if (pending.type == orderType::BUY || pending.type == orderType::SELL) {
        pending.orderId = domain::orderIdFrom(pending.globalSequenceNumber);
    }
    if (admissionIndex_ != nullptr) {
        const core::admission::MarkSequencedStatus status = admissionIndex_->markSequenced(pending);
        if (status != core::admission::MarkSequencedStatus::SEQUENCED) {
            throw std::logic_error("sequencer admission binding failed: " +
                                   std::string{markSequencedStatusName(status)});
        }
    }

    pendingReadyForMatching_ = true;
    return handoffPendingCommand();
}

bool exchange::sequencer::Sequencer::handoffPendingCommand() {
    if (!pendingCommand_.has_value()) {
        return true;
    }
    if (!matchingEngineQueue_.push(*pendingCommand_)) {
        return false;
    }
    pendingCommand_.reset();
    pendingReadyForMatching_ = false;
    return true;
}

exchange::storage::RunJournalAppendResult exchange::sequencer::Sequencer::appendCommand(
    const sequenceMessage& message) noexcept {
    using storage::JournalCommandCodecError;
    using storage::RunJournalAppendError;
    using storage::RunJournalAppendOutcome;
    if (!message.clientCommandId.has_value()) {
        return {.outcome = RunJournalAppendOutcome::INVALID_COMMAND,
                .codecError = JournalCommandCodecError::INVALID_CLIENT_COMMAND_ID};
    }
    // The live representation is wider than V1; never silently truncate a configuration version.
    if (message.configurationVersion > std::numeric_limits<std::uint32_t>::max()) {
        return {.outcome = RunJournalAppendOutcome::INVALID_COMMAND,
                .error = RunJournalAppendError::INSTRUMENT_CONFIGURATION_MISMATCH};
    }
    const auto& header = journalWriter_->header();
    const auto configurationVersion = static_cast<std::uint32_t>(message.configurationVersion);
    if (message.type == orderType::BUY || message.type == orderType::SELL) {
        return journalWriter_->append(storage::JournalNewOrderV1{
            .exchangeRunId = header.exchangeRunId,
            .commandSequence = message.globalSequenceNumber,
            .behavioralRulesVersion = header.behavioralRulesVersion,
            .configurationVersion = configurationVersion,
            .clientId = message.clientId,
            .instrumentId = message.instrumentId,
            .clientCommandId = *message.clientCommandId,
            .side = message.type == orderType::BUY ? domain::Side::BUY : domain::Side::SELL,
            .timeInForce = message.tif,
            .price = message.price,
            .quantity = message.quantity,
        });
    }
    if (message.type != orderType::CANCEL) {
        return {.outcome = RunJournalAppendOutcome::INVALID_COMMAND,
                .codecError = JournalCommandCodecError::UNSUPPORTED_RECORD_TYPE};
    }
    if (!message.targetOrderId.has_value()) {
        return {.outcome = RunJournalAppendOutcome::INVALID_COMMAND,
                .codecError = JournalCommandCodecError::ZERO_TARGET_ORDER_ID};
    }
    return journalWriter_->append(storage::JournalCancelV1{
        .exchangeRunId = header.exchangeRunId,
        .commandSequence = message.globalSequenceNumber,
        .behavioralRulesVersion = header.behavioralRulesVersion,
        .configurationVersion = configurationVersion,
        .clientId = message.clientId,
        .instrumentId = message.instrumentId,
        .clientCommandId = *message.clientCommandId,
        .targetOrderId = *message.targetOrderId,
    });
}

exchange::domain::CommandSequence exchange::sequencer::Sequencer::nextCommandSequence() {
    lastAssignedSequence_ = domain::CommandSequence{lastAssignedSequence_.value() + 1};
    return lastAssignedSequence_;
}

void exchange::sequencer::Sequencer::send(sequenceMessage&) {}
