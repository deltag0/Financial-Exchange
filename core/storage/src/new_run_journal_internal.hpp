#pragma once

#include "new_run_journal.hpp"

namespace exchange::storage::detail {

struct NewRunJournalPreparationHooks final {
    void *context{nullptr};
    RunJournalCreateResult (*createWriter)(void *context, const std::filesystem::path &canonicalJournalPath,
                                           const RunHeaderV1 &header,
                                           std::unique_ptr<RunJournalWriterV1> &output) noexcept {nullptr};
};

[[nodiscard]] bool deriveCanonicalRunJournalPathV1(const std::filesystem::path &canonicalCatalogPath,
                                                   domain::ExchangeRunId exchangeRunId,
                                                   std::filesystem::path &output) noexcept;

[[nodiscard]] NewRunJournalPreparationResult prepareNewRunJournalV1WithHooks(
    const std::filesystem::path &canonicalCatalogPath, const NewRunConfigurationV1 &configuration,
    const NewRunJournalPreparationHooks &hooks, PreparedNewRunJournalV1 &output) noexcept;

} // namespace exchange::storage::detail
