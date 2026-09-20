#pragma once

#include "run_catalog_storage.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>

namespace exchange::storage {

enum class ExchangeRunIdReservationOutcome : std::uint8_t {
    RESERVED,
    CATALOG_LOAD_FAILED,
    JOURNAL_PRESENT_WITHOUT_CATALOG,
    DATA_DIRECTORY_IO_FAILURE,
    EXHAUSTED,
    CATALOG_REPLACE_FAILED,
};

struct ExchangeRunIdReservationResult final {
    ExchangeRunIdReservationOutcome outcome{ExchangeRunIdReservationOutcome::CATALOG_LOAD_FAILED};
    RunCatalogLoadResult catalogLoad{};
    std::optional<RunCatalogReplaceResult> catalogReplacement{};
    int systemError{0};
};

// Success replaces output only after the catalog replacement is durably
// committed.
[[nodiscard]] ExchangeRunIdReservationResult reserveNextExchangeRunId(const std::filesystem::path &canonicalCatalogPath,
                                                                      domain::ExchangeRunId &output) noexcept;

} // namespace exchange::storage
