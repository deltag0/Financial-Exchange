#include "../include/sequencer.hpp"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

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
    while (true) {
        processNext();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool exchange::sequencer::Sequencer::processNext() {
    if (pendingSequencedCommand.has_value()) {
        return handoffPendingCommand();
    }

    // A Sequencer instance currently owns one shard queue.
    if (mq_shards.empty() || mq_shards[0]->empty()) {
        return false;
    }

    sequenceMessage message{};
    if (!mq_shards[0]->pop(message)) {
        return false;
    }

    std::cout << "[Sequencer] Processing message ID: " << message.id << " on shard: " << (int)message.shard_id
              << " (Ticker: " << message.symbol << ")" << std::endl;

    message.globalSequenceNumber = getNextGlobalSequenceNumber(message);
    if (message.type == orderType::BUY || message.type == orderType::SELL) {
        message.orderId = domain::orderIdFrom(message.globalSequenceNumber);
    }
    message.topicSequenceNumber = getNextTopicSequenceNumber(message);

    if (admissionIndex != nullptr) {
        const core::admission::MarkSequencedStatus status = admissionIndex->markSequenced(message);
        if (status != core::admission::MarkSequencedStatus::SEQUENCED) {
            throw std::logic_error("sequencer admission binding failed: " +
                                   std::string{markSequencedStatusName(status)});
        }
    }

    pendingSequencedCommand = message;
    return handoffPendingCommand();
}

bool exchange::sequencer::Sequencer::handoffPendingCommand() {
    if (!pendingSequencedCommand.has_value()) {
        return true;
    }
    if (matchingEngineQueue == nullptr) {
        throw std::logic_error("sequencer has no matching-engine queue");
    }
    if (!matchingEngineQueue->push(*pendingSequencedCommand)) {
        return false;
    }
    pendingSequencedCommand.reset();
    return true;
}

exchange::domain::CommandSequence exchange::sequencer::Sequencer::getNextGlobalSequenceNumber(
    const sequenceMessage& message) {
    return domain::CommandSequence{++globalSequenceNumber};
}

uint64_t exchange::sequencer::Sequencer::getNextTopicSequenceNumber(const sequenceMessage& message) {
    topicData& data = topicSequence[message.port];
    data.lastSenderPort = message.port;
    return ++data.sequenceNumber;
}

void exchange::sequencer::Sequencer::send(sequenceMessage&) {}
