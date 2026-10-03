#include "run_journal_recovery.hpp"

#include "run_journal_reader_internal.hpp"
#include "run_journal_recovery_internal.hpp"
#include "run_journal_writer_internal.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <new>
#include <span>
#include <unistd.h>
#include <utility>

namespace exchange::storage {
namespace {

ssize_t systemReadFile(void *, const int descriptor, void *buffer, const std::size_t size) noexcept {
    return ::read(descriptor, buffer, size);
}

off_t systemSeekFile(void *, const int descriptor, const off_t offset, const int whence) noexcept {
    return ::lseek(descriptor, offset, whence);
}

int systemTruncateFile(void *, const int descriptor, const off_t length) noexcept {
    return ::ftruncate(descriptor, length);
}

int systemSyncFile(void *, const int descriptor) noexcept {
    return ::fsync(descriptor);
}

int systemCloseFile(void *, const int descriptor) noexcept {
    return ::close(descriptor);
}

const detail::RunJournalRecoveryHooks SYSTEM_HOOKS{
    nullptr, systemReadFile, systemSeekFile, systemTruncateFile, systemSyncFile, systemCloseFile,
};

RunJournalRecoveryResult recoveryResult(const RunJournalRecoveryOutcome outcome, const RunJournalRecoveryError error,
                                        const RunJournalLoadResult &validation, const int systemError,
                                        std::vector<std::byte> preservedTailBytes = {}) noexcept {
    return {outcome, error, validation, systemError, std::move(preservedTailBytes)};
}

int openReadWrite(const std::filesystem::path &path) noexcept {
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

off_t seekDescriptor(const int descriptor, const off_t offset, const detail::RunJournalRecoveryHooks &hooks,
                     int &systemError) noexcept {
    off_t result = -1;
    do {
        result = hooks.seekFile(hooks.context, descriptor, offset, SEEK_SET);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        systemError = errno;
    } else if (result != offset) {
        systemError = EIO;
        result = -1;
    }
    return result;
}

bool readExact(const int descriptor, const std::span<std::byte> destination,
               const detail::RunJournalRecoveryHooks &hooks, int &systemError) noexcept {
    std::size_t bytesRead = 0;
    while (bytesRead < destination.size()) {
        const ssize_t result =
            hooks.readFile(hooks.context, descriptor, destination.data() + bytesRead, destination.size() - bytesRead);
        if (result > 0 && static_cast<std::size_t>(result) <= destination.size() - bytesRead) {
            bytesRead += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        systemError = result == 0 || result > 0 ? EIO : errno;
        return false;
    }
    return true;
}

bool syncDescriptor(const int descriptor, const detail::RunJournalRecoveryHooks &hooks, int &systemError) noexcept {
    int result = -1;
    do {
        result = hooks.syncFile(hooks.context, descriptor);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
        return true;
    }
    systemError = result < 0 ? errno : EIO;
    return false;
}

RunJournalRecoveryResult closeAfterFailure(const int descriptor, const detail::RunJournalRecoveryHooks &hooks,
                                           const bool truncationBegan, const RunJournalRecoveryError error,
                                           const RunJournalLoadResult &validation, const int systemError,
                                           std::vector<std::byte> preservedTailBytes = {}) noexcept {
    if (hooks.closeFile(hooks.context, descriptor) != 0) {
        const int closeError = errno;
        return recoveryResult(
            truncationBegan ? RunJournalRecoveryOutcome::RECOVERY_REQUIRED : RunJournalRecoveryOutcome::IO_FAILURE,
            RunJournalRecoveryError::CLOSE_FAILED, validation, closeError, std::move(preservedTailBytes));
    }
    return recoveryResult(
        truncationBegan ? RunJournalRecoveryOutcome::RECOVERY_REQUIRED : RunJournalRecoveryOutcome::IO_FAILURE, error,
        validation, systemError, std::move(preservedTailBytes));
}

} // namespace

namespace detail {

RunJournalRecoveryResult prepareRunJournalV1WithHooks(
    const std::filesystem::path &canonicalPath, const RunJournalRecoveryHooks &recoveryHooks,
    const RunJournalWriterHooks &writerHooks, PreparedRunJournalV1 &output,
    const std::optional<domain::ExchangeRunId> expectedRunId) noexcept {
    const int descriptor = openReadWrite(canonicalPath);
    if (descriptor < 0) {
        const int systemError = errno;
        if (systemError == ENOENT) {
            const RunJournalLoadResult validation{
                .outcome = RunJournalLoadOutcome::MISSING,
                .systemError = systemError,
            };
            return recoveryResult(RunJournalRecoveryOutcome::MISSING, RunJournalRecoveryError::NONE, validation,
                                  systemError);
        }
        return recoveryResult(RunJournalRecoveryOutcome::IO_FAILURE, RunJournalRecoveryError::NONE, {}, systemError);
    }

    LoadedRunJournalV1 loaded;
    const RunJournalReaderHooks readerHooks{recoveryHooks.context, recoveryHooks.readFile};
    auto validation = loadRunJournalV1FromDescriptorWithHooks(descriptor, readerHooks, loaded);
    if ((validation.outcome == RunJournalLoadOutcome::VALID ||
         validation.outcome == RunJournalLoadOutcome::INCOMPLETE_TAIL) &&
        expectedRunId.has_value() && loaded.header.exchangeRunId != *expectedRunId) {
        validation = {
            .outcome = RunJournalLoadOutcome::CORRUPTION,
            .error = RunJournalLoadError::EXCHANGE_RUN_ID_MISMATCH,
        };
    }
    if (validation.outcome != RunJournalLoadOutcome::VALID &&
        validation.outcome != RunJournalLoadOutcome::INCOMPLETE_TAIL) {
        const auto outcome = validation.outcome == RunJournalLoadOutcome::CORRUPTION
                                 ? RunJournalRecoveryOutcome::CORRUPTION
                                 : RunJournalRecoveryOutcome::IO_FAILURE;
        if (recoveryHooks.closeFile(recoveryHooks.context, descriptor) != 0) {
            return recoveryResult(RunJournalRecoveryOutcome::IO_FAILURE, RunJournalRecoveryError::CLOSE_FAILED,
                                  validation, errno);
        }
        return recoveryResult(outcome, RunJournalRecoveryError::NONE, validation, validation.systemError);
    }

    if (loaded.validCommittedByteCount > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return closeAfterFailure(descriptor, recoveryHooks, false, RunJournalRecoveryError::SEEK_FAILED, validation,
                                 EOVERFLOW);
    }
    RunHeaderV1 writerHeader;
    try {
        writerHeader = loaded.header;
    } catch (const std::bad_alloc &) {
        return closeAfterFailure(descriptor, recoveryHooks, false, RunJournalRecoveryError::NONE, validation, ENOMEM);
    }
    const off_t committedEnd = static_cast<off_t>(loaded.validCommittedByteCount);
    std::vector<std::byte> preservedTailBytes;
    bool truncationBegan = false;

    if (validation.outcome == RunJournalLoadOutcome::INCOMPLETE_TAIL) {
        try {
            preservedTailBytes.resize(static_cast<std::size_t>(validation.tailLength));
        } catch (const std::bad_alloc &) {
            return closeAfterFailure(descriptor, recoveryHooks, false,
                                     RunJournalRecoveryError::TAIL_PRESERVATION_FAILED, validation, ENOMEM);
        }

        int systemError = 0;
        if (seekDescriptor(descriptor, committedEnd, recoveryHooks, systemError) < 0) {
            return closeAfterFailure(descriptor, recoveryHooks, false,
                                     RunJournalRecoveryError::TAIL_PRESERVATION_FAILED, validation, systemError);
        }
        if (!readExact(descriptor, preservedTailBytes, recoveryHooks, systemError)) {
            return closeAfterFailure(descriptor, recoveryHooks, false,
                                     RunJournalRecoveryError::TAIL_PRESERVATION_FAILED, validation, systemError);
        }

        truncationBegan = true;
        if (recoveryHooks.truncateFile(recoveryHooks.context, descriptor, committedEnd) != 0) {
            const int truncateError = errno;
            return closeAfterFailure(descriptor, recoveryHooks, true, RunJournalRecoveryError::TRUNCATE_FAILED,
                                     validation, truncateError, std::move(preservedTailBytes));
        }
        if (!syncDescriptor(descriptor, recoveryHooks, systemError)) {
            return closeAfterFailure(descriptor, recoveryHooks, true, RunJournalRecoveryError::SYNC_FAILED, validation,
                                     systemError, std::move(preservedTailBytes));
        }
    }

    int seekError = 0;
    if (seekDescriptor(descriptor, committedEnd, recoveryHooks, seekError) < 0) {
        return closeAfterFailure(descriptor, recoveryHooks, truncationBegan, RunJournalRecoveryError::SEEK_FAILED,
                                 validation, seekError, std::move(preservedTailBytes));
    }

    auto writer = makeRecoveredRunJournalWriterV1(
        descriptor, std::move(writerHeader), static_cast<std::uint64_t>(loaded.commands.size()),
        loaded.validCommittedByteCount, domain::CommandSequence{static_cast<std::uint64_t>(loaded.commands.size()) + 1},
        writerHooks);
    if (writer == nullptr) {
        return closeAfterFailure(descriptor, recoveryHooks, truncationBegan, RunJournalRecoveryError::NONE, validation,
                                 ENOMEM, std::move(preservedTailBytes));
    }

    PreparedRunJournalV1 prepared{
        .journal = std::move(loaded),
        .writer = std::move(writer),
    };
    output = std::move(prepared);
    return recoveryResult(RunJournalRecoveryOutcome::PREPARED, RunJournalRecoveryError::NONE, validation, 0,
                          std::move(preservedTailBytes));
}

} // namespace detail

RunJournalRecoveryResult prepareRunJournalV1(const std::filesystem::path &canonicalPath, PreparedRunJournalV1 &output,
                                             const std::optional<domain::ExchangeRunId> expectedRunId) noexcept {
    return detail::prepareRunJournalV1WithHooks(canonicalPath, SYSTEM_HOOKS, detail::systemRunJournalWriterHooks(),
                                                output, expectedRunId);
}

} // namespace exchange::storage
