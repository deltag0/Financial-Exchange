#pragma once

#include "run_journal_reader.hpp"
#include "run_journal_writer.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace exchange::storage {

enum class RunJournalRecoveryOutcome : std::uint8_t {
    PREPARED,
    MISSING,
    IO_FAILURE,
    CORRUPTION,
    RECOVERY_REQUIRED,
};

enum class RunJournalRecoveryError : std::uint8_t {
    NONE,
    TAIL_PRESERVATION_FAILED,
    TRUNCATE_FAILED,
    SEEK_FAILED,
    SYNC_FAILED,
    CLOSE_FAILED,
};

struct RunJournalRecoveryResult final {
    RunJournalRecoveryOutcome outcome{RunJournalRecoveryOutcome::IO_FAILURE};
    RunJournalRecoveryError error{RunJournalRecoveryError::NONE};
    RunJournalLoadResult validation{};
    int systemError{0};
    std::vector<std::byte> preservedTailBytes{};
};

struct PreparedRunJournalV1 final {
    LoadedRunJournalV1 journal{};
    std::unique_ptr<RunJournalWriterV1> writer{};
};

// The caller guarantees exclusive recovery ownership for canonicalPath. Only PREPARED replaces output.
// If supplied, expectedRunId is checked on the loaded descriptor before any repair or writer transfer.
[[nodiscard]] RunJournalRecoveryResult prepareRunJournalV1(
    const std::filesystem::path &canonicalPath, PreparedRunJournalV1 &output,
    std::optional<domain::ExchangeRunId> expectedRunId = {}) noexcept;

} // namespace exchange::storage
