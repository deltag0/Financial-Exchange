#pragma once

#include "domain_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace exchange::storage {

enum class RunCatalogDisposition : std::uint8_t {
    NONE = 0,
    OPEN = 1,
    PAUSED = 2,
    CAPACITY_REACHED = 3,
    FAIL_STOPPED = 4,
    RECOVERY_FAILED = 5,
};

struct RunCatalogSnapshotV1 final {
    std::uint64_t generation{};
    std::optional<domain::ExchangeRunId> lastReservedRunId;
    std::optional<domain::ExchangeRunId> activeRunId;
    RunCatalogDisposition activeDisposition{RunCatalogDisposition::NONE};
    std::optional<domain::ExchangeRunId> retainedStoppedRunId;

    bool operator==(const RunCatalogSnapshotV1&) const = default;
};

enum class RunCatalogCodecError : std::uint8_t {
    NONE = 0,
    INVALID_SIZE,
    INVALID_MAGIC,
    UNSUPPORTED_VERSION,
    INVALID_TOTAL_LENGTH,
    NONZERO_RESERVED_BYTES,
    CHECKSUM_MISMATCH,
    ZERO_GENERATION,
    UNKNOWN_DISPOSITION,
    ZERO_PRESENT_RUN_ID,
    ACTIVE_DISPOSITION_MISMATCH,
    RUN_ID_EXCEEDS_LAST_RESERVED,
    DUPLICATE_ACTIVE_AND_RETAINED_RUN_ID,
};

inline constexpr std::size_t RUN_CATALOG_V1_SIZE = 56;
using RunCatalogV1Bytes = std::array<std::byte, RUN_CATALOG_V1_SIZE>;

[[nodiscard]] RunCatalogCodecError encodeRunCatalogV1(const RunCatalogSnapshotV1& snapshot,
                                                      RunCatalogV1Bytes& output) noexcept;

[[nodiscard]] RunCatalogCodecError decodeRunCatalogV1(std::span<const std::byte> input,
                                                      RunCatalogSnapshotV1& output) noexcept;

} // namespace exchange::storage
