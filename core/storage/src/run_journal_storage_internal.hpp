#pragma once

#include "run_journal_storage.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <sys/types.h>

namespace exchange::storage::detail {

struct RunJournalStorageHooks final {
    void *context{nullptr};
    ssize_t (*writeFile)(void *context, int descriptor, const void *buffer, std::size_t size) noexcept {nullptr};
    int (*syncFile)(void *context, int descriptor) noexcept {nullptr};
    int (*closeFile)(void *context, int descriptor) noexcept {nullptr};
};

[[nodiscard]] RunJournalCreateResult createRunJournalV1WithHooks(const std::filesystem::path &canonicalPath,
                                                                 const RunHeaderV1 &header,
                                                                 const RunJournalStorageHooks &hooks) noexcept;

[[nodiscard]] RunJournalCreateResult createEncodedRunJournalV1AndRetainDescriptor(
    const std::filesystem::path &canonicalPath, std::span<const std::byte> encodedHeader,
    int &descriptorOutput) noexcept;

} // namespace exchange::storage::detail
