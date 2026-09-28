#include "exchange_run_controller.hpp"

#include "admission_completion_consumer.hpp"
#include "bus.hpp"
#include "matching_engine.hpp"
#include "new_run_journal_internal.hpp"
#include "private_result_queue.hpp"
#include "private_result_projection.hpp"
#include "public_trade_queue.hpp"
#include "public_trade_projection.hpp"
#include "run_catalog_storage_internal.hpp"
#include "sequencer.hpp"

#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace exchange::core {

namespace {

class CommandResultDispatcher final {
public:
    CommandResultDispatcher(admission::AdmissionCompletionConsumer& completion,
                            private_result::BoundedPrivateResultQueue& privateHandoff,
                            market_data::BoundedPublicTradeQueue& publicHandoff)
        : completion_(completion), privateHandoff_(privateHandoff), publicHandoff_(publicHandoff) {}

    [[nodiscard]] bool advance() {
        bool progressed = false;
        if (pendingResult_ == nullptr) {
            if (!completion_.processNext(pendingResult_)) {
                return false;
            }
            progressed = true;
        }
        if (!projectionsReady_) {
            auto privateResults = private_result::projectPrivateResults(*pendingResult_);
            auto publicTrades = market_data::projectPublicTrades(*pendingResult_);
            pendingPrivateResults_ = std::move(privateResults);
            pendingPublicTrades_ = std::move(publicTrades);
            projectionsReady_ = true;
            progressed = true;
        }

        if (!pendingPrivateResults_.empty() && privateHandoff_.tryPush(pendingPrivateResults_)) {
            progressed = true;
        }
        if (!pendingPublicTrades_.empty() && publicHandoff_.tryPush(pendingPublicTrades_)) {
            progressed = true;
        }
        if (pendingPrivateResults_.empty() && pendingPublicTrades_.empty()) {
            clearPending();
        }
        return progressed;
    }

    [[nodiscard]] bool hasPendingResult() const noexcept {
        return pendingResult_ != nullptr;
    }

    [[nodiscard]] bool projectionPending() const noexcept {
        return pendingResult_ != nullptr && !projectionsReady_;
    }

    [[nodiscard]] bool privateHandoffPending() const noexcept {
        return !pendingPrivateResults_.empty();
    }

    [[nodiscard]] bool publicHandoffPending() const noexcept {
        return !pendingPublicTrades_.empty();
    }

    [[nodiscard]] std::size_t pendingPrivateEventCount() const noexcept {
        std::size_t count = 0;
        for (const auto& recipient : pendingPrivateResults_) {
            count += recipient.privateResult.size();
        }
        return count;
    }

    [[nodiscard]] std::size_t pendingTradeCount() const noexcept {
        return pendingPublicTrades_.size();
    }

private:
    void clearPending() noexcept {
        projectionsReady_ = false;
        pendingPrivateResults_.clear();
        pendingPublicTrades_.clear();
        pendingResult_.reset();
    }

    admission::AdmissionCompletionConsumer& completion_;
    private_result::BoundedPrivateResultQueue& privateHandoff_;
    market_data::BoundedPublicTradeQueue& publicHandoff_;
    matching_engine::ImmutableCommandResultBatch pendingResult_{};
    private_result::RecipientResults pendingPrivateResults_{};
    std::vector<market_data::PublicTrade> pendingPublicTrades_{};
    bool projectionsReady_{false};
};

} // namespace

struct ExchangeRunController::CommandProcessingPath final {
    CommandProcessingPath(RunStateV1& run, const std::size_t submissionCapacity, const std::size_t matchingCapacity,
                          const std::size_t resultCapacity, const std::size_t privateEventCapacity,
                          const std::size_t publicTradeRecordCapacity)
        : ingress(checkedCommandCapacity(submissionCapacity)),
          matching(checkedCommandCapacity(matchingCapacity)),
          results(resultCapacity),
          privateResults(checkedPrivateEventCapacity(privateEventCapacity, run)),
          publicTrades(checkedPublicTradeCapacity(publicTradeRecordCapacity, run)),
          sequencer(ingress, matching, *run.admissionIndex, *run.journalWriter),
          matcher(&matching, bus, results, *run.matchingState),
          completion(results, *run.admissionIndex),
          resultDispatcher(completion, privateResults, publicTrades) {}

    static std::size_t checkedCommandCapacity(const std::size_t capacity) {
        // Boost reserves a uint16 index for its free-list sentinel.
        if (capacity == 0 || capacity >= 65'535) {
            throw std::invalid_argument("controller command queue capacity must be between 1 and 65534");
        }
        return capacity;
    }

    static std::size_t checkedPublicTradeCapacity(const std::size_t capacity, const RunStateV1& run) {
        const std::uint64_t maximumBatch = run.journalWriter->header().maxEventsPerCommand;
        if (maximumBatch > std::numeric_limits<std::size_t>::max() || capacity < maximumBatch) {
            throw std::invalid_argument("controller public-trade record capacity must hold one maximum command result");
        }
        return capacity;
    }

    static std::size_t checkedPrivateEventCapacity(const std::size_t capacity, const RunStateV1& run) {
        const std::uint64_t maximumEvents = run.journalWriter->header().maxEventsPerCommand;
        if (capacity < 2 * maximumEvents) {
            throw std::invalid_argument("controller private-event capacity must hold one worst-case projection");
        }
        return capacity;
    }

    [[nodiscard]] bool processingDrained() const {
        return ingress.empty() && matching.empty() && results.size() == 0 && !sequencer.pendingCommand().has_value() &&
               !matcher.hasPendingResult() && !matcher.inProgressCommand().has_value() &&
               !completion.hasPendingBatch() && !resultDispatcher.hasPendingResult();
    }

    [[nodiscard]] bool resultHandoffsDrained() const {
        return privateResults.empty() && publicTrades.empty() && !resultDispatcher.privateHandoffPending() &&
               !resultDispatcher.publicHandoffPending();
    }

    // Queues precede workers; all workers borrow the same private, stable run owners.
    SharedQueue<sequencer::sequenceMessage> ingress;
    SharedQueue<sequencer::sequenceMessage> matching;
    matching_engine::BoundedCommandResultQueue results;
    private_result::BoundedPrivateResultQueue privateResults;
    market_data::BoundedPublicTradeQueue publicTrades;
    Bus bus{1};
    sequencer::Sequencer sequencer;
    matching_engine::MatchingEngine matcher;
    admission::AdmissionCompletionConsumer completion;
    CommandResultDispatcher resultDispatcher;
    std::exception_ptr internalFailure{};
    // The one submission/advancement driver updates these around atomic staging and writer progress.
    std::uint64_t acceptedNotAppendedCommands{0};
    std::uint64_t acceptedNotAppendedBytes{0};
    bool capacityTransitionPending{false};
    std::optional<storage::RunCatalogLoadResult> capacityCatalogLoad{};
    std::optional<storage::RunCatalogReplaceResult> capacityCatalogReplacement{};
    FailStopCatalogPreconditionFailure failStopCatalogPreconditionFailure{FailStopCatalogPreconditionFailure::NONE};
    std::optional<storage::RunCatalogLoadResult> failStopCatalogLoad{};
    std::optional<storage::RunCatalogReplaceResult> failStopCatalogReplacement{};
};

namespace {

std::optional<std::size_t> prospectiveFrameSize(const sequencer::sequenceMessage& command,
                                                const storage::RunJournalWriterV1& writer) {
    if (!command.clientCommandId.has_value() ||
        command.configurationVersion > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    const auto& header = writer.header();
    const auto configurationVersion = static_cast<std::uint32_t>(command.configurationVersion);
    if (command.type == sequencer::orderType::BUY || command.type == sequencer::orderType::SELL) {
        return writer.prospectiveFrameSize(storage::JournalNewOrderV1{
            .exchangeRunId = header.exchangeRunId,
            .commandSequence = writer.nextCommandSequence(),
            .behavioralRulesVersion = header.behavioralRulesVersion,
            .configurationVersion = configurationVersion,
            .clientId = command.clientId,
            .instrumentId = command.instrumentId,
            .clientCommandId = *command.clientCommandId,
            .side = command.type == sequencer::orderType::BUY ? domain::Side::BUY : domain::Side::SELL,
            .timeInForce = command.tif,
            .price = command.price,
            .quantity = command.quantity,
        });
    }
    if (command.type != sequencer::orderType::CANCEL || !command.targetOrderId.has_value()) {
        return std::nullopt;
    }
    return writer.prospectiveFrameSize(storage::JournalCancelV1{
        .exchangeRunId = header.exchangeRunId,
        .commandSequence = writer.nextCommandSequence(),
        .behavioralRulesVersion = header.behavioralRulesVersion,
        .configurationVersion = configurationVersion,
        .clientId = command.clientId,
        .instrumentId = command.instrumentId,
        .clientCommandId = *command.clientCommandId,
        .targetOrderId = *command.targetOrderId,
    });
}

StoppedRunCleanupResult removeStoppedJournal(const std::filesystem::path& journalPath,
                                             const std::filesystem::path& journalDirectory,
                                             const domain::ExchangeRunId replacedRunId,
                                             const storage::detail::RunCatalogStorageHooks& hooks) noexcept {
    StoppedRunCleanupResult result{
        .outcome = StoppedRunCleanupOutcome::REMOVE_FAILED,
        .replacedRunId = replacedRunId,
    };

    int operation = -1;
    do {
        operation = hooks.removeFile(hooks.context, journalPath.c_str());
    } while (operation < 0 && errno == EINTR);
    if (operation != 0) {
        result.systemError = errno;
        return result;
    }

    int directoryDescriptor = -1;
    do {
        directoryDescriptor = ::open(journalDirectory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } while (directoryDescriptor < 0 && errno == EINTR);
    if (directoryDescriptor < 0) {
        result.outcome = StoppedRunCleanupOutcome::DIRECTORY_OPEN_FAILED;
        result.systemError = errno;
        return result;
    }

    do {
        operation = hooks.syncFile(hooks.context, directoryDescriptor);
    } while (operation < 0 && errno == EINTR);
    if (operation != 0) {
        const int systemError = errno;
        do {
            operation = ::close(directoryDescriptor);
        } while (operation < 0 && errno == EINTR);
        result.outcome = StoppedRunCleanupOutcome::DIRECTORY_SYNC_FAILED;
        result.systemError = systemError;
        return result;
    }

    do {
        operation = ::close(directoryDescriptor);
    } while (operation < 0 && errno == EINTR);
    if (operation != 0) {
        result.outcome = StoppedRunCleanupOutcome::DIRECTORY_CLOSE_FAILED;
        result.systemError = errno;
        return result;
    }

    result.outcome = StoppedRunCleanupOutcome::COMMITTED;
    return result;
}

} // namespace

ExchangeRunController::ExchangeRunController() = default;
ExchangeRunController::~ExchangeRunController() = default;

bool ExchangeRunController::installCommandProcessingV1(const std::size_t submissionCapacity,
                                                       const std::size_t matchingCapacity,
                                                       const std::size_t resultCapacity,
                                                       const std::size_t privateEventCapacity,
                                                       const std::size_t publicTradeRecordCapacity) {
    if (commandProcessing_ != nullptr || run_.journalWriter == nullptr ||
        (state_ != ExchangeRunStartupState::READY && state_ != ExchangeRunStartupState::PAUSED)) {
        return false;
    }
    commandProcessing_ = std::make_unique<CommandProcessingPath>(
        run_, submissionCapacity, matchingCapacity, resultCapacity, privateEventCapacity, publicTradeRecordCapacity);
    return true;
}

admission::AdmissionDecision ExchangeRunController::submitCommandV1(const domain::ExchangeRunId exchangeRunId,
                                                                    const sequencer::sequenceMessage& command) {
    if (commandProcessing_ == nullptr || exchangeRunId != run_.journalWriter->header().exchangeRunId) {
        return {.status = admission::AdmissionStatus::ADMISSION_UNAVAILABLE,
                .rejectionReason = domain::AdmissionRejectionReason::EXCHANGE_RUN_UNAVAILABLE,
                .originalResult = {}};
    }
    auto& path = *commandProcessing_;
    if (state_ == ExchangeRunStartupState::CAPACITY_REACHED ||
        (state_ == ExchangeRunStartupState::READY && path.capacityTransitionPending)) {
        return run_.admissionIndex->reserveAndStageForRunCapacity(command, path.ingress, false);
    }
    if (state_ != ExchangeRunStartupState::READY) {
        return run_.admissionIndex->reserveAndStage(command, path.ingress);
    }

    const auto frameSize = prospectiveFrameSize(command, *run_.journalWriter);
    bool capacityAvailable = true;
    bool exhaustsCapacity = false;
    if (frameSize.has_value()) {
        const auto& header = run_.journalWriter->header();
        const std::uint64_t committedCommands = run_.journalWriter->committedCommandCount();
        const std::uint64_t committedBytes = run_.journalWriter->committedByteCount();
        const std::uint64_t frameBytes = *frameSize;
        capacityAvailable = path.acceptedNotAppendedCommands < header.maxRunCommands - committedCommands &&
                            path.acceptedNotAppendedBytes <= header.maxRunJournalBytes - committedBytes &&
                            frameBytes <= header.maxRunJournalBytes - committedBytes - path.acceptedNotAppendedBytes;
        if (capacityAvailable) {
            exhaustsCapacity = committedCommands + path.acceptedNotAppendedCommands + 1 == header.maxRunCommands ||
                               committedBytes + path.acceptedNotAppendedBytes + frameBytes == header.maxRunJournalBytes;
        }
    }

    auto decision = run_.admissionIndex->reserveAndStageForRunCapacity(command, path.ingress, capacityAvailable);
    if (decision.status == admission::AdmissionStatus::FIRST_SUBMISSION) {
        ++path.acceptedNotAppendedCommands;
        if (frameSize.has_value()) {
            path.acceptedNotAppendedBytes += *frameSize;
        }
        if (exhaustsCapacity) {
            path.capacityTransitionPending = true;
            closeAdmission();
        }
    } else if (decision.rejectionReason == domain::AdmissionRejectionReason::RUN_CAPACITY_REACHED) {
        path.capacityTransitionPending = true;
        closeAdmission();
    }
    return decision;
}

bool ExchangeRunController::advanceCommandProcessingV1() {
    return detail::advanceCommandProcessingV1WithHooks(storage::detail::systemRunCatalogStorageHooks(), *this);
}

namespace detail {

FailedRunRecoveryResult recoverFailedRunV1WithHooks(const std::filesystem::path& canonicalCatalogPath,
                                                    const storage::detail::RunCatalogStorageHooks& hooks,
                                                    ExchangeRunController& controller) noexcept {
    const bool installedFailStopped = controller.state_ == ExchangeRunStartupState::FAIL_STOPPED;
    if (!installedFailStopped && controller.state_ != ExchangeRunStartupState::UNAVAILABLE &&
        controller.state_ != ExchangeRunStartupState::RECOVERY_FAILED) {
        return {.outcome = FailedRunRecoveryOutcome::NOT_RECOVERABLE};
    }
    if (!installedFailStopped && controller.commandProcessing_ != nullptr) {
        return {.outcome = FailedRunRecoveryOutcome::NOT_RECOVERABLE};
    }

    FailedRunRecoveryResult result{
        .outcome = FailedRunRecoveryOutcome::CATALOG_LOAD_FAILED,
        .originalProcessingInspection =
            installedFailStopped ? std::optional{controller.inspectCommandProcessingV1()} : std::nullopt,
    };

    std::filesystem::path catalogBinding;
    try {
        catalogBinding = std::filesystem::absolute(canonicalCatalogPath).lexically_normal();
    } catch (...) {
        result.outcome = FailedRunRecoveryOutcome::CATALOG_PATH_FAILED;
        return result;
    }
    if (installedFailStopped && catalogBinding != controller.canonicalCatalogPath_.lexically_normal()) {
        result.outcome = FailedRunRecoveryOutcome::CATALOG_BINDING_MISMATCH;
        return result;
    }

    storage::RunCatalogSnapshotV1 catalog;
    result.catalogLoad = storage::loadRunCatalogV1(catalogBinding, catalog);
    if (result.catalogLoad->outcome != storage::RunCatalogLoadOutcome::LOADED) {
        return result;
    }
    if (!catalog.activeRunId.has_value()) {
        result.outcome = FailedRunRecoveryOutcome::NO_ACTIVE_RUN;
        return result;
    }

    const domain::ExchangeRunId runId = *catalog.activeRunId;
    if (installedFailStopped && controller.run_.journalWriter->header().exchangeRunId != runId) {
        result.outcome = FailedRunRecoveryOutcome::ACTIVE_RUN_ID_MISMATCH;
        return result;
    }
    if (catalog.activeDisposition != storage::RunCatalogDisposition::FAIL_STOPPED &&
        catalog.activeDisposition != storage::RunCatalogDisposition::RECOVERY_FAILED) {
        result.outcome = FailedRunRecoveryOutcome::CATALOG_NOT_FAILED;
        return result;
    }
    if (installedFailStopped && catalog.activeDisposition != storage::RunCatalogDisposition::FAIL_STOPPED) {
        result.outcome = FailedRunRecoveryOutcome::CATALOG_NOT_FAILED;
        return result;
    }
    if (catalog.generation == std::numeric_limits<std::uint64_t>::max()) {
        result.outcome = FailedRunRecoveryOutcome::GENERATION_EXHAUSTED;
        return result;
    }

    std::filesystem::path journalPath;
    if (!storage::detail::deriveCanonicalRunJournalPathV1(catalogBinding, runId, journalPath)) {
        result.outcome = FailedRunRecoveryOutcome::JOURNAL_PATH_FAILED;
        return result;
    }
    if (installedFailStopped && (!controller.commandProcessing_->privateResults.empty() ||
                                 !controller.commandProcessing_->publicTrades.empty())) {
        result.outcome = FailedRunRecoveryOutcome::RESULT_HANDOFF_BACKPRESSURE;
        return result;
    }

    if (installedFailStopped) {
        // Workers borrow the run owners, so destroy the path before closing the failed writer and releasing state.
        controller.commandProcessing_.reset();
        controller.run_ = {};
    }
    controller.state_ = ExchangeRunStartupState::UNAVAILABLE;

    RunStateV1 recovered;
    result.recovery = recovery::recoverRunV1(journalPath, recovered, runId);
    const bool recoveredSuccessfully = result.recovery->outcome == recovery::RecoveredRunOutcome::RECOVERED;

    if (!recoveredSuccessfully && catalog.activeDisposition == storage::RunCatalogDisposition::RECOVERY_FAILED) {
        controller.state_ = ExchangeRunStartupState::RECOVERY_FAILED;
        result.outcome = FailedRunRecoveryOutcome::RECOVERY_FAILED;
        return result;
    }

    ++catalog.generation;
    catalog.activeDisposition = recoveredSuccessfully ? storage::RunCatalogDisposition::PAUSED
                                                      : storage::RunCatalogDisposition::RECOVERY_FAILED;
    result.catalogReplacement = storage::detail::replaceRunCatalogV1WithHooks(catalogBinding, catalog, hooks);
    if (result.catalogReplacement->outcome != storage::RunCatalogReplaceOutcome::COMMITTED) {
        result.outcome = FailedRunRecoveryOutcome::CATALOG_REPLACE_FAILED;
        return result;
    }

    if (!recoveredSuccessfully) {
        controller.state_ = ExchangeRunStartupState::RECOVERY_FAILED;
        result.outcome = FailedRunRecoveryOutcome::RECOVERY_FAILED;
        return result;
    }

    controller.run_ = std::move(recovered);
    controller.canonicalCatalogPath_ = std::move(catalogBinding);
    controller.state_ = ExchangeRunStartupState::PAUSED;
    result.outcome = FailedRunRecoveryOutcome::PAUSED;
    return result;
}

bool advanceCommandProcessingV1WithHooks(const storage::detail::RunCatalogStorageHooks& hooks,
                                         ExchangeRunController& controller) {
    if (controller.commandProcessing_ == nullptr || controller.state_ != ExchangeRunStartupState::READY) {
        return false;
    }
    auto& path = *controller.commandProcessing_;
    const auto& writer = *controller.run_.journalWriter;
    const std::uint64_t beforeCommands = writer.committedCommandCount();
    const std::uint64_t beforeBytes = writer.committedByteCount();
    const auto accountJournalProgress = [&]() {
        const std::uint64_t appendedCommands = writer.committedCommandCount() - beforeCommands;
        const std::uint64_t appendedBytes = writer.committedByteCount() - beforeBytes;
        if (appendedCommands > path.acceptedNotAppendedCommands || appendedBytes > path.acceptedNotAppendedBytes) {
            return false;
        }
        path.acceptedNotAppendedCommands -= appendedCommands;
        path.acceptedNotAppendedBytes -= appendedBytes;
        return true;
    };
    const auto publishFailStopped = [&]() noexcept {
        controller.state_ = ExchangeRunStartupState::UNAVAILABLE;

        storage::RunCatalogSnapshotV1 catalog;
        const auto load = storage::loadRunCatalogV1(controller.canonicalCatalogPath_, catalog);
        path.failStopCatalogLoad = load;
        if (load.outcome != storage::RunCatalogLoadOutcome::LOADED) {
            return;
        }
        if (catalog.activeRunId != writer.header().exchangeRunId) {
            path.failStopCatalogPreconditionFailure = FailStopCatalogPreconditionFailure::ACTIVE_RUN_ID_MISMATCH;
            return;
        }
        if (catalog.activeDisposition != storage::RunCatalogDisposition::OPEN) {
            path.failStopCatalogPreconditionFailure = FailStopCatalogPreconditionFailure::CATALOG_NOT_OPEN;
            return;
        }
        if (catalog.generation == std::numeric_limits<std::uint64_t>::max()) {
            path.failStopCatalogPreconditionFailure = FailStopCatalogPreconditionFailure::GENERATION_EXHAUSTED;
            return;
        }

        ++catalog.generation;
        catalog.activeDisposition = storage::RunCatalogDisposition::FAIL_STOPPED;
        auto replacement =
            storage::detail::replaceRunCatalogV1WithHooks(controller.canonicalCatalogPath_, catalog, hooks);
        path.failStopCatalogReplacement = replacement;
        if (replacement.outcome != storage::RunCatalogReplaceOutcome::COMMITTED) {
            return;
        }
        controller.state_ = ExchangeRunStartupState::FAIL_STOPPED;
    };
    try {
        const bool sequenced = path.sequencer.advance();
        if (!accountJournalProgress()) {
            throw std::logic_error("controller capacity accounting diverged from journal progress");
        }
        if (path.sequencer.journalAppendFailure().has_value()) {
            controller.closeAdmission();
            publishFailStopped();
            return false;
        }
        const bool matched = path.matcher.advance();
        const bool dispatched = path.resultDispatcher.advance();
        const bool progressed = sequenced || matched || dispatched;
        if (!path.capacityTransitionPending || !path.processingDrained()) {
            return progressed;
        }

        storage::RunCatalogSnapshotV1 catalog;
        const auto load = storage::loadRunCatalogV1(controller.canonicalCatalogPath_, catalog);
        path.capacityCatalogLoad = load;
        if (load.outcome != storage::RunCatalogLoadOutcome::LOADED) {
            controller.state_ = ExchangeRunStartupState::UNAVAILABLE;
            return false;
        }
        if (catalog.activeRunId != writer.header().exchangeRunId ||
            catalog.activeDisposition != storage::RunCatalogDisposition::OPEN ||
            catalog.generation == std::numeric_limits<std::uint64_t>::max()) {
            controller.state_ = ExchangeRunStartupState::UNAVAILABLE;
            return false;
        }
        ++catalog.generation;
        catalog.activeDisposition = storage::RunCatalogDisposition::CAPACITY_REACHED;
        auto replacement =
            storage::detail::replaceRunCatalogV1WithHooks(controller.canonicalCatalogPath_, catalog, hooks);
        path.capacityCatalogReplacement = replacement;
        if (replacement.outcome != storage::RunCatalogReplaceOutcome::COMMITTED) {
            controller.state_ = ExchangeRunStartupState::UNAVAILABLE;
            return false;
        }
        controller.state_ = ExchangeRunStartupState::CAPACITY_REACHED;
        return true;
    } catch (...) {
        // No automatic retry after uncertain mutation or an invariant exception.
        (void)accountJournalProgress();
        path.internalFailure = std::current_exception();
        controller.closeAdmission();
        publishFailStopped();
        return false;
    }
}

} // namespace detail

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
            .pendingResultProjection = path.resultDispatcher.projectionPending(),
            .pendingPrivateEventRecords = path.resultDispatcher.pendingPrivateEventCount(),
            .queuedPrivateEventRecords = path.privateResults.queuedEventCount(),
            .pendingPublicTradeRecords = path.resultDispatcher.pendingTradeCount(),
            .queuedPublicTradeRecords = path.publicTrades.queuedRecordCount(),
            .pendingCommand = path.sequencer.pendingCommand(),
            .appendFailure = path.sequencer.journalAppendFailure(),
            .internalFailure = path.internalFailure,
            .capacityTransitionPending = path.capacityTransitionPending,
            .capacityCatalogLoad = path.capacityCatalogLoad,
            .capacityCatalogReplacement = path.capacityCatalogReplacement,
            .failStopCatalogPreconditionFailure = path.failStopCatalogPreconditionFailure,
            .failStopCatalogLoad = path.failStopCatalogLoad,
            .failStopCatalogReplacement = path.failStopCatalogReplacement};
}

bool ExchangeRunController::tryPopPrivateResultBatchV1(private_result::RecipientResults& batch) {
    return commandProcessing_ != nullptr && commandProcessing_->privateResults.tryPop(batch);
}

bool ExchangeRunController::tryPopPublicTradeBatchV1(std::vector<market_data::PublicTrade>& batch) {
    return commandProcessing_ != nullptr && commandProcessing_->publicTrades.tryPop(batch);
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
    if (controller.commandProcessing_->capacityTransitionPending) {
        return {.outcome = RunPauseOutcome::NOT_READY};
    }

    // This is the admission barrier: every reservation accepted before it remains owned by the path.
    controller.closeAdmission();
    auto& path = *controller.commandProcessing_;
    while (true) {
        if (path.processingDrained()) {
            if (!path.resultHandoffsDrained()) {
                return {.outcome = RunPauseOutcome::RESULT_HANDOFF_BACKPRESSURE};
            }
            break;
        }
        if (!controller.advanceCommandProcessingV1()) {
            const auto appendFailure = path.sequencer.journalAppendFailure();
            if (appendFailure.has_value() || path.internalFailure != nullptr ||
                controller.state_ == ExchangeRunStartupState::FAIL_STOPPED) {
                return {.outcome = RunPauseOutcome::PROCESSING_FAILED};
            }
            if (path.resultDispatcher.privateHandoffPending() || path.resultDispatcher.publicHandoffPending() ||
                !path.privateResults.empty() || !path.publicTrades.empty()) {
                return {.outcome = RunPauseOutcome::RESULT_HANDOFF_BACKPRESSURE};
            }
            controller.state_ = ExchangeRunStartupState::UNAVAILABLE;
            return {.outcome = RunPauseOutcome::DRAIN_STALLED};
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

RunStopResult stopRunV1WithHooks(const std::optional<domain::ExchangeRunId> replacedRunConfirmation,
                                 const storage::detail::RunCatalogStorageHooks& hooks,
                                 ExchangeRunController& controller) noexcept {
    const auto initialState = controller.state_;
    if (initialState != ExchangeRunStartupState::READY && initialState != ExchangeRunStartupState::PAUSED &&
        initialState != ExchangeRunStartupState::CAPACITY_REACHED) {
        return {.outcome = RunStopOutcome::NOT_STOPPABLE};
    }

    storage::RunCatalogSnapshotV1 catalog;
    const auto load = storage::loadRunCatalogV1(controller.canonicalCatalogPath_, catalog);
    RunStopResult result{
        .outcome = RunStopOutcome::CATALOG_LOAD_FAILED,
        .catalogLoad = load,
    };
    if (load.outcome != storage::RunCatalogLoadOutcome::LOADED) {
        return result;
    }

    const auto activeRunId = controller.run_.journalWriter->header().exchangeRunId;
    if (catalog.activeRunId != activeRunId) {
        result.outcome = RunStopOutcome::ACTIVE_RUN_ID_MISMATCH;
        return result;
    }

    const auto requiredDisposition =
        initialState == ExchangeRunStartupState::READY    ? storage::RunCatalogDisposition::OPEN
        : initialState == ExchangeRunStartupState::PAUSED ? storage::RunCatalogDisposition::PAUSED
                                                          : storage::RunCatalogDisposition::CAPACITY_REACHED;
    if (catalog.activeDisposition != requiredDisposition) {
        result.outcome = RunStopOutcome::CATALOG_DISPOSITION_MISMATCH;
        return result;
    }

    const std::uint64_t requiredGenerations = initialState == ExchangeRunStartupState::READY ? 2 : 1;
    if (std::numeric_limits<std::uint64_t>::max() - catalog.generation < requiredGenerations) {
        result.outcome = RunStopOutcome::GENERATION_EXHAUSTED;
        return result;
    }

    std::filesystem::path replacedJournalPath;
    std::filesystem::path replacedJournalDirectory;
    if (catalog.retainedStoppedRunId.has_value()) {
        result.cleanup.replacedRunId = catalog.retainedStoppedRunId;
        if (!replacedRunConfirmation.has_value()) {
            result.outcome = RunStopOutcome::RETAINED_RUN_CONFIRMATION_REQUIRED;
            return result;
        }
        if (replacedRunConfirmation != catalog.retainedStoppedRunId) {
            result.outcome = RunStopOutcome::RETAINED_RUN_CONFIRMATION_MISMATCH;
            return result;
        }
        if (!storage::detail::deriveCanonicalRunJournalPathV1(controller.canonicalCatalogPath_,
                                                              *catalog.retainedStoppedRunId, replacedJournalPath)) {
            result.outcome = RunStopOutcome::JOURNAL_PATH_FAILED;
            return result;
        }
        try {
            replacedJournalDirectory = replacedJournalPath.parent_path();
        } catch (...) {
            result.outcome = RunStopOutcome::JOURNAL_PATH_FAILED;
            return result;
        }
    }

    if (initialState == ExchangeRunStartupState::READY) {
        result.pause = pauseRunV1WithHooks(hooks, controller);
        if (result.pause->outcome != RunPauseOutcome::PAUSED) {
            result.outcome = result.pause->outcome == RunPauseOutcome::RESULT_HANDOFF_BACKPRESSURE
                                 ? RunStopOutcome::RESULT_HANDOFF_BACKPRESSURE
                                 : RunStopOutcome::PAUSE_FAILED;
            return result;
        }
        ++catalog.generation;
        catalog.activeDisposition = storage::RunCatalogDisposition::PAUSED;
    } else {
        controller.closeAdmission();
        if (controller.commandProcessing_ != nullptr && !controller.commandProcessing_->resultHandoffsDrained()) {
            result.outcome = RunStopOutcome::RESULT_HANDOFF_BACKPRESSURE;
            return result;
        }
    }

    ++catalog.generation;
    catalog.activeRunId.reset();
    catalog.activeDisposition = storage::RunCatalogDisposition::NONE;
    catalog.retainedStoppedRunId = activeRunId;
    result.catalogReplacement =
        storage::detail::replaceRunCatalogV1WithHooks(controller.canonicalCatalogPath_, catalog, hooks);
    if (result.catalogReplacement->outcome != storage::RunCatalogReplaceOutcome::COMMITTED) {
        controller.state_ = ExchangeRunStartupState::UNAVAILABLE;
        result.outcome = RunStopOutcome::CATALOG_REPLACE_FAILED;
        return result;
    }

    controller.state_ = ExchangeRunStartupState::STOPPED;
    // The committed selection makes any previously retained view obsolete even when cleanup fails.
    controller.retainedStoppedRunView_.reset();
    // Workers borrow the run owners, so release them before closing the writer and destroying run state.
    controller.commandProcessing_.reset();
    controller.run_ = {};

    if (!result.cleanup.replacedRunId.has_value()) {
        result.outcome = RunStopOutcome::STOPPED;
        return result;
    }

    result.cleanup =
        removeStoppedJournal(replacedJournalPath, replacedJournalDirectory, *result.cleanup.replacedRunId, hooks);
    result.outcome = result.cleanup.outcome == StoppedRunCleanupOutcome::COMMITTED
                         ? RunStopOutcome::STOPPED
                         : RunStopOutcome::STOPPED_CLEANUP_FAILED;
    return result;
}

} // namespace detail

RunPauseResult ExchangeRunController::pauseRunV1() noexcept {
    return detail::pauseRunV1WithHooks(storage::detail::systemRunCatalogStorageHooks(), *this);
}

RunStopResult ExchangeRunController::stopRunV1(
    const std::optional<domain::ExchangeRunId> replacedRunConfirmation) noexcept {
    return detail::stopRunV1WithHooks(replacedRunConfirmation, storage::detail::systemRunCatalogStorageHooks(), *this);
}

FailedRunRecoveryResult ExchangeRunController::recoverFailedRunV1(
    const std::filesystem::path& canonicalCatalogPath) noexcept {
    return detail::recoverFailedRunV1WithHooks(canonicalCatalogPath, storage::detail::systemRunCatalogStorageHooks(),
                                               *this);
}

} // namespace exchange::core
