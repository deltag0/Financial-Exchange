#include "run_catalog_storage.hpp"

#include "run_catalog_storage_internal.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <unistd.h>

namespace exchange::storage {
namespace {

constexpr std::size_t CATALOG_READ_BUFFER_SIZE = RUN_CATALOG_V1_SIZE + 1;
constexpr std::size_t TEMPORARY_RANDOM_SUFFIX_LENGTH = 6;

ssize_t systemWriteFile(void*, int descriptor, const void* buffer, std::size_t size) noexcept {
    return ::write(descriptor, buffer, size);
}

int systemSyncFile(void*, int descriptor) noexcept {
    return ::fsync(descriptor);
}

int systemReplaceFile(void*, const char* source, const char* destination) noexcept {
    return ::rename(source, destination);
}

const detail::RunCatalogStorageHooks SYSTEM_HOOKS{
    nullptr,
    systemWriteFile,
    systemSyncFile,
    systemReplaceFile,
};

RunCatalogLoadResult ioLoadFailure(int systemError) {
    return {RunCatalogLoadOutcome::IO_FAILURE, RunCatalogCodecError::NONE, systemError};
}

RunCatalogReplaceResult replaceResult(RunCatalogReplaceOutcome outcome,
                                      RunCatalogCodecError codecError = RunCatalogCodecError::NONE,
                                      int systemError = 0) {
    return {outcome, codecError, systemError, std::nullopt};
}

int openReadOnly(const std::filesystem::path& path) {
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

int openDirectory(const std::filesystem::path& path) {
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

bool syncFile(int descriptor, const detail::RunCatalogStorageHooks& hooks, int& systemError) {
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

void removeTemporaryFile(const std::string& path) {
    int result = -1;
    do {
        result = ::unlink(path.c_str());
    } while (result < 0 && errno == EINTR);
}

RunCatalogReplaceResult uncertainResult(const std::filesystem::path& canonicalPath, int systemError) {
    RunCatalogSnapshotV1 observedSnapshot{};
    const auto loadResult = loadRunCatalogV1(canonicalPath, observedSnapshot);

    RunCatalogReplaceResult result =
        replaceResult(RunCatalogReplaceOutcome::UNCERTAIN, RunCatalogCodecError::NONE, systemError);
    if (loadResult.outcome == RunCatalogLoadOutcome::LOADED) {
        result.observedSnapshot = observedSnapshot;
    }
    return result;
}

} // namespace

namespace detail {

const RunCatalogStorageHooks& systemRunCatalogStorageHooks() noexcept {
    return SYSTEM_HOOKS;
}

RunCatalogReplaceResult replaceRunCatalogV1WithHooks(const std::filesystem::path& canonicalPath,
                                                     const RunCatalogSnapshotV1& nextSnapshot,
                                                     const RunCatalogStorageHooks& hooks) noexcept {
    RunCatalogV1Bytes encoded{};
    const auto encodeError = encodeRunCatalogV1(nextSnapshot, encoded);
    if (encodeError != RunCatalogCodecError::NONE) {
        return replaceResult(RunCatalogReplaceOutcome::INVALID_SNAPSHOT, encodeError);
    }

    RunCatalogSnapshotV1 currentSnapshot{};
    const auto currentResult = loadRunCatalogV1(canonicalPath, currentSnapshot);
    switch (currentResult.outcome) {
        case RunCatalogLoadOutcome::INVALID:
            return replaceResult(RunCatalogReplaceOutcome::INVALID_EXISTING_CATALOG, currentResult.codecError);
        case RunCatalogLoadOutcome::IO_FAILURE:
            return replaceResult(RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE, RunCatalogCodecError::NONE,
                                 currentResult.systemError);
        case RunCatalogLoadOutcome::MISSING:
            if (nextSnapshot.generation != 1) {
                return replaceResult(RunCatalogReplaceOutcome::GENERATION_CONFLICT);
            }
            break;
        case RunCatalogLoadOutcome::LOADED:
            if (currentSnapshot.generation == std::numeric_limits<std::uint64_t>::max() ||
                nextSnapshot.generation != currentSnapshot.generation + 1) {
                return replaceResult(RunCatalogReplaceOutcome::GENERATION_CONFLICT);
            }
            break;
    }

    std::string temporaryPath;
    std::filesystem::path parentPath;
    try {
        temporaryPath = canonicalPath.string() + ".tmp.XXXXXX";
        parentPath = canonicalPath.parent_path();
        if (parentPath.empty()) {
            parentPath = ".";
        }
    } catch (const std::bad_alloc&) {
        return replaceResult(RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE, RunCatalogCodecError::NONE, ENOMEM);
    } catch (...) {
        return replaceResult(RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE, RunCatalogCodecError::NONE, EIO);
    }

    int temporaryDescriptor = -1;
    do {
        for (std::size_t index = temporaryPath.size() - TEMPORARY_RANDOM_SUFFIX_LENGTH; index < temporaryPath.size();
             ++index) {
            temporaryPath[index] = 'X';
        }
        temporaryDescriptor = ::mkstemp(temporaryPath.data());
    } while (temporaryDescriptor < 0 && errno == EINTR);

    if (temporaryDescriptor < 0) {
        const int systemError = errno;
        return replaceResult(RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE, RunCatalogCodecError::NONE,
                             systemError);
    }

    std::size_t bytesWritten = 0;
    while (bytesWritten < encoded.size()) {
        const ssize_t result = hooks.writeFile(hooks.context, temporaryDescriptor, encoded.data() + bytesWritten,
                                               encoded.size() - bytesWritten);
        if (result > 0) {
            bytesWritten += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }

        const int systemError = result == 0 ? EIO : errno;
        ::close(temporaryDescriptor);
        removeTemporaryFile(temporaryPath);
        return replaceResult(RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE, RunCatalogCodecError::NONE,
                             systemError);
    }

    int systemError = 0;
    if (!syncFile(temporaryDescriptor, hooks, systemError)) {
        ::close(temporaryDescriptor);
        removeTemporaryFile(temporaryPath);
        return replaceResult(RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE, RunCatalogCodecError::NONE,
                             systemError);
    }

    if (::close(temporaryDescriptor) != 0) {
        systemError = errno;
        removeTemporaryFile(temporaryPath);
        return replaceResult(RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE, RunCatalogCodecError::NONE,
                             systemError);
    }

    if (hooks.replaceFile(hooks.context, temporaryPath.c_str(), canonicalPath.c_str()) != 0) {
        systemError = errno;
        if (systemError == EINTR) {
            return uncertainResult(canonicalPath, systemError);
        }
        removeTemporaryFile(temporaryPath);
        return replaceResult(RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE, RunCatalogCodecError::NONE,
                             systemError);
    }

    const int directoryDescriptor = openDirectory(parentPath);
    if (directoryDescriptor < 0) {
        return uncertainResult(canonicalPath, errno);
    }

    if (!syncFile(directoryDescriptor, hooks, systemError)) {
        ::close(directoryDescriptor);
        return uncertainResult(canonicalPath, systemError);
    }

    if (::close(directoryDescriptor) != 0) {
        return uncertainResult(canonicalPath, errno);
    }

    return replaceResult(RunCatalogReplaceOutcome::COMMITTED);
}

} // namespace detail

RunCatalogLoadResult loadRunCatalogV1(const std::filesystem::path& canonicalPath,
                                      RunCatalogSnapshotV1& output) noexcept {
    const int descriptor = openReadOnly(canonicalPath);
    if (descriptor < 0) {
        const int systemError = errno;
        if (systemError == ENOENT) {
            return {RunCatalogLoadOutcome::MISSING, RunCatalogCodecError::NONE, systemError};
        }
        return ioLoadFailure(systemError);
    }

    std::array<std::byte, CATALOG_READ_BUFFER_SIZE> bytes{};
    std::size_t bytesRead = 0;
    while (bytesRead < bytes.size()) {
        const ssize_t result = ::read(descriptor, bytes.data() + bytesRead, bytes.size() - bytesRead);
        if (result > 0) {
            bytesRead += static_cast<std::size_t>(result);
            continue;
        }
        if (result == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }

        const int systemError = errno;
        ::close(descriptor);
        return ioLoadFailure(systemError);
    }

    if (::close(descriptor) != 0) {
        return ioLoadFailure(errno);
    }

    RunCatalogSnapshotV1 decoded{};
    const auto codecError = decodeRunCatalogV1(std::span<const std::byte>(bytes.data(), bytesRead), decoded);
    if (codecError != RunCatalogCodecError::NONE) {
        return {RunCatalogLoadOutcome::INVALID, codecError, 0};
    }

    output = decoded;
    return {RunCatalogLoadOutcome::LOADED, RunCatalogCodecError::NONE, 0};
}

RunCatalogReplaceResult replaceRunCatalogV1(const std::filesystem::path& canonicalPath,
                                            const RunCatalogSnapshotV1& nextSnapshot) noexcept {
    return detail::replaceRunCatalogV1WithHooks(canonicalPath, nextSnapshot, SYSTEM_HOOKS);
}

} // namespace exchange::storage
