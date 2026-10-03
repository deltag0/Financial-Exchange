#include "exchange_run_id_reservation.hpp"

#include "exchange_run_id_reservation_internal.hpp"

#include <cerrno>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <utility>

namespace exchange::storage {
namespace {

bool hasJournalLikeEntry(const std::filesystem::path &dataDirectory, bool &found, int &systemError) noexcept {
    try {
        std::error_code error;
        std::filesystem::directory_iterator entry(dataDirectory, error);
        if (error) {
            systemError = error.value();
            return false;
        }

        const std::filesystem::directory_iterator end;
        while (entry != end) {
            const std::string filename = entry->path().filename().string();
            if (filename.starts_with("run-") && filename.ends_with(".fxjr")) {
                found = true;
                return true;
            }
            entry.increment(error);
            if (error) {
                systemError = error.value();
                return false;
            }
        }
        found = false;
        return true;
    } catch (const std::bad_alloc &) {
        systemError = ENOMEM;
        return false;
    } catch (...) {
        systemError = EIO;
        return false;
    }
}

} // namespace

namespace detail {

ExchangeRunIdReservationResult reserveNextExchangeRunIdWithHooks(const std::filesystem::path &canonicalCatalogPath,
                                                                 const RunCatalogStorageHooks &storageHooks,
                                                                 domain::ExchangeRunId &output) noexcept {
    RunCatalogSnapshotV1 current{};
    const RunCatalogLoadResult load = loadRunCatalogV1(canonicalCatalogPath, current);
    if (load.outcome == RunCatalogLoadOutcome::INVALID || load.outcome == RunCatalogLoadOutcome::IO_FAILURE) {
        return {
            .outcome = ExchangeRunIdReservationOutcome::CATALOG_LOAD_FAILED,
            .catalogLoad = load,
        };
    }

    RunCatalogSnapshotV1 next{};
    domain::ExchangeRunId proposedRunId{};
    if (load.outcome == RunCatalogLoadOutcome::MISSING) {
        std::filesystem::path dataDirectory;
        try {
            dataDirectory = canonicalCatalogPath.parent_path();
            if (dataDirectory.empty()) {
                dataDirectory = ".";
            }
        } catch (const std::bad_alloc &) {
            return {
                .outcome = ExchangeRunIdReservationOutcome::DATA_DIRECTORY_IO_FAILURE,
                .catalogLoad = load,
                .systemError = ENOMEM,
            };
        } catch (...) {
            return {
                .outcome = ExchangeRunIdReservationOutcome::DATA_DIRECTORY_IO_FAILURE,
                .catalogLoad = load,
                .systemError = EIO,
            };
        }

        bool journalPresent = false;
        int systemError = 0;
        if (!hasJournalLikeEntry(dataDirectory, journalPresent, systemError)) {
            return {
                .outcome = ExchangeRunIdReservationOutcome::DATA_DIRECTORY_IO_FAILURE,
                .catalogLoad = load,
                .systemError = systemError,
            };
        }
        if (journalPresent) {
            return {
                .outcome = ExchangeRunIdReservationOutcome::JOURNAL_PRESENT_WITHOUT_CATALOG,
                .catalogLoad = load,
            };
        }

        proposedRunId = domain::ExchangeRunId{1};
        next = {
            .generation = 1,
            .lastReservedRunId = proposedRunId,
            .activeRunId = std::nullopt,
            .activeDisposition = RunCatalogDisposition::NONE,
            .retainedStoppedRunId = std::nullopt,
        };
    } else {
        if (current.generation == std::numeric_limits<std::uint64_t>::max() ||
            (current.lastReservedRunId.has_value() &&
             current.lastReservedRunId->value() == std::numeric_limits<std::uint64_t>::max())) {
            return {
                .outcome = ExchangeRunIdReservationOutcome::EXHAUSTED,
                .catalogLoad = load,
            };
        }

        const std::uint64_t nextRunId =
            current.lastReservedRunId.has_value() ? current.lastReservedRunId->value() + 1 : 1;
        proposedRunId = domain::ExchangeRunId{nextRunId};
        next = current;
        ++next.generation;
        next.lastReservedRunId = proposedRunId;
    }

    RunCatalogReplaceResult replacement = replaceRunCatalogV1WithHooks(canonicalCatalogPath, next, storageHooks);
    if (replacement.outcome != RunCatalogReplaceOutcome::COMMITTED) {
        return {
            .outcome = ExchangeRunIdReservationOutcome::CATALOG_REPLACE_FAILED,
            .catalogLoad = load,
            .catalogReplacement = std::move(replacement),
        };
    }

    output = proposedRunId;
    return {
        .outcome = ExchangeRunIdReservationOutcome::RESERVED,
        .catalogLoad = load,
        .catalogReplacement = std::move(replacement),
    };
}

} // namespace detail

ExchangeRunIdReservationResult reserveNextExchangeRunId(const std::filesystem::path &canonicalCatalogPath,
                                                        domain::ExchangeRunId &output) noexcept {
    return detail::reserveNextExchangeRunIdWithHooks(canonicalCatalogPath, detail::systemRunCatalogStorageHooks(),
                                                     output);
}

} // namespace exchange::storage
