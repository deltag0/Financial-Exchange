#pragma once

#include "recovered_run.hpp"

namespace exchange::core::recovery::detail {

struct RecoveredRunHooks final {
    decltype(&storage::prepareRunJournalV1) prepare;
    decltype(&matching_engine::replayPreparedRunJournalV1) replay;
    decltype(&admission::reconstructCommandAdmissionIndex) reconstructAdmission;
};

[[nodiscard]] RecoveredRunResult recoverRunV1WithHooks(
    const std::filesystem::path &canonicalJournalPath, const RecoveredRunHooks &hooks, RunStateV1 &output,
    std::optional<domain::ExchangeRunId> expectedRunId = {}) noexcept;

} // namespace exchange::core::recovery::detail
