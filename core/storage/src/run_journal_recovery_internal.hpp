#pragma once

#include "run_journal_recovery.hpp"
#include "run_journal_writer_internal.hpp"

#include <cstddef>
#include <filesystem>
#include <sys/types.h>

namespace exchange::storage::detail {

struct RunJournalRecoveryHooks final {
    void *context{nullptr};
    ssize_t (*readFile)(void *context, int descriptor, void *buffer, std::size_t size) noexcept {nullptr};
    off_t (*seekFile)(void *context, int descriptor, off_t offset, int whence) noexcept {nullptr};
    int (*truncateFile)(void *context, int descriptor, off_t length) noexcept {nullptr};
    int (*syncFile)(void *context, int descriptor) noexcept {nullptr};
    int (*closeFile)(void *context, int descriptor) noexcept {nullptr};
};

[[nodiscard]] RunJournalRecoveryResult prepareRunJournalV1WithHooks(
    const std::filesystem::path &canonicalPath, const RunJournalRecoveryHooks &recoveryHooks,
    const RunJournalWriterHooks &writerHooks, PreparedRunJournalV1 &output,
    std::optional<domain::ExchangeRunId> expectedRunId = {}) noexcept;

} // namespace exchange::storage::detail
