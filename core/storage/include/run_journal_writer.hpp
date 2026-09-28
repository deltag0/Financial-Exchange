#pragma once

#include "journal_command_codec.hpp"
#include "journal_run_header_codec.hpp"
#include "run_journal_storage.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sys/types.h>
#include <vector>

namespace exchange::storage {

class RunJournalWriterV1;

namespace detail {
struct RunJournalWriterHooks;
[[nodiscard]] std::unique_ptr<RunJournalWriterV1> makeRecoveredRunJournalWriterV1(
    int descriptor, RunHeaderV1 header, std::uint64_t committedCommandCount, std::uint64_t committedByteCount,
    domain::CommandSequence nextCommandSequence, const RunJournalWriterHooks &hooks) noexcept;
} // namespace detail

enum class RunJournalAppendOutcome : std::uint8_t {
    COMMITTED,
    INVALID_COMMAND,
    CAPACITY_REACHED,
    IO_FAILURE,
    RECOVERY_REQUIRED,
};

enum class RunJournalAppendError : std::uint8_t {
    NONE,
    WRITER_UNUSABLE,
    UNEXPECTED_COMMAND_SEQUENCE,
    EXCHANGE_RUN_ID_MISMATCH,
    BEHAVIORAL_RULES_VERSION_MISMATCH,
    INSTRUMENT_CONFIGURATION_MISMATCH,
};

struct RunJournalAppendResult final {
    RunJournalAppendOutcome outcome{RunJournalAppendOutcome::INVALID_COMMAND};
    RunJournalAppendError error{RunJournalAppendError::NONE};
    JournalCommandCodecError codecError{JournalCommandCodecError::NONE};
    int systemError{0};
};

namespace detail {
[[nodiscard]] RunJournalCreateResult createRunJournalWriterV1WithHooks(
    const std::filesystem::path &canonicalPath, const RunHeaderV1 &header, const RunJournalWriterHooks &hooks,
    std::unique_ptr<RunJournalWriterV1> &output) noexcept;
}

// One owner serializes appends through one exclusively created or successfully recovered descriptor.
class RunJournalWriterV1 final {
public:
    ~RunJournalWriterV1();

    RunJournalWriterV1(const RunJournalWriterV1 &) = delete;
    RunJournalWriterV1 &operator=(const RunJournalWriterV1 &) = delete;
    RunJournalWriterV1(RunJournalWriterV1 &&) = delete;
    RunJournalWriterV1 &operator=(RunJournalWriterV1 &&) = delete;

    [[nodiscard]] RunJournalAppendResult append(const JournalNewOrderV1 &command) noexcept;
    [[nodiscard]] RunJournalAppendResult append(const JournalCancelV1 &command) noexcept;

    // Validates context and encodes with the authoritative codec without checking capacity or writing.
    [[nodiscard]] std::optional<std::size_t> prospectiveFrameSize(const JournalNewOrderV1 &command) const noexcept;
    [[nodiscard]] std::optional<std::size_t> prospectiveFrameSize(const JournalCancelV1 &command) const noexcept;

    [[nodiscard]] std::uint64_t committedCommandCount() const noexcept;
    [[nodiscard]] std::uint64_t committedByteCount() const noexcept;
    [[nodiscard]] domain::CommandSequence nextCommandSequence() const noexcept;
    [[nodiscard]] const RunHeaderV1 &header() const noexcept;

private:
    RunJournalWriterV1(RunHeaderV1 header, std::uint64_t headerByteCount,
                       const detail::RunJournalWriterHooks &hooks) noexcept;
    RunJournalWriterV1(int descriptor, RunHeaderV1 header, std::uint64_t committedCommandCount,
                       std::uint64_t committedByteCount, domain::CommandSequence nextCommandSequence,
                       const detail::RunJournalWriterHooks &hooks) noexcept;

    [[nodiscard]] RunJournalAppendError validateCommandContext(domain::ExchangeRunId exchangeRunId,
                                                               domain::CommandSequence commandSequence,
                                                               std::uint32_t behavioralRulesVersion,
                                                               domain::InstrumentId instrumentId,
                                                               std::uint32_t configurationVersion) const noexcept;
    [[nodiscard]] RunJournalAppendResult appendEncoded(const std::vector<std::byte> &frame) noexcept;

    friend RunJournalCreateResult detail::createRunJournalWriterV1WithHooks(
        const std::filesystem::path &, const RunHeaderV1 &, const detail::RunJournalWriterHooks &,
        std::unique_ptr<RunJournalWriterV1> &) noexcept;
    friend std::unique_ptr<RunJournalWriterV1> detail::makeRecoveredRunJournalWriterV1(
        int, RunHeaderV1, std::uint64_t, std::uint64_t, domain::CommandSequence,
        const detail::RunJournalWriterHooks &) noexcept;

    int descriptor_{-1};
    RunHeaderV1 header_;
    std::uint64_t committedCommandCount_{0};
    std::uint64_t committedByteCount_{0};
    domain::CommandSequence nextCommandSequence_{1};
    bool usable_{true};
    void *hookContext_{nullptr};
    ssize_t (*writeFile_)(void *context, int descriptor, const void *buffer, std::size_t size) noexcept {nullptr};
    int (*syncFile_)(void *context, int descriptor) noexcept {nullptr};
};

[[nodiscard]] RunJournalCreateResult createRunJournalWriterV1(const std::filesystem::path &canonicalPath,
                                                              const RunHeaderV1 &header,
                                                              std::unique_ptr<RunJournalWriterV1> &output) noexcept;

} // namespace exchange::storage
