#include "exchange_run_controller.hpp"

#include "admission_completion_consumer.hpp"
#include "bus.hpp"
#include "matching_engine.hpp"
#include "run_catalog_storage_internal.hpp"
#include "sequencer.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace exchange::core {

struct ExchangeRunController::CommandProcessingPath final {
    CommandProcessingPath(RunStateV1& run, const std::size_t submissionCapacity, const std::size_t matchingCapacity,
                          const std::size_t resultCapacity)
        : ingress(checkedCommandCapacity(submissionCapacity)),
          matching(checkedCommandCapacity(matchingCapacity)),
          results(resultCapacity),
          sequencer(ingress, matching, *run.admissionIndex, *run.journalWriter),
          matcher(&matching, bus, results, *run.matchingState),
          completion(results, *run.admissionIndex) {}

    static std::size_t checkedCommandCapacity(const std::size_t capacity) {
        // Boost reserves a uint16 index for its free-list sentinel.
        if (capacity == 0 || capacity >= 65'535) {
            throw std::invalid_argument("controller command queue capacity must be between 1 and 65534");
        }
        return capacity;
    }

    // Queues precede workers; all workers borrow the same private, stable run owners.
    SharedQueue<sequencer::sequenceMessage> ingress;
    SharedQueue<sequencer::sequenceMessage> matching;
    matching_engine::BoundedCommandResultQueue results;
    // No delivery consumer in this slice; keep the existing matcher publication boundary bounded.
    Bus bus{1};
    sequencer::Sequencer sequencer;
    matching_engine::MatchingEngine matcher;
    admission::AdmissionCompletionConsumer completion;
    std::exception_ptr internalFailure{};
};

ExchangeRunController::ExchangeRunController() = default;
ExchangeRunController::~ExchangeRunController() = default;

bool ExchangeRunController::installCommandProcessingV1(const std::size_t submissionCapacity,
                                                       const std::size_t matchingCapacity,
                                                       const std::size_t resultCapacity) {
    if (commandProcessing_ != nullptr || run_.journalWriter == nullptr ||
        (state_ != ExchangeRunStartupState::READY && state_ != ExchangeRunStartupState::PAUSED)) {
        return false;
    }
    commandProcessing_ =
        std::make_unique<CommandProcessingPath>(run_, submissionCapacity, matchingCapacity, resultCapacity);
    return true;
}

admission::AdmissionDecision ExchangeRunController::submitCommandV1(const domain::ExchangeRunId exchangeRunId,
                                                                    const sequencer::sequenceMessage& command) {
    if (commandProcessing_ == nullptr || exchangeRunId != run_.journalWriter->header().exchangeRunId) {
        return {.status = admission::AdmissionStatus::ADMISSION_UNAVAILABLE,
                .rejectionReason = domain::AdmissionRejectionReason::EXCHANGE_RUN_UNAVAILABLE,
                .originalResult = {}};
    }
    return run_.admissionIndex->reserveAndStage(command, commandProcessing_->ingress);
}

bool ExchangeRunController::advanceCommandProcessingV1() {
    if (commandProcessing_ == nullptr || state_ != ExchangeRunStartupState::READY) {
        return false;
    }
    auto& path = *commandProcessing_;
    try {
        const bool sequenced = path.sequencer.advance();
        if (path.sequencer.journalAppendFailure().has_value()) {
            closeAdmission();
            state_ = ExchangeRunStartupState::UNAVAILABLE;
            return false;
        }
        const bool matched = path.matcher.advance();
        const bool completed = path.completion.processNext();
        return sequenced || matched || completed;
    } catch (...) {
        // No automatic retry after uncertain mutation or an invariant exception.
        path.internalFailure = std::current_exception();
        closeAdmission();
        state_ = ExchangeRunStartupState::UNAVAILABLE;
        return false;
    }
}

CommandProcessingInspection ExchangeRunController::inspectCommandProcessingV1() const {
    if (commandProcessing_ == nullptr) {
        return {};
    }
    const auto& path = *commandProcessing_;
    return {.installed = true,
            .ingressEmpty = path.ingress.empty(),
            .matchingEmpty = path.matching.empty(),
            .queuedResults = path.results.size(),
            .pendingMatchingResult = path.matcher.hasPendingResult(),
            .inProgressMatchingCommand = path.matcher.inProgressCommand(),
            .pendingCompletion = path.completion.hasPendingBatch(),
            .pendingCommand = path.sequencer.pendingCommand(),
            .appendFailure = path.sequencer.journalAppendFailure(),
            .internalFailure = path.internalFailure};
}

namespace detail {

RunPauseResult pauseRunV1WithHooks(const storage::detail::RunCatalogStorageHooks& hooks,
                                   ExchangeRunController& controller) noexcept {
    if (controller.state_ != ExchangeRunStartupState::READY) {
        return {.outcome = RunPauseOutcome::NOT_READY};
    }
    if (controller.commandProcessing_ == nullptr) {
        return {.outcome = RunPauseOutcome::PATH_NOT_INSTALLED};
    }

    // This is the admission barrier: every reservation accepted before it remains owned by the path.
    controller.closeAdmission();
    auto& path = *controller.commandProcessing_;
    while (true) {
        if (path.ingress.empty() && path.matching.empty() && path.results.size() == 0 &&
            !path.sequencer.pendingCommand().has_value() && !path.matcher.hasPendingResult() &&
            !path.matcher.inProgressCommand().has_value() && !path.completion.hasPendingBatch()) {
            break;
        }
        if (!controller.advanceCommandProcessingV1()) {
            controller.state_ = ExchangeRunStartupState::UNAVAILABLE;
            const auto appendFailure = path.sequencer.journalAppendFailure();
            return {.outcome = appendFailure.has_value() || path.internalFailure != nullptr
                                   ? RunPauseOutcome::PROCESSING_FAILED
                                   : RunPauseOutcome::DRAIN_STALLED};
        }
    }

    storage::RunCatalogSnapshotV1 catalog;
    const auto load = storage::loadRunCatalogV1(controller.canonicalCatalogPath_, catalog);
    if (load.outcome != storage::RunCatalogLoadOutcome::LOADED) {
        controller.state_ = ExchangeRunStartupState::UNAVAILABLE;
        return {.outcome = RunPauseOutcome::CATALOG_LOAD_FAILED, .catalogLoad = load};
    }
    if (catalog.activeRunId != controller.run_.journalWriter->header().exchangeRunId) {
        controller.state_ = ExchangeRunStartupState::UNAVAILABLE;
        return {.outcome = RunPauseOutcome::ACTIVE_RUN_ID_MISMATCH, .catalogLoad = load};
    }
    if (catalog.activeDisposition != storage::RunCatalogDisposition::OPEN) {
        controller.state_ = ExchangeRunStartupState::UNAVAILABLE;
        return {.outcome = RunPauseOutcome::CATALOG_NOT_OPEN, .catalogLoad = load};
    }
    if (catalog.generation == std::numeric_limits<std::uint64_t>::max()) {
        controller.state_ = ExchangeRunStartupState::UNAVAILABLE;
        return {.outcome = RunPauseOutcome::GENERATION_EXHAUSTED, .catalogLoad = load};
    }

    ++catalog.generation;
    catalog.activeDisposition = storage::RunCatalogDisposition::PAUSED;
    auto replacement = storage::detail::replaceRunCatalogV1WithHooks(controller.canonicalCatalogPath_, catalog, hooks);
    if (replacement.outcome != storage::RunCatalogReplaceOutcome::COMMITTED) {
        controller.state_ = ExchangeRunStartupState::UNAVAILABLE;
        return {.outcome = RunPauseOutcome::CATALOG_REPLACE_FAILED,
                .catalogLoad = load,
                .catalogReplacement = std::move(replacement)};
    }
    controller.state_ = ExchangeRunStartupState::PAUSED;
    return {.outcome = RunPauseOutcome::PAUSED, .catalogLoad = load, .catalogReplacement = std::move(replacement)};
}

} // namespace detail

RunPauseResult ExchangeRunController::pauseRunV1() noexcept {
    return detail::pauseRunV1WithHooks(storage::detail::systemRunCatalogStorageHooks(), *this);
}

} // namespace exchange::core
