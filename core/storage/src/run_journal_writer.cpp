#include "run_journal_writer.hpp"

#include "run_journal_storage_internal.hpp"
#include "run_journal_writer_internal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <new>
#include <unistd.h>
#include <utility>
#include <vector>

namespace exchange::storage {
namespace {

ssize_t systemWriteFile(void *, const int descriptor, const void *buffer, const std::size_t size) noexcept {
    return ::write(descriptor, buffer, size);
}

int systemSyncFile(void *, const int descriptor) noexcept {
    return ::fdatasync(descriptor);
}

const detail::RunJournalWriterHooks SYSTEM_HOOKS{
    nullptr,
    systemWriteFile,
    systemSyncFile,
};

RunJournalAppendResult appendResult(const RunJournalAppendOutcome outcome,
                                    const RunJournalAppendError error = RunJournalAppendError::NONE,
                                    const JournalCommandCodecError codecError = JournalCommandCodecError::NONE,
                                    const int systemError = 0) noexcept {
    return {outcome, error, codecError, systemError};
}

} // namespace

RunJournalWriterV1::RunJournalWriterV1(RunHeaderV1 header, const std::uint64_t headerByteCount,
                                       const detail::RunJournalWriterHooks &hooks) noexcept
    : header_(std::move(header)),
      committedByteCount_(headerByteCount),
      hookContext_(hooks.context),
      writeFile_(hooks.writeFile),
      syncFile_(hooks.syncFile) {}

RunJournalWriterV1::RunJournalWriterV1(const int descriptor, RunHeaderV1 header,
                                       const std::uint64_t committedCommandCount,
                                       const std::uint64_t committedByteCount,
                                       const domain::CommandSequence nextCommandSequence,
                                       const detail::RunJournalWriterHooks &hooks) noexcept
    : descriptor_(descriptor),
      header_(std::move(header)),
      committedCommandCount_(committedCommandCount),
      committedByteCount_(committedByteCount),
      nextCommandSequence_(nextCommandSequence),
      hookContext_(hooks.context),
      writeFile_(hooks.writeFile),
      syncFile_(hooks.syncFile) {}

RunJournalWriterV1::~RunJournalWriterV1() {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
    }
}

RunJournalAppendResult RunJournalWriterV1::append(const JournalNewOrderV1 &command) noexcept {
    if (!usable_) {
        return appendResult(RunJournalAppendOutcome::RECOVERY_REQUIRED, RunJournalAppendError::WRITER_UNUSABLE,
                            JournalCommandCodecError::NONE, 0);
    }

    const auto contextError =
        validateCommandContext(command.exchangeRunId, command.commandSequence, command.behavioralRulesVersion,
                               command.instrumentId, command.configurationVersion);
    if (contextError != RunJournalAppendError::NONE) {
        return appendResult(RunJournalAppendOutcome::INVALID_COMMAND, contextError);
    }

    std::vector<std::byte> frame;
    const auto codecError = encodeNewOrderV1(command, frame);
    if (codecError != JournalCommandCodecError::NONE) {
        if (codecError == JournalCommandCodecError::ALLOCATION_FAILURE) {
            return appendResult(RunJournalAppendOutcome::IO_FAILURE, RunJournalAppendError::NONE, codecError, ENOMEM);
        }
        return appendResult(RunJournalAppendOutcome::INVALID_COMMAND, RunJournalAppendError::NONE, codecError);
    }
    return appendEncoded(frame);
}

RunJournalAppendResult RunJournalWriterV1::append(const JournalCancelV1 &command) noexcept {
    if (!usable_) {
        return appendResult(RunJournalAppendOutcome::RECOVERY_REQUIRED, RunJournalAppendError::WRITER_UNUSABLE,
                            JournalCommandCodecError::NONE, 0);
    }

    const auto contextError =
        validateCommandContext(command.exchangeRunId, command.commandSequence, command.behavioralRulesVersion,
                               command.instrumentId, command.configurationVersion);
    if (contextError != RunJournalAppendError::NONE) {
        return appendResult(RunJournalAppendOutcome::INVALID_COMMAND, contextError);
    }

    std::vector<std::byte> frame;
    const auto codecError = encodeCancelV1(command, frame);
    if (codecError != JournalCommandCodecError::NONE) {
        if (codecError == JournalCommandCodecError::ALLOCATION_FAILURE) {
            return appendResult(RunJournalAppendOutcome::IO_FAILURE, RunJournalAppendError::NONE, codecError, ENOMEM);
        }
        return appendResult(RunJournalAppendOutcome::INVALID_COMMAND, RunJournalAppendError::NONE, codecError);
    }
    return appendEncoded(frame);
}

std::optional<std::size_t> RunJournalWriterV1::prospectiveFrameSize(const JournalNewOrderV1 &command) const noexcept {
    if (!usable_ ||
        validateCommandContext(command.exchangeRunId, command.commandSequence, command.behavioralRulesVersion,
                               command.instrumentId, command.configurationVersion) != RunJournalAppendError::NONE) {
        return std::nullopt;
    }
    std::vector<std::byte> frame;
    if (encodeNewOrderV1(command, frame) != JournalCommandCodecError::NONE) {
        return std::nullopt;
    }
    return frame.size();
}

std::optional<std::size_t> RunJournalWriterV1::prospectiveFrameSize(const JournalCancelV1 &command) const noexcept {
    if (!usable_ ||
        validateCommandContext(command.exchangeRunId, command.commandSequence, command.behavioralRulesVersion,
                               command.instrumentId, command.configurationVersion) != RunJournalAppendError::NONE) {
        return std::nullopt;
    }
    std::vector<std::byte> frame;
    if (encodeCancelV1(command, frame) != JournalCommandCodecError::NONE) {
        return std::nullopt;
    }
    return frame.size();
}

RunJournalAppendError RunJournalWriterV1::validateCommandContext(
    const domain::ExchangeRunId exchangeRunId, const domain::CommandSequence commandSequence,
    const std::uint32_t behavioralRulesVersion, const domain::InstrumentId instrumentId,
    const std::uint32_t configurationVersion) const noexcept {
    if (commandSequence != nextCommandSequence_) {
        return RunJournalAppendError::UNEXPECTED_COMMAND_SEQUENCE;
    }
    if (exchangeRunId != header_.exchangeRunId) {
        return RunJournalAppendError::EXCHANGE_RUN_ID_MISMATCH;
    }
    if (behavioralRulesVersion != header_.behavioralRulesVersion) {
        return RunJournalAppendError::BEHAVIORAL_RULES_VERSION_MISMATCH;
    }
    const auto instrument = std::lower_bound(
        header_.instruments.begin(), header_.instruments.end(), instrumentId,
        [](const RunHeaderInstrumentV1 &entry, const domain::InstrumentId id) { return entry.instrumentId < id; });
    if (instrument == header_.instruments.end() || instrument->instrumentId != instrumentId ||
        instrument->configurationVersion != configurationVersion) {
        return RunJournalAppendError::INSTRUMENT_CONFIGURATION_MISMATCH;
    }
    return RunJournalAppendError::NONE;
}

RunJournalAppendResult RunJournalWriterV1::appendEncoded(const std::vector<std::byte> &frame) noexcept {
    if (committedCommandCount_ >= header_.maxRunCommands) {
        return appendResult(RunJournalAppendOutcome::CAPACITY_REACHED);
    }
    if (frame.size() > header_.maxRunJournalBytes - committedByteCount_) {
        return appendResult(RunJournalAppendOutcome::CAPACITY_REACHED);
    }

    std::size_t bytesWritten = 0;
    while (bytesWritten < frame.size()) {
        const std::size_t remaining = frame.size() - bytesWritten;
        const ssize_t result = writeFile_(hookContext_, descriptor_, frame.data() + bytesWritten, remaining);
        if (result > 0 && static_cast<std::size_t>(result) <= remaining) {
            bytesWritten += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }

        const int systemError = result == 0 || result > 0 ? EIO : errno;
        usable_ = false;
        const auto outcome =
            bytesWritten == 0 ? RunJournalAppendOutcome::IO_FAILURE : RunJournalAppendOutcome::RECOVERY_REQUIRED;
        return appendResult(outcome, RunJournalAppendError::NONE, JournalCommandCodecError::NONE, systemError);
    }

    int syncResult = -1;
    do {
        syncResult = syncFile_(hookContext_, descriptor_);
    } while (syncResult < 0 && errno == EINTR);
    if (syncResult != 0) {
        const int systemError = syncResult < 0 ? errno : EIO;
        usable_ = false;
        return appendResult(RunJournalAppendOutcome::RECOVERY_REQUIRED, RunJournalAppendError::NONE,
                            JournalCommandCodecError::NONE, systemError);
    }

    ++committedCommandCount_;
    committedByteCount_ += static_cast<std::uint64_t>(frame.size());
    nextCommandSequence_ = domain::CommandSequence{nextCommandSequence_.value() + 1};
    return appendResult(RunJournalAppendOutcome::COMMITTED);
}

std::uint64_t RunJournalWriterV1::committedCommandCount() const noexcept {
    return committedCommandCount_;
}

std::uint64_t RunJournalWriterV1::committedByteCount() const noexcept {
    return committedByteCount_;
}

domain::CommandSequence RunJournalWriterV1::nextCommandSequence() const noexcept {
    return nextCommandSequence_;
}

const RunHeaderV1 &RunJournalWriterV1::header() const noexcept {
    return header_;
}

namespace detail {

const RunJournalWriterHooks &systemRunJournalWriterHooks() noexcept {
    return SYSTEM_HOOKS;
}

std::unique_ptr<RunJournalWriterV1> makeRecoveredRunJournalWriterV1(const int descriptor, RunHeaderV1 header,
                                                                    const std::uint64_t committedCommandCount,
                                                                    const std::uint64_t committedByteCount,
                                                                    const domain::CommandSequence nextCommandSequence,
                                                                    const RunJournalWriterHooks &hooks) noexcept {
    try {
        return std::unique_ptr<RunJournalWriterV1>(new RunJournalWriterV1(
            descriptor, std::move(header), committedCommandCount, committedByteCount, nextCommandSequence, hooks));
    } catch (const std::bad_alloc &) {
        return nullptr;
    }
}

RunJournalCreateResult createRunJournalWriterV1WithHooks(const std::filesystem::path &canonicalPath,
                                                         const RunHeaderV1 &header, const RunJournalWriterHooks &hooks,
                                                         std::unique_ptr<RunJournalWriterV1> &output) noexcept {
    std::vector<std::byte> encodedHeader;
    const auto codecError = encodeRunHeaderV1(header, encodedHeader);
    if (codecError != RunHeaderCodecError::NONE) {
        if (codecError == RunHeaderCodecError::ALLOCATION_FAILURE) {
            return {RunJournalCreateOutcome::IO_FAILURE, codecError, ENOMEM};
        }
        return {RunJournalCreateOutcome::INVALID_HEADER, codecError, 0};
    }

    std::unique_ptr<RunJournalWriterV1> writer;
    try {
        writer = std::unique_ptr<RunJournalWriterV1>(
            new RunJournalWriterV1(header, static_cast<std::uint64_t>(encodedHeader.size()), hooks));
    } catch (const std::bad_alloc &) {
        return {RunJournalCreateOutcome::IO_FAILURE, RunHeaderCodecError::NONE, ENOMEM};
    }

    int descriptor = -1;
    const auto createResult = createEncodedRunJournalV1AndRetainDescriptor(canonicalPath, encodedHeader, descriptor);
    if (createResult.outcome != RunJournalCreateOutcome::CREATED) {
        return createResult;
    }

    writer->descriptor_ = descriptor;
    output = std::move(writer);
    return createResult;
}

} // namespace detail

RunJournalCreateResult createRunJournalWriterV1(const std::filesystem::path &canonicalPath, const RunHeaderV1 &header,
                                                std::unique_ptr<RunJournalWriterV1> &output) noexcept {
    return detail::createRunJournalWriterV1WithHooks(canonicalPath, header, SYSTEM_HOOKS, output);
}

} // namespace exchange::storage
