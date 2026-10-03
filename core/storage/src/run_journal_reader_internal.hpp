#pragma once

#include "run_journal_reader.hpp"

#include <cstddef>
#include <filesystem>
#include <sys/types.h>

namespace exchange::storage::detail {

struct RunJournalReaderHooks final {
    void *context{nullptr};
    ssize_t (*readFile)(void *context, int descriptor, void *buffer, std::size_t size) noexcept {nullptr};
};

// The caller owns descriptor lifetime and must position it at the journal start.
[[nodiscard]] RunJournalLoadResult loadRunJournalV1FromDescriptorWithHooks(int descriptor,
                                                                           const RunJournalReaderHooks &hooks,
                                                                           LoadedRunJournalV1 &output) noexcept;

[[nodiscard]] RunJournalLoadResult loadRunJournalV1WithHooks(const std::filesystem::path &canonicalPath,
                                                             const RunJournalReaderHooks &hooks,
                                                             LoadedRunJournalV1 &output) noexcept;

} // namespace exchange::storage::detail
