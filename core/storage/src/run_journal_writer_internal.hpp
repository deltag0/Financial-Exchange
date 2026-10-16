#pragma once

#include "run_journal_writer.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <sys/types.h>

namespace exchange::storage::detail {

struct RunJournalWriterHooks final {
    void *context{nullptr};
    ssize_t (*writeFile)(void *context, int descriptor, const void *buffer, std::size_t size) noexcept {nullptr};
    int (*syncFile)(void *context, int descriptor) noexcept {nullptr};
};

[[nodiscard]] const RunJournalWriterHooks &systemRunJournalWriterHooks() noexcept;

[[nodiscard]] RunJournalCreateResult createRunJournalWriterV1WithHooks(
    const std::filesystem::path &canonicalPath, const RunHeaderV1 &header, const RunJournalWriterHooks &hooks,
    std::unique_ptr<RunJournalWriterV1> &output) noexcept;

} // namespace exchange::storage::detail
