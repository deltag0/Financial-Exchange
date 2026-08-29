#pragma once

#include "command_admission.hpp"
#include "../../../matching_engine/include/command_result_queue.hpp"

#include <optional>

namespace exchange::core::admission {

class AdmissionCompletionConsumer final {
public:
    AdmissionCompletionConsumer(matching_engine::BoundedCommandResultQueue& resultQueue,
                                CommandAdmissionIndex& admissionIndex)
        : resultQueue_(resultQueue), admissionIndex_(admissionIndex) {}

    [[nodiscard]] bool processNext();
    [[noreturn]] void run();

    [[nodiscard]] bool hasPendingBatch() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] const matching_engine::ImmutableCommandResultBatch& pendingBatch() const noexcept {
        return pendingBatch_;
    }

private:
    matching_engine::BoundedCommandResultQueue& resultQueue_;
    CommandAdmissionIndex& admissionIndex_;
    matching_engine::ImmutableCommandResultBatch pendingBatch_;
    std::optional<CompletionStatus> failure_;
};

} // namespace exchange::core::admission
