#pragma once

#include "command_admission.hpp"
#include "matching_state.hpp"
#include "run_journal_writer.hpp"

#include <memory>

namespace exchange::core {

// Owns the mutable in-process state and append authority for one exchange run.
struct RunStateV1 final {
    std::unique_ptr<storage::RunJournalWriterV1> journalWriter{};
    std::unique_ptr<matching_engine::MatchingState> matchingState{};
    std::unique_ptr<admission::CommandAdmissionIndex> admissionIndex{};
};

} // namespace exchange::core
