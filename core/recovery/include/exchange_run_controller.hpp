#pragma once

#include "recovered_run.hpp"
#include "run_catalog_storage.hpp"
#include "start_new_run.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>

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

// Read only on the single driving thread (or while externally quiesced).
struct CommandProcessingInspection final {
    bool installed{false};
    bool ingressEmpty{true};
    bool matchingEmpty{true};
    std::size_t queuedResults{0};
    bool pendingMatchingResult{false};
    std::optional<sequencer::sequenceMessage> inProgressMatchingCommand{};
    bool pendingCompletion{false};
    std::optional<sequencer::sequenceMessage> pendingCommand{};
    std::optional<storage::RunJournalAppendResult> appendFailure{};
    std::exception_ptr internalFailure{};
};

enum class RunPauseOutcome : std::uint8_t {
    PAUSED,
    NOT_READY,
    PATH_NOT_INSTALLED,
    PROCESSING_FAILED,
    DRAIN_STALLED,
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

    // Single-threaded composition; installation never changes the gate or reconstructs run state.
    // Capacities are positive; command queue capacities must fit Boost's fixed-size index.
    // Returns false when no Ready/Paused run is owned or a path is already installed.
    [[nodiscard]] bool installCommandProcessingV1(std::size_t submissionCapacity, std::size_t matchingCapacity,
                                                  std::size_t resultCapacity);
    // The caller supplies an already normalized command and explicit run binding.
    [[nodiscard]] admission::AdmissionDecision submitCommandV1(domain::ExchangeRunId exchangeRunId,
                                                               const sequencer::sequenceMessage& command);
    // One deterministic cycle: sequence, check append evidence, match, complete one result.
    // False means idle, not Ready, or latched failure; inspect to distinguish these cases.
    [[nodiscard]] bool advanceCommandProcessingV1();
    [[nodiscard]] CommandProcessingInspection inspectCommandProcessingV1() const;

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

    struct CommandProcessingPath;

    ExchangeRunStartupState state_{ExchangeRunStartupState::UNAVAILABLE};
    RunStateV1 run_{};
    std::filesystem::path canonicalCatalogPath_{};
    // Declared last so borrowing workers/queues are destroyed before their run owners.
    std::unique_ptr<CommandProcessingPath> commandProcessing_{};
};

} // namespace exchange::core
