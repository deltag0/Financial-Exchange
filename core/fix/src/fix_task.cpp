#include "../include/fix_task.hpp"
#include "fix_parser.hpp"
#include <iostream>

namespace exchange::core::task {
namespace {

const char *admissionStatusName(const admission::AdmissionStatus status) {
    switch (status) {
        case admission::AdmissionStatus::FIRST_SUBMISSION:
            return "FirstSubmission";
        case admission::AdmissionStatus::IDENTICAL_IN_FLIGHT:
            return "IdenticalInFlight";
        case admission::AdmissionStatus::IDENTICAL_COMPLETED:
            return "IdenticalCompleted";
        case admission::AdmissionStatus::CONFLICTING_REUSE:
            return "DuplicateCommandConflict";
        case admission::AdmissionStatus::ADMISSION_UNAVAILABLE:
            return "AdmissionUnavailable";
    }
    return "UnknownAdmissionStatus";
}

} // namespace

void FixTask::onCreate(const FIX::SessionID &sessionID) {
    std::cout << "[FixTask] Session created: " << sessionID << std::endl;
}

void FixTask::onLogon(const FIX::SessionID &sessionID) {
    std::cout << "[FixTask] Logon received: " << sessionID << std::endl;
}

void FixTask::onLogout(const FIX::SessionID &sessionID) {
    std::cout << "[FixTask] Logout received: " << sessionID << std::endl;
}

void FixTask::toAdmin(FIX::Message &, const FIX::SessionID &) {}

void FixTask::toApp(FIX::Message &message, const FIX::SessionID &) noexcept {
    std::cout << "[FixTask] -> Sending App Data: " << message.toString() << std::endl;
}

void FixTask::fromAdmin(const FIX::Message &message, const FIX::SessionID &) noexcept {
    std::cout << "[FixTask] <- Received Admin (Raw): " << message.toString().substr(0, 30) << "..."
              << std::endl;
}

void FixTask::fromApp(const FIX::Message &message, const FIX::SessionID &sessionID) noexcept {

    try {
        FIX::MsgType msgType;
        message.getHeader().getField(msgType);

        if (!fix::isProcessableMessageType(msgType.getValue())) {
            return;
        }

        sequencer::sequenceMessage normalized =
            fix::parseFixMessage(message, sessionID, clientIdentityResolver, mq_shards.size());
        const admission::AdmissionDecision decision = commandAdmissionIndex.reserve(normalized);
        if (decision.status != admission::AdmissionStatus::FIRST_SUBMISSION) {
            std::cerr << "[FixTask] Command admission outcome: " << admissionStatusName(decision.status) << std::endl;
            return;
        }

        if (!internalQueues.fixMessageQueue->push(normalized)) {
            const bool abandoned = commandAdmissionIndex.abandonReservation(normalized);
            std::cerr << "[FixTask] Normalized command queue is full; reservation "
                      << (abandoned ? "abandoned" : "could not be abandoned") << std::endl;
        }

    } catch (const FIX::FieldNotFound &e) {
        std::cerr << "[FixTask] Required field missing: " << e.field << std::endl;
    } catch (const std::exception &e) {
        std::cerr << "[FixTask] Error: " << e.what() << std::endl;
    }
}

void FixTask::sendFixMessage(FIX::Message &message, const FIX::SessionID &sessionID) {
    FIX::Session::sendToTarget(message, sessionID);
}

bool FixTask::trySendSequencerMessage(const sequencer::sequenceMessage &message) {
    return mq_shards[message.shard_id]->push(message);
}

bool FixTask::processNextNormalizedCommand() {
    if (!pendingNormalizedCommand.has_value()) {
        sequencer::sequenceMessage message{};
        if (!internalQueues.fixMessageQueue->pop(message)) {
            return false;
        }
        pendingNormalizedCommand = message;
    }

    if (!trySendSequencerMessage(*pendingNormalizedCommand)) {
        return false;
    }
    pendingNormalizedCommand.reset();
    return true;
}

void FixTask::send(sequencer::sequenceMessage &) {}

void FixTask::run() {
    while (true) {
        for (int i = 0; i < EXT_BURST_MESSAGES; ++i) {
            if (!processNextNormalizedCommand()) {
                break;
            }
        }

        for (int i = 0; i < INT_BURST_MESSAGES; ++i) {
            bool check = false;
            sequencer::sequenceMessage internalMsg{};
            check = multicastBus.read(cursor, internalMsg);

            if (!check) break;

            // ! Just for now print it out
            std::cout << "[FixTask] -> Multicast Internal Message ID: " << internalMsg.id
                      << " shard: " << static_cast<int>(internalMsg.shard_id)
                      << " symbol: " << internalMsg.symbol << std::endl;
        }
    }
}

} // namespace exchange::core::task
