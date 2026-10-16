#include "recovered_run.hpp"

#include "recovered_run_internal.hpp"

#include <utility>

namespace exchange::core::recovery {
namespace detail {

RecoveredRunResult recoverRunV1WithHooks(const std::filesystem::path &canonicalJournalPath,
                                         const RecoveredRunHooks &hooks, RunStateV1 &output,
                                         const std::optional<domain::ExchangeRunId> expectedRunId) noexcept {
    storage::PreparedRunJournalV1 prepared;
    storage::RunJournalRecoveryResult preparation = hooks.prepare(canonicalJournalPath, prepared, expectedRunId);
    if (preparation.outcome != storage::RunJournalRecoveryOutcome::PREPARED) {
        return {
            .outcome = RecoveredRunOutcome::PREPARATION_FAILED,
            .preparation = std::move(preparation),
        };
    }

    matching_engine::ReplayedRunJournalV1 replayed;
    const matching_engine::RunJournalReplayResult replay = hooks.replay(prepared, replayed);
    if (replay.outcome != matching_engine::RunJournalReplayOutcome::REPLAYED) {
        return {
            .outcome = RecoveredRunOutcome::REPLAY_FAILED,
            .preparation = std::move(preparation),
            .replay = replay,
        };
    }

    std::unique_ptr<admission::CommandAdmissionIndex> admissionIndex;
    const admission::CommandAdmissionRecoveryResult admission =
        hooks.reconstructAdmission(prepared.journal, replayed.commandResults, admissionIndex);
    if (admission.outcome != admission::CommandAdmissionRecoveryOutcome::RECONSTRUCTED) {
        return {
            .outcome = RecoveredRunOutcome::ADMISSION_FAILED,
            .preparation = std::move(preparation),
            .replay = replay,
            .admission = admission,
        };
    }

    RunStateV1 recovered{
        .journalWriter = std::move(prepared.writer),
        .matchingState = std::move(replayed.matchingState),
        .admissionIndex = std::move(admissionIndex),
    };
    output = std::move(recovered);
    return {
        .outcome = RecoveredRunOutcome::RECOVERED,
        .preparation = std::move(preparation),
        .replay = replay,
        .admission = admission,
    };
}

} // namespace detail

RecoveredRunResult recoverRunV1(const std::filesystem::path &canonicalJournalPath, RunStateV1 &output,
                                const std::optional<domain::ExchangeRunId> expectedRunId) noexcept {
    static constexpr detail::RecoveredRunHooks HOOKS{
        storage::prepareRunJournalV1,
        matching_engine::replayPreparedRunJournalV1,
        admission::reconstructCommandAdmissionIndex,
    };
    return detail::recoverRunV1WithHooks(canonicalJournalPath, HOOKS, output, expectedRunId);
}

} // namespace exchange::core::recovery
