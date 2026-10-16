#include "new_run_activation.hpp"

#include "new_run_activation_internal.hpp"
#include "new_run_journal_internal.hpp"

#include <limits>
#include <utility>

namespace exchange::storage {
namespace detail {

NewRunActivationResult activatePreparedNewRunV1WithHooks(const std::filesystem::path &canonicalCatalogPath,
                                                         const PreparedNewRunJournalV1 &prepared,
                                                         const RunCatalogStorageHooks &storageHooks) noexcept {
    if (prepared.writer == nullptr) {
        return {.outcome = NewRunActivationOutcome::MISSING_WRITER};
    }

    const domain::ExchangeRunId exchangeRunId = prepared.header.exchangeRunId;
    if (exchangeRunId.value() == 0) {
        return {.outcome = NewRunActivationOutcome::ZERO_EXCHANGE_RUN_ID};
    }

    std::filesystem::path expectedJournalPath;
    if (!deriveCanonicalRunJournalPathV1(canonicalCatalogPath, exchangeRunId, expectedJournalPath) ||
        prepared.canonicalJournalPath != expectedJournalPath) {
        return {.outcome = NewRunActivationOutcome::NONCANONICAL_JOURNAL_PATH};
    }

    RunCatalogSnapshotV1 current{};
    const RunCatalogLoadResult load = loadRunCatalogV1(canonicalCatalogPath, current);
    if (load.outcome != RunCatalogLoadOutcome::LOADED) {
        return {
            .outcome = NewRunActivationOutcome::CATALOG_LOAD_FAILED,
            .catalogLoad = load,
        };
    }

    if (!current.lastReservedRunId.has_value() || *current.lastReservedRunId != exchangeRunId) {
        return {
            .outcome = NewRunActivationOutcome::LAST_RESERVED_RUN_ID_MISMATCH,
            .catalogLoad = load,
        };
    }
    if (current.activeRunId.has_value()) {
        return {
            .outcome = NewRunActivationOutcome::ACTIVE_RUN_EXISTS,
            .catalogLoad = load,
        };
    }
    if (current.generation == std::numeric_limits<std::uint64_t>::max()) {
        return {
            .outcome = NewRunActivationOutcome::GENERATION_EXHAUSTED,
            .catalogLoad = load,
        };
    }

    RunCatalogSnapshotV1 next = current;
    ++next.generation;
    next.activeRunId = exchangeRunId;
    next.activeDisposition = RunCatalogDisposition::OPEN;

    RunCatalogReplaceResult replacement = replaceRunCatalogV1WithHooks(canonicalCatalogPath, next, storageHooks);
    if (replacement.outcome != RunCatalogReplaceOutcome::COMMITTED) {
        return {
            .outcome = NewRunActivationOutcome::CATALOG_REPLACE_FAILED,
            .catalogLoad = load,
            .catalogReplacement = std::move(replacement),
        };
    }

    return {
        .outcome = NewRunActivationOutcome::ACTIVATED,
        .catalogLoad = load,
        .catalogReplacement = std::move(replacement),
    };
}

} // namespace detail

NewRunActivationResult activatePreparedNewRunV1(const std::filesystem::path &canonicalCatalogPath,
                                                const PreparedNewRunJournalV1 &prepared) noexcept {
    return detail::activatePreparedNewRunV1WithHooks(canonicalCatalogPath, prepared,
                                                     detail::systemRunCatalogStorageHooks());
}

} // namespace exchange::storage
