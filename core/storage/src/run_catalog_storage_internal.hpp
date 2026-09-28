#pragma once

#include "run_catalog_storage.hpp"

#include <cstddef>
#include <filesystem>
#include <sys/types.h>

namespace exchange::storage::detail {

struct RunCatalogStorageHooks {
    void* context{nullptr};
    ssize_t (*writeFile)(void* context, int descriptor, const void* buffer, std::size_t size) noexcept {nullptr};
    int (*syncFile)(void* context, int descriptor) noexcept {nullptr};
    int (*replaceFile)(void* context, const char* source, const char* destination) noexcept {nullptr};
    int (*removeFile)(void* context, const char* path) noexcept {nullptr};
};

[[nodiscard]] const RunCatalogStorageHooks& systemRunCatalogStorageHooks() noexcept;

[[nodiscard]] RunCatalogReplaceResult replaceRunCatalogV1WithHooks(const std::filesystem::path& canonicalPath,
                                                                   const RunCatalogSnapshotV1& nextSnapshot,
                                                                   const RunCatalogStorageHooks& hooks) noexcept;

} // namespace exchange::storage::detail
