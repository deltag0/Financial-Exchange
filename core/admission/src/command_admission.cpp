#include "command_admission.hpp"

#include "../../../matching_engine/include/command_result_queue.hpp"

#include <stdexcept>
#include <utility>

namespace exchange::core::admission {
namespace {

domain::Side toDomainSide(const sequencer::orderType side) {
    if (side == sequencer::orderType::BUY) {
        return domain::Side::BUY;
    }
    if (side == sequencer::orderType::SELL) {
        return domain::Side::SELL;
    }
    throw std::invalid_argument("admission NewOrder must have BUY or SELL side");
}

} // namespace

CommandAdmissionIndex::CommandAdmissionIndex(const std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("command-admission capacity must be positive");
    }
}

AdmissionDecision CommandAdmissionIndex::reserve(const sequencer::sequenceMessage& command) {
    const AdmissionKey key = keyFrom(command);
    const NormalizedBusinessCommand businessCommand = businessCommandFrom(command);

    std::lock_guard lock(mutex_);
    const auto existing = records_.find(key);
    if (existing != records_.end()) {
        if (existing->second.command != businessCommand) {
            ++statistics_.conflictingReuse;
            return AdmissionDecision{
                .status = AdmissionStatus::CONFLICTING_REUSE,
                .rejectionReason = domain::AdmissionRejectionReason::DUPLICATE_COMMAND_CONFLICT,
                .originalResult = {},
            };
        }
        if (existing->second.state != RecordState::COMPLETED) {
            ++statistics_.identicalInFlight;
            return AdmissionDecision{
                .status = AdmissionStatus::IDENTICAL_IN_FLIGHT,
                .rejectionReason = std::nullopt,
                .originalResult = {},
            };
        }

        ++statistics_.identicalCompleted;
        return AdmissionDecision{
            .status = AdmissionStatus::IDENTICAL_COMPLETED,
            .rejectionReason = std::nullopt,
            .originalResult = existing->second.result,
        };
    }

    if (records_.size() >= capacity_) {
        ++statistics_.admissionUnavailable;
        return AdmissionDecision{
            .status = AdmissionStatus::ADMISSION_UNAVAILABLE,
            .rejectionReason = std::nullopt,
            .originalResult = {},
        };
    }

    records_.emplace(key, Record{
                              .command = businessCommand,
                              .state = RecordState::RESERVED,
                              .commandSequence = {},
                              .result = {},
                          });
    ++statistics_.firstSubmissions;
    return AdmissionDecision{
        .status = AdmissionStatus::FIRST_SUBMISSION,
        .rejectionReason = std::nullopt,
        .originalResult = {},
    };
}

MarkSequencedStatus CommandAdmissionIndex::markSequenced(const sequencer::sequenceMessage& command) {
    if (command.globalSequenceNumber.value() == 0) {
        return MarkSequencedStatus::INVALID_SEQUENCE;
    }
    const AdmissionKey key = keyFrom(command);
    const NormalizedBusinessCommand businessCommand = businessCommandFrom(command);

    std::lock_guard lock(mutex_);
    const auto existing = records_.find(key);
    if (existing == records_.end()) {
        return MarkSequencedStatus::UNKNOWN_RESERVATION;
    }
    if (existing->second.command != businessCommand) {
        return MarkSequencedStatus::COMMAND_MISMATCH;
    }
    if (existing->second.state == RecordState::RESERVED) {
        existing->second.state = RecordState::SEQUENCED;
        existing->second.commandSequence = command.globalSequenceNumber;
        return MarkSequencedStatus::SEQUENCED;
    }
    if (existing->second.state == RecordState::SEQUENCED) {
        if (existing->second.commandSequence != command.globalSequenceNumber) {
            return MarkSequencedStatus::SEQUENCE_MISMATCH;
        }
        return MarkSequencedStatus::IDEMPOTENT;
    }
    return MarkSequencedStatus::WRONG_STATE;
}

CompletionStatus CommandAdmissionIndex::complete(matching_engine::ImmutableCommandResultBatch result) {
    if (result == nullptr) {
        return CompletionStatus::INVALID_BATCH;
    }
    const domain::CommandResultCorrelation& correlation = result->correlation();
    if (correlation.clientId.value() == 0 || correlation.commandSequence.value() == 0 ||
        correlation.clientCommandId.value().empty() || result->commandSequence() != correlation.commandSequence) {
        return CompletionStatus::INVALID_BATCH;
    }
    const AdmissionKey key = keyFrom(correlation);

    std::lock_guard lock(mutex_);
    const auto existing = records_.find(key);
    if (existing == records_.end()) {
        return CompletionStatus::UNKNOWN_RESERVATION;
    }
    if (existing->second.state != RecordState::SEQUENCED) {
        return CompletionStatus::WRONG_STATE;
    }
    if (existing->second.commandSequence != correlation.commandSequence) {
        return CompletionStatus::CORRELATION_MISMATCH;
    }
    existing->second.state = RecordState::COMPLETED;
    existing->second.result = std::move(result);
    return CompletionStatus::COMPLETED;
}

matching_engine::ImmutableCommandResultBatch CommandAdmissionIndex::completedResult(
    const sequencer::sequenceMessage& command) const {
    const AdmissionKey key = keyFrom(command);
    const NormalizedBusinessCommand businessCommand = businessCommandFrom(command);

    std::lock_guard lock(mutex_);
    const auto existing = records_.find(key);
    if (existing == records_.end() || existing->second.state != RecordState::COMPLETED ||
        existing->second.command != businessCommand) {
        return {};
    }
    return existing->second.result;
}

bool CommandAdmissionIndex::abandonReservation(const sequencer::sequenceMessage& command) {
    const AdmissionKey key = keyFrom(command);
    const NormalizedBusinessCommand businessCommand = businessCommandFrom(command);

    std::lock_guard lock(mutex_);
    const auto existing = records_.find(key);
    if (existing == records_.end() || existing->second.state != RecordState::RESERVED ||
        existing->second.command != businessCommand) {
        return false;
    }
    records_.erase(existing);
    return true;
}

std::size_t CommandAdmissionIndex::size() const {
    std::lock_guard lock(mutex_);
    return records_.size();
}

std::size_t CommandAdmissionIndex::capacity() const noexcept {
    return capacity_;
}

AdmissionStatistics CommandAdmissionIndex::statistics() const {
    std::lock_guard lock(mutex_);
    return statistics_;
}

bool CommandAdmissionIndex::AdmissionKeyLess::operator()(const AdmissionKey& left,
                                                         const AdmissionKey& right) const noexcept {
    if (left.clientId != right.clientId) {
        return left.clientId < right.clientId;
    }
    return left.clientCommandId.value() < right.clientCommandId.value();
}

AdmissionKey CommandAdmissionIndex::keyFrom(const sequencer::sequenceMessage& command) {
    if (command.clientId.value() == 0 || !command.clientCommandId.has_value()) {
        throw std::invalid_argument("admission command is missing exact client identity");
    }
    return AdmissionKey{
        .clientId = command.clientId,
        .clientCommandId = *command.clientCommandId,
    };
}

AdmissionKey CommandAdmissionIndex::keyFrom(const domain::CommandResultCorrelation& correlation) {
    return AdmissionKey{
        .clientId = correlation.clientId,
        .clientCommandId = correlation.clientCommandId,
    };
}

NormalizedBusinessCommand CommandAdmissionIndex::businessCommandFrom(const sequencer::sequenceMessage& command) {
    if (command.instrumentId.value() == 0) {
        throw std::invalid_argument("admission command is missing InstrumentId");
    }

    if (command.type == sequencer::orderType::BUY || command.type == sequencer::orderType::SELL) {
        if (command.configurationVersion == 0 || command.price.value() == 0 || command.quantity.value() == 0) {
            throw std::invalid_argument("admission NewOrder is missing normalized business fields");
        }
        return NormalizedNewOrder{
            .instrumentId = command.instrumentId,
            .configurationVersion = command.configurationVersion,
            .side = toDomainSide(command.type),
            .price = command.price,
            .quantity = command.quantity,
            .timeInForce = command.tif,
        };
    }

    if (command.type == sequencer::orderType::CANCEL) {
        if (!command.targetOrderId.has_value() || command.targetOrderId->value() == 0) {
            throw std::invalid_argument("admission Cancel is missing TargetOrderId");
        }
        return NormalizedCancel{
            .instrumentId = command.instrumentId,
            .targetOrderId = *command.targetOrderId,
        };
    }

    throw std::invalid_argument("unsupported command type reached admission");
}

} // namespace exchange::core::admission
