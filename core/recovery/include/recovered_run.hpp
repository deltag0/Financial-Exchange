#pragma once

#include "command_admission_recovery.hpp"
#include "run_state.hpp"
#include "run_journal_recovery.hpp"
#include "run_journal_replay.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>

namespace exchange::core::recovery {

enum class RecoveredRunOutcome : std::uint8_t {
    RECOVERED,
    PREPARATION_FAILED,
    REPLAY_FAILED,
    ADMISSION_FAILED,
};

struct RecoveredRunResult final {
    RecoveredRunOutcome outcome{RecoveredRunOutcome::PREPARATION_FAILED};
    storage::RunJournalRecoveryResult preparation{};
    matching_engine::RunJournalReplayResult replay{};
    admission::CommandAdmissionRecoveryResult admission{};
};

// Success replaces output. Every failure leaves the caller's existing recovered run unchanged.
[[nodiscard]] RecoveredRunResult recoverRunV1(const std::filesystem::path &canonicalJournalPath, RunStateV1 &output,
                                              std::optional<domain::ExchangeRunId> expectedRunId = {}) noexcept;

} // namespace exchange::core::recovery
