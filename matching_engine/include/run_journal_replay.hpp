#pragma once

#include "matching_state.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace exchange::storage {
struct PreparedRunJournalV1;
}

namespace exchange::matching_engine {

enum class RunJournalReplayOutcome : std::uint8_t {
    REPLAYED,
    INVALID_JOURNAL,
    INVARIANT_FAILURE,
    INTERNAL_FAILURE,
};

enum class RunJournalReplayError : std::uint8_t {
    NONE,
    HEADER_CONTEXT_MISMATCH,
    COMMAND_COUNT_EXCEEDED,
    COMMAND_SEQUENCE_MISMATCH,
    EXCHANGE_RUN_ID_MISMATCH,
    BEHAVIORAL_RULES_VERSION_MISMATCH,
    INSTRUMENT_CONFIGURATION_MISMATCH,
    MATCHING_INVARIANT_FAILURE,
};

struct RunJournalReplayResult final {
    RunJournalReplayOutcome outcome{RunJournalReplayOutcome::INTERNAL_FAILURE};
    RunJournalReplayError error{RunJournalReplayError::NONE};
};

struct ReplayedRunJournalV1 final {
    std::vector<ImmutableCommandResultBatch> commandResults{};
    std::unique_ptr<MatchingState> matchingState{};
};

// Replays only the validated in-memory journal. Success replaces output; every failure leaves it unchanged.
[[nodiscard]] RunJournalReplayResult replayPreparedRunJournalV1(const storage::PreparedRunJournalV1 &prepared,
                                                                ReplayedRunJournalV1 &output) noexcept;

} // namespace exchange::matching_engine
