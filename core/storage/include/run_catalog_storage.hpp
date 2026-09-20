#pragma once

#include "run_catalog_codec.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>

namespace exchange::storage {

enum class RunCatalogLoadOutcome : std::uint8_t {
    LOADED,
    MISSING,
    INVALID,
    IO_FAILURE,
};

struct RunCatalogLoadResult {
    RunCatalogLoadOutcome outcome{RunCatalogLoadOutcome::IO_FAILURE};
    RunCatalogCodecError codecError{RunCatalogCodecError::NONE};
    int systemError{0};
};

enum class RunCatalogReplaceOutcome : std::uint8_t {
    COMMITTED,
    INVALID_SNAPSHOT,
    INVALID_EXISTING_CATALOG,
    GENERATION_CONFLICT,
    NOT_COMMITTED_IO_FAILURE,
    UNCERTAIN,
};

struct RunCatalogReplaceResult {
    RunCatalogReplaceOutcome outcome{RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE};
    RunCatalogCodecError codecError{RunCatalogCodecError::NONE};
    int systemError{0};
    std::optional<RunCatalogSnapshotV1> observedSnapshot{};
};

[[nodiscard]] RunCatalogLoadResult loadRunCatalogV1(const std::filesystem::path& canonicalPath,
                                                    RunCatalogSnapshotV1& output) noexcept;

[[nodiscard]] RunCatalogReplaceResult replaceRunCatalogV1(const std::filesystem::path& canonicalPath,
                                                          const RunCatalogSnapshotV1& nextSnapshot) noexcept;

} // namespace exchange::storage
