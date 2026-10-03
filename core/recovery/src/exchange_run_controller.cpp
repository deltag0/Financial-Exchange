#include "exchange_run_controller.hpp"

#include "new_run_journal_internal.hpp"
#include "run_catalog_storage_internal.hpp"
#include "start_new_run_internal.hpp"

#include <limits>
#include <utility>

namespace exchange::core {
namespace detail {

NewRunStartupResult startupNewRunV1WithHooks(const std::filesystem::path& canonicalCatalogPath,
                                             const storage::NewRunConfigurationV1& configuration,
                                             const StartNewRunHooks* hooks,
                                             ExchangeRunController& controller) noexcept {
    if (controller.run_.journalWriter != nullptr) {
        return {.outcome = NewRunStartupOutcome::RUN_ALREADY_OWNED};
    }

    std::filesystem::path catalogBinding;
    try {
        catalogBinding = std::filesystem::absolute(canonicalCatalogPath);
    } catch (...) {
        return {.outcome = NewRunStartupOutcome::CATALOG_PATH_FAILED};
    }

    storage::RunCatalogSnapshotV1 catalog;
    const auto load = storage::loadRunCatalogV1(catalogBinding, catalog);
    if (load.outcome == storage::RunCatalogLoadOutcome::INVALID ||
        load.outcome == storage::RunCatalogLoadOutcome::IO_FAILURE) {
        return {.outcome = NewRunStartupOutcome::START_FAILED, .catalogLoad = load};
    }
    if (load.outcome == storage::RunCatalogLoadOutcome::LOADED && catalog.activeRunId.has_value()) {
        return {.outcome = NewRunStartupOutcome::ACTIVE_RUN_EXISTS, .catalogLoad = load};
    }

    // Preparation owns missing-catalog journal-evidence policy.
    RunStateV1 started;
    auto startup = hooks == nullptr ? startNewRunV1(catalogBinding, configuration, started)
                                    : startNewRunV1WithHooks(catalogBinding, configuration, *hooks, started);
    if (startup.outcome != StartNewRunOutcome::STARTED) {
        return {.outcome = NewRunStartupOutcome::START_FAILED, .catalogLoad = load, .startup = std::move(startup)};
    }

    controller.run_ = std::move(started);
    controller.canonicalCatalogPath_ = std::move(catalogBinding);
    controller.openAdmission();
    controller.state_ = ExchangeRunStartupState::READY;
    return {.outcome = NewRunStartupOutcome::READY, .catalogLoad = load, .startup = std::move(startup)};
}

ExistingRunStartupResult startupExistingRunV1WithHooks(const std::filesystem::path& canonicalCatalogPath,
                                                       const storage::detail::RunCatalogStorageHooks& hooks,
                                                       ExchangeRunController& controller) noexcept {
    if (controller.run_.journalWriter != nullptr) {
        return {.outcome = ExistingRunStartupOutcome::EXPLICIT_OPERATION_REQUIRED};
    }
    controller.state_ = ExchangeRunStartupState::UNAVAILABLE;
    controller.run_ = {};

    std::filesystem::path catalogBinding;
    try {
        catalogBinding = std::filesystem::absolute(canonicalCatalogPath);
    } catch (...) {
        return {.outcome = ExistingRunStartupOutcome::CATALOG_PATH_FAILED};
    }

    storage::RunCatalogSnapshotV1 catalog;
    const auto load = storage::loadRunCatalogV1(catalogBinding, catalog);
    if (load.outcome != storage::RunCatalogLoadOutcome::LOADED) {
        return {.outcome = ExistingRunStartupOutcome::CATALOG_LOAD_FAILED, .catalogLoad = load};
    }
    if (!catalog.activeRunId.has_value()) {
        return {.outcome = ExistingRunStartupOutcome::NO_ACTIVE_RUN, .catalogLoad = load};
    }
    if (catalog.activeDisposition != storage::RunCatalogDisposition::OPEN &&
        catalog.activeDisposition != storage::RunCatalogDisposition::PAUSED) {
        return {.outcome = ExistingRunStartupOutcome::EXPLICIT_OPERATION_REQUIRED, .catalogLoad = load};
    }
    if (catalog.generation == std::numeric_limits<std::uint64_t>::max()) {
        return {.outcome = ExistingRunStartupOutcome::GENERATION_EXHAUSTED, .catalogLoad = load};
    }

    std::filesystem::path journalPath;
    if (!storage::detail::deriveCanonicalRunJournalPathV1(catalogBinding, *catalog.activeRunId, journalPath)) {
        return {.outcome = ExistingRunStartupOutcome::JOURNAL_PATH_FAILED, .catalogLoad = load};
    }

    RunStateV1 recovered;
    auto recovery = recovery::recoverRunV1(journalPath, recovered, catalog.activeRunId);
    if (recovery.outcome != recovery::RecoveredRunOutcome::RECOVERED) {
        controller.state_ = ExchangeRunStartupState::RECOVERY_FAILED;
        return {
            .outcome = ExistingRunStartupOutcome::RECOVERY_FAILED,
            .catalogLoad = load,
            .recovery = std::move(recovery),
        };
    }

    ++catalog.generation;
    catalog.activeDisposition = storage::RunCatalogDisposition::PAUSED;
    auto replacement = storage::detail::replaceRunCatalogV1WithHooks(catalogBinding, catalog, hooks);
    if (replacement.outcome != storage::RunCatalogReplaceOutcome::COMMITTED) {
        return {
            .outcome = ExistingRunStartupOutcome::CATALOG_REPLACE_FAILED,
            .catalogLoad = load,
            .recovery = std::move(recovery),
            .catalogReplacement = std::move(replacement),
        };
    }

    controller.run_ = std::move(recovered);
    controller.canonicalCatalogPath_ = std::move(catalogBinding);
    controller.state_ = ExchangeRunStartupState::PAUSED;
    return {
        .outcome = ExistingRunStartupOutcome::PAUSED,
        .catalogLoad = load,
        .recovery = std::move(recovery),
        .catalogReplacement = std::move(replacement),
    };
}

RunResumeResult resumeRunV1WithHooks(const storage::detail::RunCatalogStorageHooks& hooks,
                                     ExchangeRunController& controller) noexcept {
    if (controller.state_ != ExchangeRunStartupState::PAUSED) {
        return {.outcome = RunResumeOutcome::NOT_PAUSED};
    }

    storage::RunCatalogSnapshotV1 catalog;
    const auto load = storage::loadRunCatalogV1(controller.canonicalCatalogPath_, catalog);
    if (load.outcome != storage::RunCatalogLoadOutcome::LOADED) {
        return {.outcome = RunResumeOutcome::CATALOG_LOAD_FAILED, .catalogLoad = load};
    }
    const auto& writer = *controller.run_.journalWriter;
    const auto& header = writer.header();
    if (catalog.activeRunId != header.exchangeRunId) {
        return {.outcome = RunResumeOutcome::ACTIVE_RUN_ID_MISMATCH, .catalogLoad = load};
    }
    if (catalog.activeDisposition != storage::RunCatalogDisposition::PAUSED) {
        return {.outcome = RunResumeOutcome::CATALOG_NOT_PAUSED, .catalogLoad = load};
    }
    if (writer.committedCommandCount() >= header.maxRunCommands ||
        writer.committedByteCount() >= header.maxRunJournalBytes) {
        return {.outcome = RunResumeOutcome::CAPACITY_REACHED, .catalogLoad = load};
    }
    if (catalog.generation == std::numeric_limits<std::uint64_t>::max()) {
        return {.outcome = RunResumeOutcome::GENERATION_EXHAUSTED, .catalogLoad = load};
    }

    ++catalog.generation;
    catalog.activeDisposition = storage::RunCatalogDisposition::OPEN;
    auto replacement = storage::detail::replaceRunCatalogV1WithHooks(controller.canonicalCatalogPath_, catalog, hooks);
    if (replacement.outcome != storage::RunCatalogReplaceOutcome::COMMITTED) {
        return {.outcome = RunResumeOutcome::CATALOG_REPLACE_FAILED,
                .catalogLoad = load,
                .catalogReplacement = std::move(replacement)};
    }

    controller.openAdmission();
    controller.state_ = ExchangeRunStartupState::READY;
    return {.outcome = RunResumeOutcome::READY, .catalogLoad = load, .catalogReplacement = std::move(replacement)};
}

} // namespace detail

ExistingRunStartupResult ExchangeRunController::startupExistingRunV1(
    const std::filesystem::path& canonicalCatalogPath) noexcept {
    return detail::startupExistingRunV1WithHooks(canonicalCatalogPath, storage::detail::systemRunCatalogStorageHooks(),
                                                 *this);
}

NewRunStartupResult ExchangeRunController::startupNewRunV1(
    const std::filesystem::path& canonicalCatalogPath, const storage::NewRunConfigurationV1& configuration) noexcept {
    return detail::startupNewRunV1WithHooks(canonicalCatalogPath, configuration, nullptr, *this);
}

RunResumeResult ExchangeRunController::resumeRunV1() noexcept {
    return detail::resumeRunV1WithHooks(storage::detail::systemRunCatalogStorageHooks(), *this);
}

CompletedResultLookupResult ExchangeRunController::lookupCompletedResultV1(
    const std::filesystem::path& canonicalCatalogPath, const domain::ExchangeRunId exchangeRunId,
    const sequencer::sequenceMessage& command) {
    std::filesystem::path catalogBinding;
    try {
        catalogBinding = std::filesystem::absolute(canonicalCatalogPath).lexically_normal();
    } catch (...) {
        return {.outcome = CompletedResultLookupOutcome::CATALOG_PATH_FAILED};
    }

    const auto boundCatalog = canonicalCatalogPath_.lexically_normal();
    if (!boundCatalog.empty() && catalogBinding != boundCatalog) {
        return {.outcome = CompletedResultLookupOutcome::CATALOG_BINDING_MISMATCH};
    }

    if (run_.admissionIndex != nullptr && run_.admissionIndex->exchangeRunId() == exchangeRunId) {
        auto completed = run_.admissionIndex->completedResult(command);
        return {
            .outcome =
                completed == nullptr ? CompletedResultLookupOutcome::NOT_FOUND : CompletedResultLookupOutcome::FOUND,
            .completedResult = std::move(completed),
        };
    }

    storage::RunCatalogSnapshotV1 catalog;
    const auto load = storage::loadRunCatalogV1(catalogBinding, catalog);
    if (load.outcome != storage::RunCatalogLoadOutcome::LOADED) {
        return {
            .outcome = CompletedResultLookupOutcome::CATALOG_LOAD_FAILED,
            .catalogLoad = load,
        };
    }
    if (catalog.activeRunId == exchangeRunId) {
        return {
            .outcome = CompletedResultLookupOutcome::ACTIVE_RUN_NOT_OWNED,
            .catalogLoad = load,
        };
    }
    if (catalog.retainedStoppedRunId != exchangeRunId) {
        return {
            .outcome = CompletedResultLookupOutcome::RUN_NOT_RETAINED,
            .catalogLoad = load,
        };
    }

    if (retainedStoppedRunView_.has_value() && retainedStoppedRunView_->canonicalCatalogPath == catalogBinding &&
        retainedStoppedRunView_->exchangeRunId == exchangeRunId) {
        auto completed = retainedStoppedRunView_->admissionIndex->completedResult(command);
        return {
            .outcome =
                completed == nullptr ? CompletedResultLookupOutcome::NOT_FOUND : CompletedResultLookupOutcome::FOUND,
            .completedResult = std::move(completed),
            .catalogLoad = load,
        };
    }

    std::filesystem::path journalPath;
    if (!storage::detail::deriveCanonicalRunJournalPathV1(catalogBinding, exchangeRunId, journalPath)) {
        return {
            .outcome = CompletedResultLookupOutcome::JOURNAL_PATH_FAILED,
            .catalogLoad = load,
        };
    }

    RunStateV1 reconstructed;
    auto recovery = recovery::recoverRunV1(journalPath, reconstructed, exchangeRunId);
    if (recovery.outcome != recovery::RecoveredRunOutcome::RECOVERED) {
        return {
            .outcome = CompletedResultLookupOutcome::RECOVERY_FAILED,
            .catalogLoad = load,
            .recovery = std::move(recovery),
        };
    }

    RetainedStoppedRunView view{
        .canonicalCatalogPath = std::move(catalogBinding),
        .exchangeRunId = exchangeRunId,
        .matchingState = std::move(reconstructed.matchingState),
        .admissionIndex = std::move(reconstructed.admissionIndex),
    };
    reconstructed.journalWriter.reset();
    retainedStoppedRunView_.emplace(std::move(view));

    auto completed = retainedStoppedRunView_->admissionIndex->completedResult(command);
    return {
        .outcome = completed == nullptr ? CompletedResultLookupOutcome::NOT_FOUND : CompletedResultLookupOutcome::FOUND,
        .completedResult = std::move(completed),
        .catalogLoad = load,
        .recovery = std::move(recovery),
    };
}

PrivateResultLookupResult ExchangeRunController::lookupPrivateResultV1(
    const std::filesystem::path& canonicalCatalogPath, const domain::ExchangeRunId exchangeRunId,
    const sequencer::sequenceMessage& command) {
    auto authoritative = lookupCompletedResultV1(canonicalCatalogPath, exchangeRunId, command);
    PrivateResultLookupResult result{
        .outcome = authoritative.outcome,
        .catalogLoad = std::move(authoritative.catalogLoad),
        .recovery = std::move(authoritative.recovery),
    };
    if (authoritative.outcome != CompletedResultLookupOutcome::FOUND) {
        return result;
    }

    if (authoritative.completedResult == nullptr || !command.clientCommandId.has_value()) {
        result.outcome = CompletedResultLookupOutcome::INVARIANT_FAILURE;
        return result;
    }
    const auto& correlation = authoritative.completedResult->correlation();
    if (correlation.exchangeRunId != exchangeRunId || correlation.clientId != command.clientId ||
        correlation.clientCommandId != *command.clientCommandId) {
        result.outcome = CompletedResultLookupOutcome::INVARIANT_FAILURE;
        return result;
    }

    result.privateResult = private_result::projectPrivateResult(*authoritative.completedResult, command.clientId);
    if (!result.privateResult.has_value()) {
        result.outcome = CompletedResultLookupOutcome::INVARIANT_FAILURE;
    }
    return result;
}

void ExchangeRunController::openAdmission() {
    run_.admissionIndex->setAdmissionOpen(true);
}

void ExchangeRunController::closeAdmission() {
    if (run_.admissionIndex != nullptr) {
        run_.admissionIndex->setAdmissionOpen(false);
    }
}

ExchangeRunStartupState ExchangeRunController::state() const noexcept {
    return state_;
}

const matching_engine::MatchingState* ExchangeRunController::matchingState() const noexcept {
    return run_.matchingState.get();
}

const admission::CommandAdmissionIndex* ExchangeRunController::admissionIndex() const noexcept {
    return run_.admissionIndex.get();
}

const storage::RunJournalWriterV1* ExchangeRunController::journalWriter() const noexcept {
    return run_.journalWriter.get();
}

} // namespace exchange::core
