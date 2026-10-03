#pragma once

#include "command_admission.hpp"

#include <cstdint>

namespace exchange::core::admission {

enum class CommandAdmissionRecoveryOutcome : std::uint8_t {
    RECONSTRUCTED,
    INVALID_INPUT,
    CAPACITY_ERROR,
    INVARIANT_FAILURE,
    INTERNAL_FAILURE,
};

enum class CommandAdmissionRecoveryError : std::uint8_t {
    NONE,
    INVALID_RUN_ID,
    COMMAND_RESULT_COUNT_MISMATCH,
    COMMAND_COUNT_CAPACITY_EXCEEDED,
    CAPACITY_UNREPRESENTABLE,
    COMMAND_SEQUENCE_MISMATCH,
    COMMAND_CONTEXT_MISMATCH,
    RESULT_CORRELATION_MISMATCH,
    RESULT_EVENT_IDENTITY_MISMATCH,
    INVALID_NORMALIZED_COMMAND,
    DUPLICATE_COMMAND_KEY,
    ALLOCATION_FAILURE,
};

struct CommandAdmissionRecoveryResult final {
    CommandAdmissionRecoveryOutcome outcome{CommandAdmissionRecoveryOutcome::INTERNAL_FAILURE};
    CommandAdmissionRecoveryError error{CommandAdmissionRecoveryError::NONE};
};

// Success replaces output. Every failure leaves the caller's existing index unchanged.
[[nodiscard]] CommandAdmissionRecoveryResult reconstructCommandAdmissionIndex(
    const storage::LoadedRunJournalV1 &journal,
    const std::vector<matching_engine::ImmutableCommandResultBatch> &commandResults,
    std::unique_ptr<CommandAdmissionIndex> &output) noexcept;

} // namespace exchange::core::admission
