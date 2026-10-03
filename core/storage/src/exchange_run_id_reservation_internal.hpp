#pragma once

#include "exchange_run_id_reservation.hpp"
#include "run_catalog_storage_internal.hpp"

namespace exchange::storage::detail {

[[nodiscard]] ExchangeRunIdReservationResult reserveNextExchangeRunIdWithHooks(
    const std::filesystem::path &canonicalCatalogPath, const RunCatalogStorageHooks &storageHooks,
    domain::ExchangeRunId &output) noexcept;

} // namespace exchange::storage::detail
