#pragma once

#include "private_result_projection.hpp"
#include "public_trade_projection.hpp"
#include "recovered_run.hpp"
#include "run_catalog_storage.hpp"
#include "start_new_run.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace exchange::storage::detail {
struct RunCatalogStorageHooks;
}

namespace exchange::core {

enum class ExistingRunStartupOutcome : std::uint8_t {
    PAUSED,
    CATALOG_LOAD_FAILED,
    CATALOG_PATH_FAILED,
    NO_ACTIVE_RUN,
    EXPLICIT_OPERATION_REQUIRED,
    GENERATION_EXHAUSTED,
    JOURNAL_PATH_FAILED,
    RECOVERY_FAILED,
    CATALOG_REPLACE_FAILED,
};

struct ExistingRunStartupResult final {
    ExistingRunStartupOutcome outcome{ExistingRunStartupOutcome::CATALOG_LOAD_FAILED};
    storage::RunCatalogLoadResult catalogLoad{};
    std::optional<recovery::RecoveredRunResult> recovery{};
    std::optional<storage::RunCatalogReplaceResult> catalogReplacement{};
};

enum class ExchangeRunStartupState : std::uint8_t {
    UNAVAILABLE,
    READY,
    PAUSED,
    CAPACITY_REACHED,
    STOPPED,
    FAIL_STOPPED,
    RECOVERY_FAILED,
};

enum class NewRunStartupOutcome : std::uint8_t {
    READY,
    RUN_ALREADY_OWNED,
    ACTIVE_RUN_EXISTS,
    START_FAILED,
    CATALOG_PATH_FAILED,
};

struct NewRunStartupResult final {
    NewRunStartupOutcome outcome{NewRunStartupOutcome::START_FAILED};
    std::optional<storage::RunCatalogLoadResult> catalogLoad{};
    std::optional<StartNewRunResult> startup{};
};

enum class RunResumeOutcome : std::uint8_t {
    READY,
    NOT_PAUSED,
    CATALOG_LOAD_FAILED,
    ACTIVE_RUN_ID_MISMATCH,
    CATALOG_NOT_PAUSED,
    CAPACITY_REACHED,
    GENERATION_EXHAUSTED,
    CATALOG_REPLACE_FAILED,
};

struct RunResumeResult final {
    RunResumeOutcome outcome{RunResumeOutcome::NOT_PAUSED};
    std::optional<storage::RunCatalogLoadResult> catalogLoad{};
    std::optional<storage::RunCatalogReplaceResult> catalogReplacement{};
};

enum class FailStopCatalogPreconditionFailure : std::uint8_t {
    NONE,
    ACTIVE_RUN_ID_MISMATCH,
    CATALOG_NOT_OPEN,
    GENERATION_EXHAUSTED,
};

// Read only on the single driving thread (or while externally quiesced).
struct CommandProcessingInspection final {
    bool installed{false};
    bool ingressEmpty{true};
    bool matchingEmpty{true};
    std::size_t queuedResults{0};
    bool pendingMatchingResult{false};
    std::optional<sequencer::sequenceMessage> inProgressMatchingCommand{};
    bool pendingCompletion{false};
    bool pendingResultProjection{false};
    std::size_t pendingPrivateEventRecords{0};
    std::size_t queuedPrivateEventRecords{0};
    std::size_t pendingPublicTradeRecords{0};
    std::size_t queuedPublicTradeRecords{0};
    std::optional<sequencer::sequenceMessage> pendingCommand{};
    std::optional<storage::RunJournalAppendResult> appendFailure{};
    std::exception_ptr internalFailure{};
    bool capacityTransitionPending{false};
    std::optional<storage::RunCatalogLoadResult> capacityCatalogLoad{};
    std::optional<storage::RunCatalogReplaceResult> capacityCatalogReplacement{};
    FailStopCatalogPreconditionFailure failStopCatalogPreconditionFailure{FailStopCatalogPreconditionFailure::NONE};
    std::optional<storage::RunCatalogLoadResult> failStopCatalogLoad{};
    std::optional<storage::RunCatalogReplaceResult> failStopCatalogReplacement{};
};

enum class FailedRunRecoveryOutcome : std::uint8_t {
    PAUSED,
    NOT_RECOVERABLE,
    CATALOG_PATH_FAILED,
    CATALOG_LOAD_FAILED,
    NO_ACTIVE_RUN,
    CATALOG_BINDING_MISMATCH,
    ACTIVE_RUN_ID_MISMATCH,
    CATALOG_NOT_FAILED,
    GENERATION_EXHAUSTED,
    JOURNAL_PATH_FAILED,
    RESULT_HANDOFF_BACKPRESSURE,
    RECOVERY_FAILED,
    CATALOG_REPLACE_FAILED,
};

struct FailedRunRecoveryResult final {
    FailedRunRecoveryOutcome outcome{FailedRunRecoveryOutcome::NOT_RECOVERABLE};
    std::optional<CommandProcessingInspection> originalProcessingInspection{};
    std::optional<storage::RunCatalogLoadResult> catalogLoad{};
    std::optional<recovery::RecoveredRunResult> recovery{};
    std::optional<storage::RunCatalogReplaceResult> catalogReplacement{};
};

enum class RunPauseOutcome : std::uint8_t {
    PAUSED,
    NOT_READY,
    PATH_NOT_INSTALLED,
    PROCESSING_FAILED,
    DRAIN_STALLED,
    RESULT_HANDOFF_BACKPRESSURE,
    CATALOG_LOAD_FAILED,
    ACTIVE_RUN_ID_MISMATCH,
    CATALOG_NOT_OPEN,
    GENERATION_EXHAUSTED,
    CATALOG_REPLACE_FAILED,
};

struct RunPauseResult final {
    RunPauseOutcome outcome{RunPauseOutcome::NOT_READY};
    std::optional<storage::RunCatalogLoadResult> catalogLoad{};
    std::optional<storage::RunCatalogReplaceResult> catalogReplacement{};
};

enum class StoppedRunCleanupOutcome : std::uint8_t {
    NOT_REQUIRED,
    COMMITTED,
    REMOVE_FAILED,
    DIRECTORY_OPEN_FAILED,
    DIRECTORY_SYNC_FAILED,
    DIRECTORY_CLOSE_FAILED,
};

struct StoppedRunCleanupResult final {
    StoppedRunCleanupOutcome outcome{StoppedRunCleanupOutcome::NOT_REQUIRED};
    std::optional<domain::ExchangeRunId> replacedRunId{};
    int systemError{0};
};

enum class RunStopOutcome : std::uint8_t {
    STOPPED,
    STOPPED_CLEANUP_FAILED,
    NOT_STOPPABLE,
    PAUSE_FAILED,
    RESULT_HANDOFF_BACKPRESSURE,
    CATALOG_LOAD_FAILED,
    ACTIVE_RUN_ID_MISMATCH,
    CATALOG_DISPOSITION_MISMATCH,
    GENERATION_EXHAUSTED,
    RETAINED_RUN_CONFIRMATION_REQUIRED,
    RETAINED_RUN_CONFIRMATION_MISMATCH,
    JOURNAL_PATH_FAILED,
    CATALOG_REPLACE_FAILED,
};

struct RunStopResult final {
    RunStopOutcome outcome{RunStopOutcome::NOT_STOPPABLE};
    std::optional<RunPauseResult> pause{};
    std::optional<storage::RunCatalogLoadResult> catalogLoad{};
    std::optional<storage::RunCatalogReplaceResult> catalogReplacement{};
    StoppedRunCleanupResult cleanup{};
};

enum class CompletedResultLookupOutcome : std::uint8_t {
    FOUND,
    NOT_FOUND,
    CATALOG_PATH_FAILED,
    CATALOG_BINDING_MISMATCH,
    CATALOG_LOAD_FAILED,
    ACTIVE_RUN_NOT_OWNED,
    RUN_NOT_RETAINED,
    JOURNAL_PATH_FAILED,
    RECOVERY_FAILED,
    INVARIANT_FAILURE,
};

struct CompletedResultLookupResult final {
    CompletedResultLookupOutcome outcome{CompletedResultLookupOutcome::NOT_FOUND};
    matching_engine::ImmutableCommandResultBatch completedResult{};
    std::optional<storage::RunCatalogLoadResult> catalogLoad{};
    std::optional<recovery::RecoveredRunResult> recovery{};
};

struct PrivateResultLookupResult final {
    CompletedResultLookupOutcome outcome{CompletedResultLookupOutcome::NOT_FOUND};
    std::optional<private_result::RecipientResult> privateResult{};
    std::optional<storage::RunCatalogLoadResult> catalogLoad{};
    std::optional<recovery::RecoveredRunResult> recovery{};
};

class ExchangeRunController;

namespace detail {
struct StartNewRunHooks;
[[nodiscard]] ExistingRunStartupResult startupExistingRunV1WithHooks(
    const std::filesystem::path& canonicalCatalogPath, const storage::detail::RunCatalogStorageHooks& hooks,
    ExchangeRunController& controller) noexcept;
[[nodiscard]] NewRunStartupResult startupNewRunV1WithHooks(const std::filesystem::path& canonicalCatalogPath,
                                                           const storage::NewRunConfigurationV1& configuration,
                                                           const StartNewRunHooks* hooks,
                                                           ExchangeRunController& controller) noexcept;
[[nodiscard]] RunResumeResult resumeRunV1WithHooks(const storage::detail::RunCatalogStorageHooks& hooks,
                                                   ExchangeRunController& controller) noexcept;
[[nodiscard]] RunPauseResult pauseRunV1WithHooks(const storage::detail::RunCatalogStorageHooks& hooks,
                                                 ExchangeRunController& controller) noexcept;
[[nodiscard]] RunStopResult stopRunV1WithHooks(std::optional<domain::ExchangeRunId> replacedRunConfirmation,
                                               const storage::detail::RunCatalogStorageHooks& hooks,
                                               ExchangeRunController& controller) noexcept;
[[nodiscard]] FailedRunRecoveryResult recoverFailedRunV1WithHooks(const std::filesystem::path& canonicalCatalogPath,
                                                                  const storage::detail::RunCatalogStorageHooks& hooks,
                                                                  ExchangeRunController& controller) noexcept;
[[nodiscard]] bool advanceCommandProcessingV1WithHooks(const storage::detail::RunCatalogStorageHooks& hooks,
                                                       ExchangeRunController& controller);
} // namespace detail

// Caller guarantees exclusive ownership of the catalog, selected journal, and this controller.
// Startup and resume open run-bound admission only after committed Open publication.
class ExchangeRunController final {
public:
    ExchangeRunController();
    ~ExchangeRunController();
    ExchangeRunController(ExchangeRunController&&) = delete;
    ExchangeRunController& operator=(ExchangeRunController&&) = delete;

    [[nodiscard]] ExistingRunStartupResult startupExistingRunV1(
        const std::filesystem::path& canonicalCatalogPath) noexcept;
    [[nodiscard]] NewRunStartupResult startupNewRunV1(const std::filesystem::path& canonicalCatalogPath,
                                                      const storage::NewRunConfigurationV1& configuration) noexcept;
    [[nodiscard]] RunResumeResult resumeRunV1() noexcept;
    // Ordered single-threaded barrier over an installed path; leaves admission closed on failure.
    [[nodiscard]] RunPauseResult pauseRunV1() noexcept;
    // Stops the active run and, when confirmed, replaces the one retained stopped run.
    [[nodiscard]] RunStopResult stopRunV1(
        std::optional<domain::ExchangeRunId> replacedRunConfirmation = std::nullopt) noexcept;
    // Explicitly reconstructs the catalog-selected FailStopped or RecoveryFailed run.
    [[nodiscard]] FailedRunRecoveryResult recoverFailedRunV1(
        const std::filesystem::path& canonicalCatalogPath) noexcept;
    // Looks up only the owned active run or the catalog-selected retained stopped run.
    [[nodiscard]] CompletedResultLookupResult lookupCompletedResultV1(const std::filesystem::path& canonicalCatalogPath,
                                                                      domain::ExchangeRunId exchangeRunId,
                                                                      const sequencer::sequenceMessage& command);
    // Participant-safe lookup derives its recipient only from the normalized command identity.
    [[nodiscard]] PrivateResultLookupResult lookupPrivateResultV1(const std::filesystem::path& canonicalCatalogPath,
                                                                  domain::ExchangeRunId exchangeRunId,
                                                                  const sequencer::sequenceMessage& command);

    // Single-threaded composition; installation never changes the gate or reconstructs run state.
    // Capacities are positive; command queues fit Boost's index and delivery queues hold worst-case projections.
    // Returns false when no Ready/Paused run is owned or a path is already installed.
    [[nodiscard]] bool installCommandProcessingV1(std::size_t submissionCapacity, std::size_t matchingCapacity,
                                                  std::size_t resultCapacity, std::size_t privateEventCapacity,
                                                  std::size_t publicTradeRecordCapacity);
    // The caller supplies an already normalized command and explicit run binding.
    [[nodiscard]] admission::AdmissionDecision submitCommandV1(domain::ExchangeRunId exchangeRunId,
                                                               const sequencer::sequenceMessage& command);
    // One deterministic cycle: sequence, check append evidence, match, complete, and hand off one result.
    // False means idle, not Ready, or latched failure; inspect to distinguish these cases.
    [[nodiscard]] bool advanceCommandProcessingV1();
    [[nodiscard]] CommandProcessingInspection inspectCommandProcessingV1() const;
    [[nodiscard]] bool tryPopPrivateResultBatchV1(private_result::RecipientResults& batch);
    [[nodiscard]] bool tryPopPublicTradeBatchV1(std::vector<market_data::PublicTrade>& batch);

    // Closes new reservations only; this is not an ordered pause/drain barrier or a catalog transition.
    void closeAdmission();

    [[nodiscard]] ExchangeRunStartupState state() const noexcept;
    [[nodiscard]] const matching_engine::MatchingState* matchingState() const noexcept;
    [[nodiscard]] const admission::CommandAdmissionIndex* admissionIndex() const noexcept;
    [[nodiscard]] const storage::RunJournalWriterV1* journalWriter() const noexcept;

private:
    void openAdmission();

    friend ExistingRunStartupResult detail::startupExistingRunV1WithHooks(
        const std::filesystem::path&, const storage::detail::RunCatalogStorageHooks&, ExchangeRunController&) noexcept;
    friend NewRunStartupResult detail::startupNewRunV1WithHooks(const std::filesystem::path&,
                                                                const storage::NewRunConfigurationV1&,
                                                                const detail::StartNewRunHooks*,
                                                                ExchangeRunController&) noexcept;
    friend RunResumeResult detail::resumeRunV1WithHooks(const storage::detail::RunCatalogStorageHooks&,
                                                        ExchangeRunController&) noexcept;
    friend RunPauseResult detail::pauseRunV1WithHooks(const storage::detail::RunCatalogStorageHooks&,
                                                      ExchangeRunController&) noexcept;
    friend RunStopResult detail::stopRunV1WithHooks(std::optional<domain::ExchangeRunId>,
                                                    const storage::detail::RunCatalogStorageHooks&,
                                                    ExchangeRunController&) noexcept;
    friend FailedRunRecoveryResult detail::recoverFailedRunV1WithHooks(const std::filesystem::path&,
                                                                       const storage::detail::RunCatalogStorageHooks&,
                                                                       ExchangeRunController&) noexcept;
    friend bool detail::advanceCommandProcessingV1WithHooks(const storage::detail::RunCatalogStorageHooks&,
                                                            ExchangeRunController&);

    struct CommandProcessingPath;
    struct RetainedStoppedRunView final {
        std::filesystem::path canonicalCatalogPath{};
        domain::ExchangeRunId exchangeRunId{};
        std::unique_ptr<matching_engine::MatchingState> matchingState{};
        std::unique_ptr<admission::CommandAdmissionIndex> admissionIndex{};
    };

    ExchangeRunStartupState state_{ExchangeRunStartupState::UNAVAILABLE};
    RunStateV1 run_{};
    std::filesystem::path canonicalCatalogPath_{};
    std::optional<RetainedStoppedRunView> retainedStoppedRunView_{};
    // Declared last so borrowing workers/queues are destroyed before their run owners.
    std::unique_ptr<CommandProcessingPath> commandProcessing_{};
};

} // namespace exchange::core
