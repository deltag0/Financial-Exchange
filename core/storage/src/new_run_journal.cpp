#include "new_run_journal.hpp"

#include "new_run_journal_internal.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace exchange::storage {
namespace {

RunJournalCreateResult createWriter(void *, const std::filesystem::path &canonicalJournalPath,
                                    const RunHeaderV1 &header, std::unique_ptr<RunJournalWriterV1> &output) noexcept {
    return createRunJournalWriterV1(canonicalJournalPath, header, output);
}

} // namespace

namespace detail {

bool deriveCanonicalRunJournalPathV1(const std::filesystem::path &canonicalCatalogPath,
                                     const domain::ExchangeRunId exchangeRunId,
                                     std::filesystem::path &output) noexcept {
    try {
        std::filesystem::path derived =
            canonicalCatalogPath.parent_path() / ("run-" + std::to_string(exchangeRunId.value()) + ".fxjr");
        output = std::move(derived);
        return true;
    } catch (...) {
        return false;
    }
}

NewRunJournalPreparationResult prepareNewRunJournalV1WithHooks(const std::filesystem::path &canonicalCatalogPath,
                                                               const NewRunConfigurationV1 &configuration,
                                                               const NewRunJournalPreparationHooks &hooks,
                                                               PreparedNewRunJournalV1 &output) noexcept {
    RunHeaderV1 header;
    try {
        header = {
            .exchangeRunId = domain::ExchangeRunId{1},
            .behavioralRulesVersion = configuration.behavioralRulesVersion,
            .maxEventsPerCommand = configuration.maxEventsPerCommand,
            .maxRunCommands = configuration.maxRunCommands,
            .maxRunJournalBytes = configuration.maxRunJournalBytes,
            .instruments = configuration.instruments,
        };
    } catch (...) {
        return {
            .outcome = NewRunJournalPreparationOutcome::HEADER_VALIDATION_FAILED,
            .headerValidation = RunHeaderCodecError::ALLOCATION_FAILURE,
        };
    }

    std::vector<std::byte> validationFrame;
    const RunHeaderCodecError validation = encodeRunHeaderV1(header, validationFrame);
    if (validation != RunHeaderCodecError::NONE) {
        return {
            .outcome = NewRunJournalPreparationOutcome::HEADER_VALIDATION_FAILED,
            .headerValidation = validation,
        };
    }

    domain::ExchangeRunId reservedRunId{};
    ExchangeRunIdReservationResult reservation = reserveNextExchangeRunId(canonicalCatalogPath, reservedRunId);
    if (reservation.outcome != ExchangeRunIdReservationOutcome::RESERVED) {
        return {
            .outcome = NewRunJournalPreparationOutcome::RESERVATION_FAILED,
            .headerValidation = validation,
            .reservation = std::move(reservation),
        };
    }
    header.exchangeRunId = reservedRunId;

    std::filesystem::path canonicalJournalPath;
    if (!deriveCanonicalRunJournalPathV1(canonicalCatalogPath, reservedRunId, canonicalJournalPath)) {
        return {
            .outcome = NewRunJournalPreparationOutcome::JOURNAL_CREATION_FAILED,
            .headerValidation = validation,
            .reservation = std::move(reservation),
        };
    }

    std::unique_ptr<RunJournalWriterV1> writer;
    RunJournalCreateResult creation = hooks.createWriter(hooks.context, canonicalJournalPath, header, writer);
    if (creation.outcome != RunJournalCreateOutcome::CREATED) {
        return {
            .outcome = NewRunJournalPreparationOutcome::JOURNAL_CREATION_FAILED,
            .headerValidation = validation,
            .reservation = std::move(reservation),
            .journalCreation = creation,
        };
    }

    PreparedNewRunJournalV1 prepared{
        .header = std::move(header),
        .canonicalJournalPath = std::move(canonicalJournalPath),
        .writer = std::move(writer),
    };
    output = std::move(prepared);
    return {
        .outcome = NewRunJournalPreparationOutcome::PREPARED,
        .headerValidation = validation,
        .reservation = std::move(reservation),
        .journalCreation = creation,
    };
}

} // namespace detail

NewRunJournalPreparationResult prepareNewRunJournalV1(const std::filesystem::path &canonicalCatalogPath,
                                                      const NewRunConfigurationV1 &configuration,
                                                      PreparedNewRunJournalV1 &output) noexcept {
    const detail::NewRunJournalPreparationHooks hooks{nullptr, createWriter};
    return detail::prepareNewRunJournalV1WithHooks(canonicalCatalogPath, configuration, hooks, output);
}

} // namespace exchange::storage
