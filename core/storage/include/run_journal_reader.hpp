#pragma once

#include "journal_command_codec.hpp"
#include "journal_run_header_codec.hpp"

#include <cstdint>
#include <filesystem>
#include <variant>
#include <vector>

namespace exchange::storage {

using JournalCommandV1 = std::variant<JournalNewOrderV1, JournalCancelV1>;

struct LoadedRunJournalV1 final {
    RunHeaderV1 header{};
    std::vector<JournalCommandV1> commands{};
    std::uint64_t validCommittedByteCount{0};

    bool operator==(const LoadedRunJournalV1 &) const = default;
};

enum class RunJournalLoadOutcome : std::uint8_t {
    VALID,
    MISSING,
    IO_FAILURE,
    CORRUPTION,
    INCOMPLETE_TAIL,
};

enum class RunJournalLoadError : std::uint8_t {
    NONE,
    FILE_BYTE_CAPACITY_EXCEEDED,
    COMMAND_COUNT_CAPACITY_EXCEEDED,
    EXCHANGE_RUN_ID_MISMATCH,
    INSTRUMENT_CONFIGURATION_MISMATCH,
    COMMAND_SEQUENCE_MISMATCH,
};

struct RunJournalLoadResult final {
    RunJournalLoadOutcome outcome{RunJournalLoadOutcome::IO_FAILURE};
    RunJournalLoadError error{RunJournalLoadError::NONE};
    RunHeaderCodecError headerCodecError{RunHeaderCodecError::NONE};
    JournalCommandCodecError commandCodecError{JournalCommandCodecError::NONE};
    int systemError{0};
    std::uint64_t tailOffset{0};
    std::uint64_t tailLength{0};
};

// VALID and INCOMPLETE_TAIL replace output. All other outcomes leave it unchanged.
[[nodiscard]] RunJournalLoadResult loadRunJournalV1(const std::filesystem::path &canonicalPath,
                                                    LoadedRunJournalV1 &output) noexcept;

} // namespace exchange::storage
