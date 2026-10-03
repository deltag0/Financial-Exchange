#pragma once

#include "exchange_run_id_reservation.hpp"
#include "run_journal_writer.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace exchange::storage {

struct NewRunConfigurationV1 final {
    std::uint32_t behavioralRulesVersion{0};
    std::uint32_t maxEventsPerCommand{0};
    std::uint64_t maxRunCommands{0};
    std::uint64_t maxRunJournalBytes{0};
    std::vector<RunHeaderInstrumentV1> instruments{};
};

struct PreparedNewRunJournalV1 final {
    RunHeaderV1 header{};
    std::filesystem::path canonicalJournalPath{};
    std::unique_ptr<RunJournalWriterV1> writer{};
};

enum class NewRunJournalPreparationOutcome : std::uint8_t {
    PREPARED,
    HEADER_VALIDATION_FAILED,
    RESERVATION_FAILED,
    JOURNAL_CREATION_FAILED,
};

struct NewRunJournalPreparationResult final {
    NewRunJournalPreparationOutcome outcome{NewRunJournalPreparationOutcome::HEADER_VALIDATION_FAILED};
    RunHeaderCodecError headerValidation{RunHeaderCodecError::NONE};
    std::optional<ExchangeRunIdReservationResult> reservation{};
    std::optional<RunJournalCreateResult> journalCreation{};
};

// Only PREPARED replaces output. A committed reservation is never rolled back after a later failure.
[[nodiscard]] NewRunJournalPreparationResult prepareNewRunJournalV1(const std::filesystem::path &canonicalCatalogPath,
                                                                    const NewRunConfigurationV1 &configuration,
                                                                    PreparedNewRunJournalV1 &output) noexcept;

} // namespace exchange::storage
