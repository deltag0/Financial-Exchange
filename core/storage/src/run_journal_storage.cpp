#include "run_journal_storage.hpp"

#include "run_journal_storage_internal.hpp"

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <new>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace exchange::storage {
namespace {

ssize_t systemWriteFile(void *, const int descriptor, const void *buffer, const std::size_t size) noexcept {
    return ::write(descriptor, buffer, size);
}

int systemSyncFile(void *, const int descriptor) noexcept {
    return ::fsync(descriptor);
}

int systemCloseFile(void *, const int descriptor) noexcept {
    return ::close(descriptor);
}

const detail::RunJournalStorageHooks SYSTEM_HOOKS{
    nullptr,
    systemWriteFile,
    systemSyncFile,
    systemCloseFile,
};

RunJournalCreateResult createResult(const RunJournalCreateOutcome outcome,
                                    const RunHeaderCodecError codecError = RunHeaderCodecError::NONE,
                                    const int systemError = 0) noexcept {
    return {outcome, codecError, systemError};
}

int openDirectory(const std::filesystem::path &path) noexcept {
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

bool syncDescriptor(const int descriptor, const detail::RunJournalStorageHooks &hooks, int &systemError) noexcept {
    int result = -1;
    do {
        result = hooks.syncFile(hooks.context, descriptor);
    } while (result < 0 && errno == EINTR);

    if (result == 0) {
        return true;
    }
    systemError = errno;
    return false;
}

RunJournalCreateResult createEncodedRunJournalV1(const std::filesystem::path &canonicalPath,
                                                 const std::span<const std::byte> encodedHeader,
                                                 const detail::RunJournalStorageHooks &hooks,
                                                 int *const descriptorOutput) noexcept {
    std::filesystem::path parentPath;
    try {
        parentPath = canonicalPath.parent_path();
        if (parentPath.empty()) {
            parentPath = ".";
        }
    } catch (const std::bad_alloc &) {
        return createResult(RunJournalCreateOutcome::IO_FAILURE, RunHeaderCodecError::NONE, ENOMEM);
    } catch (...) {
        return createResult(RunJournalCreateOutcome::IO_FAILURE, RunHeaderCodecError::NONE, EIO);
    }

    int descriptor = -1;
    do {
        descriptor = ::open(canonicalPath.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        const int systemError = errno;
        if (systemError == EEXIST) {
            return createResult(RunJournalCreateOutcome::PATH_ALREADY_EXISTS, RunHeaderCodecError::NONE, systemError);
        }
        return createResult(RunJournalCreateOutcome::IO_FAILURE, RunHeaderCodecError::NONE, systemError);
    }

    std::size_t bytesWritten = 0;
    while (bytesWritten < encodedHeader.size()) {
        const std::size_t remaining = encodedHeader.size() - bytesWritten;
        const ssize_t result =
            hooks.writeFile(hooks.context, descriptor, encodedHeader.data() + bytesWritten, remaining);
        if (result > 0 && static_cast<std::size_t>(result) <= remaining) {
            bytesWritten += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }

        const int systemError = result == 0 || result > 0 ? EIO : errno;
        hooks.closeFile(hooks.context, descriptor);
        return createResult(RunJournalCreateOutcome::IO_FAILURE, RunHeaderCodecError::NONE, systemError);
    }

    int systemError = 0;
    if (!syncDescriptor(descriptor, hooks, systemError)) {
        hooks.closeFile(hooks.context, descriptor);
        return createResult(RunJournalCreateOutcome::UNCERTAIN, RunHeaderCodecError::NONE, systemError);
    }

    if (descriptorOutput == nullptr && hooks.closeFile(hooks.context, descriptor) != 0) {
        return createResult(RunJournalCreateOutcome::UNCERTAIN, RunHeaderCodecError::NONE, errno);
    }

    const int directoryDescriptor = openDirectory(parentPath);
    if (directoryDescriptor < 0) {
        const int directoryOpenError = errno;
        if (descriptorOutput != nullptr) {
            hooks.closeFile(hooks.context, descriptor);
        }
        return createResult(RunJournalCreateOutcome::UNCERTAIN, RunHeaderCodecError::NONE, directoryOpenError);
    }

    if (!syncDescriptor(directoryDescriptor, hooks, systemError)) {
        hooks.closeFile(hooks.context, directoryDescriptor);
        if (descriptorOutput != nullptr) {
            hooks.closeFile(hooks.context, descriptor);
        }
        return createResult(RunJournalCreateOutcome::UNCERTAIN, RunHeaderCodecError::NONE, systemError);
    }

    if (hooks.closeFile(hooks.context, directoryDescriptor) != 0) {
        const int directoryCloseError = errno;
        if (descriptorOutput != nullptr) {
            hooks.closeFile(hooks.context, descriptor);
        }
        return createResult(RunJournalCreateOutcome::UNCERTAIN, RunHeaderCodecError::NONE, directoryCloseError);
    }

    if (descriptorOutput != nullptr) {
        *descriptorOutput = descriptor;
    }
    return createResult(RunJournalCreateOutcome::CREATED);
}

} // namespace

namespace detail {

RunJournalCreateResult createRunJournalV1WithHooks(const std::filesystem::path &canonicalPath,
                                                   const RunHeaderV1 &header,
                                                   const RunJournalStorageHooks &hooks) noexcept {
    std::vector<std::byte> encoded;
    const auto codecError = encodeRunHeaderV1(header, encoded);
    if (codecError != RunHeaderCodecError::NONE) {
        if (codecError == RunHeaderCodecError::ALLOCATION_FAILURE) {
            return createResult(RunJournalCreateOutcome::IO_FAILURE, codecError, ENOMEM);
        }
        return createResult(RunJournalCreateOutcome::INVALID_HEADER, codecError);
    }
    return createEncodedRunJournalV1(canonicalPath, encoded, hooks, nullptr);
}

RunJournalCreateResult createEncodedRunJournalV1AndRetainDescriptor(const std::filesystem::path &canonicalPath,
                                                                    const std::span<const std::byte> encodedHeader,
                                                                    int &descriptorOutput) noexcept {
    return createEncodedRunJournalV1(canonicalPath, encodedHeader, SYSTEM_HOOKS, &descriptorOutput);
}

} // namespace detail

RunJournalCreateResult createRunJournalV1(const std::filesystem::path &canonicalPath,
                                          const RunHeaderV1 &header) noexcept {
    return detail::createRunJournalV1WithHooks(canonicalPath, header, SYSTEM_HOOKS);
}

} // namespace exchange::storage
