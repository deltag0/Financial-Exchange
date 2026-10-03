#pragma once

#include "start_new_run.hpp"

namespace exchange::storage::detail {
struct NewRunJournalPreparationHooks;
}

namespace exchange::core::detail {

struct StartNewRunHooks final {
    void *context{nullptr};
    std::optional<StartNewRunOutcome> (*constructState)(
        void *context, domain::ExchangeRunId exchangeRunId, std::uint64_t maxRunCommands,
        std::unique_ptr<matching_engine::MatchingState> &matchingState,
        std::unique_ptr<admission::CommandAdmissionIndex> &admissionIndex) noexcept {nullptr};
    storage::NewRunActivationResult (*activate)(void *context, const std::filesystem::path &canonicalCatalogPath,
                                                const storage::PreparedNewRunJournalV1 &prepared) noexcept {nullptr};
    // Reuses storage fault injection; any installed writer hook context must outlive the writer.
    const storage::detail::NewRunJournalPreparationHooks *preparationHooks{nullptr};
};

[[nodiscard]] std::optional<StartNewRunOutcome> constructEmptyRunStateV1(
    domain::ExchangeRunId exchangeRunId, std::uint64_t maxRunCommands,
    std::unique_ptr<matching_engine::MatchingState> &matchingState,
    std::unique_ptr<admission::CommandAdmissionIndex> &admissionIndex) noexcept;

[[nodiscard]] StartNewRunResult startNewRunV1WithHooks(const std::filesystem::path &canonicalCatalogPath,
                                                       const storage::NewRunConfigurationV1 &configuration,
                                                       const StartNewRunHooks &hooks, RunStateV1 &output) noexcept;

} // namespace exchange::core::detail
