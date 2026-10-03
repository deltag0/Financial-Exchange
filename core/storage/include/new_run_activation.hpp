#pragma once

#include "new_run_journal.hpp"
#include "run_catalog_storage.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>

namespace exchange::storage {

enum class NewRunActivationOutcome : std::uint8_t {
    ACTIVATED,
    MISSING_WRITER,
    ZERO_EXCHANGE_RUN_ID,
    NONCANONICAL_JOURNAL_PATH,
    CATALOG_LOAD_FAILED,
    LAST_RESERVED_RUN_ID_MISMATCH,
    ACTIVE_RUN_EXISTS,
    GENERATION_EXHAUSTED,
    CATALOG_REPLACE_FAILED,
};

struct NewRunActivationResult final {
    NewRunActivationOutcome outcome{NewRunActivationOutcome::MISSING_WRITER};
    std::optional<RunCatalogLoadResult> catalogLoad{};
    std::optional<RunCatalogReplaceResult> catalogReplacement{};
};

// Activation never consumes or mutates the prepared journal. Only a committed catalog replacement reports success.
[[nodiscard]] NewRunActivationResult activatePreparedNewRunV1(const std::filesystem::path &canonicalCatalogPath,
                                                              const PreparedNewRunJournalV1 &prepared) noexcept;

} // namespace exchange::storage
