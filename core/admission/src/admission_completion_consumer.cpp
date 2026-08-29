#include "admission_completion_consumer.hpp"

#include <stdexcept>
#include <string>

namespace exchange::core::admission {
namespace {

const char* completionStatusName(const CompletionStatus status) {
    switch (status) {
        case CompletionStatus::COMPLETED:
            return "Completed";
        case CompletionStatus::INVALID_BATCH:
            return "InvalidBatch";
        case CompletionStatus::UNKNOWN_RESERVATION:
            return "UnknownReservation";
        case CompletionStatus::WRONG_STATE:
            return "WrongState";
        case CompletionStatus::CORRELATION_MISMATCH:
            return "CorrelationMismatch";
    }
    return "UnknownCompletionStatus";
}

} // namespace

bool AdmissionCompletionConsumer::processNext() {
    if (failure_.has_value()) {
        throw std::logic_error("admission completion consumer is fail-stopped after " +
                               std::string{completionStatusName(*failure_)});
    }
    if (pendingBatch_ == nullptr && !resultQueue_.tryPop(pendingBatch_)) {
        return false;
    }

    const CompletionStatus status = admissionIndex_.complete(pendingBatch_);
    if (status != CompletionStatus::COMPLETED) {
        failure_ = status;
        throw std::logic_error("admission completion invariant failure: " + std::string{completionStatusName(status)});
    }

    pendingBatch_.reset();
    return true;
}

[[noreturn]] void AdmissionCompletionConsumer::run() {
    while (true) {
        static_cast<void>(processNext());
    }
}

bool AdmissionCompletionConsumer::hasPendingBatch() const noexcept {
    return pendingBatch_ != nullptr;
}

bool AdmissionCompletionConsumer::failed() const noexcept {
    return failure_.has_value();
}

} // namespace exchange::core::admission
