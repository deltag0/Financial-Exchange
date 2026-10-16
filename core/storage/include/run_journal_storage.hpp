#pragma once

#include "journal_run_header_codec.hpp"

#include <cstdint>
#include <filesystem>

namespace exchange::storage {

enum class RunJournalCreateOutcome : std::uint8_t {
    CREATED,
    INVALID_HEADER,
    PATH_ALREADY_EXISTS,
    IO_FAILURE,
    UNCERTAIN,
};

struct RunJournalCreateResult final {
    RunJournalCreateOutcome outcome{RunJournalCreateOutcome::IO_FAILURE};
    RunHeaderCodecError codecError{RunHeaderCodecError::NONE};
    int systemError{0};
};

[[nodiscard]] RunJournalCreateResult createRunJournalV1(const std::filesystem::path &canonicalPath,
                                                        const RunHeaderV1 &header) noexcept;

} // namespace exchange::storage
