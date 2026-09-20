#pragma once

#include "new_run_activation.hpp"
#include "run_catalog_storage_internal.hpp"

namespace exchange::storage::detail {

[[nodiscard]] NewRunActivationResult activatePreparedNewRunV1WithHooks(
    const std::filesystem::path &canonicalCatalogPath, const PreparedNewRunJournalV1 &prepared,
    const RunCatalogStorageHooks &storageHooks) noexcept;

} // namespace exchange::storage::detail
