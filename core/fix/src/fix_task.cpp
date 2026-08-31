#include "../include/fix_task.hpp"
#include "../../task/include/adaptive_idle.hpp"
#include "fix_parser.hpp"
#include <iostream>

namespace exchange::core::task {

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

void FixTask::toApp(FIX::Message &, const FIX::SessionID &) noexcept {}

void FixTask::fromAdmin(const FIX::Message &, const FIX::SessionID &) noexcept {}

void FixTask::fromApp(const FIX::Message &message, const FIX::SessionID &sessionID) noexcept {
    std::optional<sequencer::sequenceMessage> normalized;
    try {
        FIX::MsgType msgType;
        message.getHeader().getField(msgType);

        if (!fix::isProcessableMessageType(msgType.getValue())) {
            return;
        }

        normalized.emplace(fix::parseFixMessage(message, sessionID, clientIdentityResolver));
    } catch (const FIX::FieldNotFound &) {
        normalizationRejections_.fetch_add(1, std::memory_order_relaxed);
        return;
    } catch (const fix::FixValidationError &) {
        normalizationRejections_.fetch_add(1, std::memory_order_relaxed);
        return;
    } catch (const std::exception &exception) {
        internalFailures_.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[FixTask] Internal normalization failure: " << exception.what() << '\n';
        return;
    } catch (...) {
        internalFailures_.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[FixTask] Internal normalization failure: unknown exception\n";
        return;
    }

    try {
        const admission::AdmissionDecision decision = commandAdmissionIndex.reserve(*normalized);
        if (decision.status != admission::AdmissionStatus::FIRST_SUBMISSION) {
            return;
        }

        if (!stagingQueue_.push(*normalized)) {
            stagingQueueSaturations_.fetch_add(1, std::memory_order_relaxed);
            const bool abandoned = commandAdmissionIndex.abandonReservation(*normalized);
            if (!abandoned) {
                reservationAbandonFailures_.fetch_add(1, std::memory_order_relaxed);
                std::cerr << "[FixTask] Invariant failure: staging-full reservation could not be abandoned\n";
            }
        }
    } catch (const std::exception &exception) {
        internalFailures_.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[FixTask] Internal admission/staging failure: " << exception.what() << '\n';
    } catch (...) {
        internalFailures_.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[FixTask] Internal admission/staging failure: unknown exception\n";
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
