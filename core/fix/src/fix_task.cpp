#include "../include/fix_task.hpp"
#include "../../task/include/adaptive_idle.hpp"
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
    std::cout << "[FixTask] <- Received Admin (Raw): " << message.toString().substr(0, 30) << "..." << std::endl;
}

void FixTask::fromApp(const FIX::Message &message, const FIX::SessionID &sessionID) noexcept {
    try {
        FIX::MsgType msgType;
        message.getHeader().getField(msgType);

        if (!fix::isProcessableMessageType(msgType.getValue())) {
            return;
        }

        sequencer::sequenceMessage normalized = fix::parseFixMessage(message, sessionID, clientIdentityResolver);
        const admission::AdmissionDecision decision = commandAdmissionIndex.reserve(normalized);
        if (decision.status != admission::AdmissionStatus::FIRST_SUBMISSION) {
            std::cerr << "[FixTask] Command admission outcome: " << admissionStatusName(decision.status) << std::endl;
            return;
        }

        if (!stagingQueue_.push(normalized)) {
            const bool abandoned = commandAdmissionIndex.abandonReservation(normalized);
            std::cerr << "[FixTask] Staging queue is full; reservation "
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

bool FixTask::processNextStagedCommand() {
    if (!pendingStagedCommand_.has_value()) {
        sequencer::sequenceMessage command{};
        if (!stagingQueue_.pop(command)) {
            return false;
        }
        pendingStagedCommand_ = command;
    }

    if (!sequencingIngressQueue_.push(*pendingStagedCommand_)) {
        return false;
    }
    pendingStagedCommand_.reset();
    return true;
}

void FixTask::send(sequencer::sequenceMessage &) {}

void FixTask::run() {
    AdaptiveIdle idle;
    while (true) {
        bool progressed = false;
        for (int i = 0; i < EXT_BURST_MESSAGES; ++i) {
            if (!processNextStagedCommand()) {
                break;
            }
            progressed = true;
        }

        for (int i = 0; i < INT_BURST_MESSAGES; ++i) {
            sequencer::sequenceMessage internalMsg{};
            if (!multicastBus.read(cursor, internalMsg)) {
                break;
            }
            progressed = true;
        }

        if (progressed) {
            idle.reset();
        } else {
            idle.wait();
        }
    }
}

} // namespace exchange::core::task
