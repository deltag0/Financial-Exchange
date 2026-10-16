#pragma once

#include "new_run_activation.hpp"
#include "new_run_journal.hpp"
#include "run_state.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>

namespace exchange::core {

enum class StartNewRunOutcome : std::uint8_t {
    STARTED,
    PREPARATION_FAILED,
    CAPACITY_UNREPRESENTABLE,
    ALLOCATION_FAILURE,
    STATE_CONSTRUCTION_FAILED,
    ACTIVATION_FAILED,
};

struct StartNewRunResult final {
    StartNewRunOutcome outcome{StartNewRunOutcome::PREPARATION_FAILED};
    storage::NewRunJournalPreparationResult preparation{};
    std::optional<storage::NewRunActivationResult> activation{};
};

// Success transfers all three run owners after activation is durably committed. Failure preserves output.
[[nodiscard]] StartNewRunResult startNewRunV1(const std::filesystem::path &canonicalCatalogPath,
                                              const storage::NewRunConfigurationV1 &configuration,
                                              RunStateV1 &output) noexcept;

} // namespace exchange::core
