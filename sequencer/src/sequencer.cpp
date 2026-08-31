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

void exchange::sequencer::Sequencer::run() {
    std::cout << "[Sequencer] Thread started" << std::endl;
    core::task::AdaptiveIdle idle;
    while (true) {
        if (drainAvailable()) {
            idle.reset();
        } else {
            idle.wait();
        }
    }
}

bool exchange::sequencer::Sequencer::drainAvailable() {
    bool progressed = false;
    while (processNext()) {
        progressed = true;
    }
    return progressed;
}

bool exchange::sequencer::Sequencer::processNext() {
    if (pendingSequencedCommand_.has_value()) {
        return handoffPendingCommand();
    }

    if (sequencingIngressQueue_.empty()) {
        return false;
    }
    if (lastAssignedSequence_.value() == std::numeric_limits<domain::CommandSequence::Underlying>::max()) {
        throw std::overflow_error("CommandSequence exhausted");
    }

    sequenceMessage message{};
    if (!sequencingIngressQueue_.pop(message)) {
        return false;
    }

    message.globalSequenceNumber = nextCommandSequence();
    if (message.type == orderType::BUY || message.type == orderType::SELL) {
        message.orderId = domain::orderIdFrom(message.globalSequenceNumber);
    }
    if (admissionIndex_ != nullptr) {
        const core::admission::MarkSequencedStatus status = admissionIndex_->markSequenced(message);
        if (status != core::admission::MarkSequencedStatus::SEQUENCED) {
            throw std::logic_error("sequencer admission binding failed: " +
                                   std::string{markSequencedStatusName(status)});
        }
    }

    pendingSequencedCommand_ = message;
    return handoffPendingCommand();
}

bool exchange::sequencer::Sequencer::handoffPendingCommand() {
    if (!pendingSequencedCommand_.has_value()) {
        return true;
    }
    if (!matchingEngineQueue_.push(*pendingSequencedCommand_)) {
        return false;
    }
    pendingSequencedCommand_.reset();
    return true;
}

exchange::domain::CommandSequence exchange::sequencer::Sequencer::nextCommandSequence() {
    lastAssignedSequence_ = domain::CommandSequence{lastAssignedSequence_.value() + 1};
    return lastAssignedSequence_;
}

void exchange::sequencer::Sequencer::send(sequenceMessage&) {}
