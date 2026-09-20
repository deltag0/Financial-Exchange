#include "start_new_run.hpp"

#include "start_new_run_internal.hpp"
#include "new_run_journal_internal.hpp"

#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace exchange::core {
namespace {

std::optional<StartNewRunOutcome> constructRunState(
    void *, const domain::ExchangeRunId exchangeRunId, const std::uint64_t maxRunCommands,
    std::unique_ptr<matching_engine::MatchingState> &matchingState,
    std::unique_ptr<admission::CommandAdmissionIndex> &admissionIndex) noexcept {
    return detail::constructEmptyRunStateV1(exchangeRunId, maxRunCommands, matchingState, admissionIndex);
}

storage::NewRunActivationResult activateNewRun(void *, const std::filesystem::path &canonicalCatalogPath,
                                               const storage::PreparedNewRunJournalV1 &prepared) noexcept {
    return storage::activatePreparedNewRunV1(canonicalCatalogPath, prepared);
}

} // namespace

namespace detail {

std::optional<StartNewRunOutcome> constructEmptyRunStateV1(
    const domain::ExchangeRunId exchangeRunId, const std::uint64_t maxRunCommands,
    std::unique_ptr<matching_engine::MatchingState> &matchingState,
    std::unique_ptr<admission::CommandAdmissionIndex> &admissionIndex) noexcept {
    if (maxRunCommands > std::numeric_limits<std::size_t>::max()) {
        return StartNewRunOutcome::CAPACITY_UNREPRESENTABLE;
    }

    try {
        auto constructedMatchingState = std::make_unique<matching_engine::MatchingState>(exchangeRunId);
        auto constructedAdmissionIndex =
            std::make_unique<admission::CommandAdmissionIndex>(static_cast<std::size_t>(maxRunCommands), exchangeRunId);
        matchingState = std::move(constructedMatchingState);
        admissionIndex = std::move(constructedAdmissionIndex);
        return std::nullopt;
    } catch (const std::bad_alloc &) {
        return StartNewRunOutcome::ALLOCATION_FAILURE;
    } catch (...) {
        return StartNewRunOutcome::STATE_CONSTRUCTION_FAILED;
    }
}

StartNewRunResult startNewRunV1WithHooks(const std::filesystem::path &canonicalCatalogPath,
                                         const storage::NewRunConfigurationV1 &configuration,
                                         const StartNewRunHooks &hooks, RunStateV1 &output) noexcept {
    storage::PreparedNewRunJournalV1 prepared;
    storage::NewRunJournalPreparationResult preparation =
        hooks.preparationHooks == nullptr
            ? storage::prepareNewRunJournalV1(canonicalCatalogPath, configuration, prepared)
            : storage::detail::prepareNewRunJournalV1WithHooks(canonicalCatalogPath, configuration,
                                                               *hooks.preparationHooks, prepared);
    if (preparation.outcome != storage::NewRunJournalPreparationOutcome::PREPARED) {
        return {
            .outcome = StartNewRunOutcome::PREPARATION_FAILED,
            .preparation = std::move(preparation),
        };
    }

    std::unique_ptr<matching_engine::MatchingState> matchingState;
    std::unique_ptr<admission::CommandAdmissionIndex> admissionIndex;
    const std::optional<StartNewRunOutcome> construction = hooks.constructState(
        hooks.context, prepared.header.exchangeRunId, prepared.header.maxRunCommands, matchingState, admissionIndex);
    if (construction.has_value() || matchingState == nullptr || admissionIndex == nullptr) {
        return {
            .outcome = construction.value_or(StartNewRunOutcome::STATE_CONSTRUCTION_FAILED),
            .preparation = std::move(preparation),
        };
    }

    storage::NewRunActivationResult activation = hooks.activate(hooks.context, canonicalCatalogPath, prepared);
    if (activation.outcome != storage::NewRunActivationOutcome::ACTIVATED) {
        return {
            .outcome = StartNewRunOutcome::ACTIVATION_FAILED,
            .preparation = std::move(preparation),
            .activation = std::move(activation),
        };
    }

    RunStateV1 started{
        .journalWriter = std::move(prepared.writer),
        .matchingState = std::move(matchingState),
        .admissionIndex = std::move(admissionIndex),
    };
    output = std::move(started);
    return {
        .outcome = StartNewRunOutcome::STARTED,
        .preparation = std::move(preparation),
        .activation = std::move(activation),
    };
}

} // namespace detail

StartNewRunResult startNewRunV1(const std::filesystem::path &canonicalCatalogPath,
                                const storage::NewRunConfigurationV1 &configuration, RunStateV1 &output) noexcept {
    static constexpr detail::StartNewRunHooks HOOKS{
        nullptr,
        constructRunState,
        activateNewRun,
    };
    return detail::startNewRunV1WithHooks(canonicalCatalogPath, configuration, HOOKS, output);
}

} // namespace exchange::core
