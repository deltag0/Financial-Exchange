#include "run_catalog_storage.hpp"

#include "run_catalog_storage_internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace exchange::storage {
namespace {

class IsolatedDirectory {
public:
    IsolatedDirectory() {
        std::array<char, 47> pathTemplate{};
        constexpr char TEMPLATE[] = "/tmp/exchange-run-catalog-test-XXXXXX";
        std::copy(std::begin(TEMPLATE), std::end(TEMPLATE), pathTemplate.begin());
        char* created = ::mkdtemp(pathTemplate.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~IsolatedDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    IsolatedDirectory(const IsolatedDirectory&) = delete;
    IsolatedDirectory& operator=(const IsolatedDirectory&) = delete;

    [[nodiscard]] bool valid() const {
        return !path_.empty();
    }

    [[nodiscard]] std::filesystem::path catalogPath() const {
        return path_ / "run-catalog-v1";
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_{};
};

RunCatalogSnapshotV1 emptySnapshot(std::uint64_t generation) {
    return {
        generation, std::nullopt, std::nullopt, RunCatalogDisposition::NONE, std::nullopt,
    };
}

RunCatalogSnapshotV1 activeSnapshot(std::uint64_t generation, std::uint64_t runId) {
    return {
        generation,   domain::ExchangeRunId{runId}, domain::ExchangeRunId{runId}, RunCatalogDisposition::OPEN,
        std::nullopt,
    };
}

RunCatalogSnapshotV1 sentinelSnapshot() {
    return activeSnapshot(99, 88);
}

void writeBytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open());
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(stream.good());
}

std::vector<std::byte> readBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(stream.is_open());
    const auto size = stream.tellg();
    EXPECT_GE(size, 0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    EXPECT_TRUE(stream.good());
    return bytes;
}

RunCatalogV1Bytes encode(const RunCatalogSnapshotV1& snapshot) {
    RunCatalogV1Bytes bytes{};
    EXPECT_EQ(encodeRunCatalogV1(snapshot, bytes), RunCatalogCodecError::NONE);
    return bytes;
}

std::size_t temporaryFileCount(const IsolatedDirectory& directory) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory.path())) {
        if (entry.path().filename().string().starts_with("run-catalog-v1.tmp.")) {
            ++count;
        }
    }
    return count;
}

struct WriteScript {
    int calls{0};
    bool interruptFirst{false};
    bool failSecond{false};
    std::size_t maximumWrite{RUN_CATALOG_V1_SIZE};
};

ssize_t scriptedWrite(void* context, int descriptor, const void* buffer, std::size_t size) noexcept {
    auto& script = *static_cast<WriteScript*>(context);
    ++script.calls;
    if (script.interruptFirst && script.calls == 1) {
        errno = EINTR;
        return -1;
    }
    if (script.failSecond && script.calls == 2) {
        errno = EIO;
        return -1;
    }
    return ::write(descriptor, buffer, std::min(size, script.maximumWrite));
}

struct SyncScript {
    int calls{0};
    int failureCall{0};
};

int scriptedSync(void* context, int descriptor) noexcept {
    auto& script = *static_cast<SyncScript*>(context);
    ++script.calls;
    if (script.calls == script.failureCall) {
        errno = EIO;
        return -1;
    }
    return ::fsync(descriptor);
}

int replaceThenReportInterruption(void*, const char* source, const char* destination) noexcept {
    if (::rename(source, destination) != 0) {
        return -1;
    }
    errno = EINTR;
    return -1;
}

TEST(RunCatalogStorageTest, MissingCatalogIsDistinctAndLeavesOutputUnchanged) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    auto output = sentinelSnapshot();
    const auto originalOutput = output;

    const auto result = loadRunCatalogV1(directory.catalogPath(), output);

    EXPECT_EQ(result.outcome, RunCatalogLoadOutcome::MISSING);
    EXPECT_EQ(result.systemError, ENOENT);
    EXPECT_EQ(output, originalOutput);
}

TEST(RunCatalogStorageTest, IoFailureIsDistinctAndLeavesOutputUnchanged) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    auto output = sentinelSnapshot();
    const auto originalOutput = output;

    const auto result = loadRunCatalogV1(directory.path(), output);

    EXPECT_EQ(result.outcome, RunCatalogLoadOutcome::IO_FAILURE);
    EXPECT_EQ(result.systemError, EISDIR);
    EXPECT_EQ(output, originalOutput);
}

TEST(RunCatalogStorageTest, CreatesGenerationOneAndReloadsExactSnapshot) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto expected = activeSnapshot(1, 7);

    const auto replaceResult = replaceRunCatalogV1(directory.catalogPath(), expected);
    RunCatalogSnapshotV1 loaded{};
    const auto loadResult = loadRunCatalogV1(directory.catalogPath(), loaded);

    EXPECT_EQ(replaceResult.outcome, RunCatalogReplaceOutcome::COMMITTED);
    EXPECT_EQ(loadResult.outcome, RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(loaded, expected);
    const auto encoded = encode(expected);
    EXPECT_EQ(readBytes(directory.catalogPath()), std::vector<std::byte>(encoded.begin(), encoded.end()));
}

TEST(RunCatalogStorageTest, AtomicallyReplacesWithGenerationTwo) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    ASSERT_EQ(replaceRunCatalogV1(directory.catalogPath(), emptySnapshot(1)).outcome,
              RunCatalogReplaceOutcome::COMMITTED);
    const auto expected = activeSnapshot(2, std::numeric_limits<std::uint64_t>::max());

    const auto replaceResult = replaceRunCatalogV1(directory.catalogPath(), expected);
    RunCatalogSnapshotV1 loaded{};
    const auto loadResult = loadRunCatalogV1(directory.catalogPath(), loaded);

    EXPECT_EQ(replaceResult.outcome, RunCatalogReplaceOutcome::COMMITTED);
    EXPECT_EQ(loadResult.outcome, RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(loaded, expected);
}

TEST(RunCatalogStorageTest, RejectsInvalidInitialGenerationWithoutCreatingCatalog) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());

    const auto result = replaceRunCatalogV1(directory.catalogPath(), emptySnapshot(2));

    EXPECT_EQ(result.outcome, RunCatalogReplaceOutcome::GENERATION_CONFLICT);
    EXPECT_FALSE(std::filesystem::exists(directory.catalogPath()));
    EXPECT_EQ(temporaryFileCount(directory), 0U);
}

TEST(RunCatalogStorageTest, RejectsInvalidStaleAndSkippedSnapshotsWithoutChangingCanonicalBytes) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    ASSERT_EQ(replaceRunCatalogV1(directory.catalogPath(), emptySnapshot(1)).outcome,
              RunCatalogReplaceOutcome::COMMITTED);
    const auto originalBytes = readBytes(directory.catalogPath());

    const auto invalid = replaceRunCatalogV1(directory.catalogPath(), emptySnapshot(0));
    const auto stale = replaceRunCatalogV1(directory.catalogPath(), emptySnapshot(1));
    const auto skipped = replaceRunCatalogV1(directory.catalogPath(), emptySnapshot(3));

    EXPECT_EQ(invalid.outcome, RunCatalogReplaceOutcome::INVALID_SNAPSHOT);
    EXPECT_EQ(invalid.codecError, RunCatalogCodecError::ZERO_GENERATION);
    EXPECT_EQ(stale.outcome, RunCatalogReplaceOutcome::GENERATION_CONFLICT);
    EXPECT_EQ(skipped.outcome, RunCatalogReplaceOutcome::GENERATION_CONFLICT);
    EXPECT_EQ(readBytes(directory.catalogPath()), originalBytes);
    EXPECT_EQ(temporaryFileCount(directory), 0U);
}

TEST(RunCatalogStorageTest, RejectsExhaustedGenerationWithoutChangingCanonicalBytes) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto exhausted = emptySnapshot(std::numeric_limits<std::uint64_t>::max());
    const auto exhaustedBytes = encode(exhausted);
    writeBytes(directory.catalogPath(), exhaustedBytes);
    const auto originalBytes = readBytes(directory.catalogPath());

    const auto result = replaceRunCatalogV1(directory.catalogPath(), emptySnapshot(1));

    EXPECT_EQ(result.outcome, RunCatalogReplaceOutcome::GENERATION_CONFLICT);
    EXPECT_EQ(readBytes(directory.catalogPath()), originalBytes);
    EXPECT_EQ(temporaryFileCount(directory), 0U);
}

TEST(RunCatalogStorageTest, RejectsMalformedCanonicalFilesAndLeavesOutputUnchanged) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto valid = encode(emptySnapshot(1));

    const auto assertInvalid = [&](std::span<const std::byte> bytes, RunCatalogCodecError expectedError) {
        writeBytes(directory.catalogPath(), bytes);
        auto output = sentinelSnapshot();
        const auto originalOutput = output;
        const auto result = loadRunCatalogV1(directory.catalogPath(), output);
        EXPECT_EQ(result.outcome, RunCatalogLoadOutcome::INVALID);
        EXPECT_EQ(result.codecError, expectedError);
        EXPECT_EQ(output, originalOutput);
    };

    assertInvalid(std::span<const std::byte>(valid).first(RUN_CATALOG_V1_SIZE - 1), RunCatalogCodecError::INVALID_SIZE);

    std::array<std::byte, RUN_CATALOG_V1_SIZE + 1> oversized{};
    std::copy(valid.begin(), valid.end(), oversized.begin());
    assertInvalid(oversized, RunCatalogCodecError::INVALID_SIZE);

    auto corruptMagic = valid;
    corruptMagic[0] = std::byte{'X'};
    assertInvalid(corruptMagic, RunCatalogCodecError::INVALID_MAGIC);

    auto invalidChecksum = valid;
    invalidChecksum[8] ^= std::byte{0x01};
    assertInvalid(invalidChecksum, RunCatalogCodecError::CHECKSUM_MISMATCH);
}

TEST(RunCatalogStorageTest, RejectsReplacementWhenCanonicalCatalogIsInvalid) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    auto bytes = encode(emptySnapshot(1));
    bytes[48] ^= std::byte{0x01};
    writeBytes(directory.catalogPath(), bytes);
    const auto originalBytes = readBytes(directory.catalogPath());

    const auto result = replaceRunCatalogV1(directory.catalogPath(), emptySnapshot(2));

    EXPECT_EQ(result.outcome, RunCatalogReplaceOutcome::INVALID_EXISTING_CATALOG);
    EXPECT_EQ(result.codecError, RunCatalogCodecError::CHECKSUM_MISMATCH);
    EXPECT_EQ(readBytes(directory.catalogPath()), originalBytes);
}

TEST(RunCatalogStorageTest, IgnoresTemporaryFilesWhenLoadingCanonicalCatalog) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto canonical = activeSnapshot(1, 1);
    writeBytes(directory.path() / "run-catalog-v1.tmp.abandoned", encode(activeSnapshot(2, 2)));
    auto loaded = sentinelSnapshot();
    const auto unchanged = loaded;

    const auto missingResult = loadRunCatalogV1(directory.catalogPath(), loaded);

    EXPECT_EQ(missingResult.outcome, RunCatalogLoadOutcome::MISSING);
    EXPECT_EQ(loaded, unchanged);

    writeBytes(directory.catalogPath(), encode(canonical));

    const auto result = loadRunCatalogV1(directory.catalogPath(), loaded);

    EXPECT_EQ(result.outcome, RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(loaded, canonical);
}

TEST(RunCatalogStorageTest, RetriesInterruptedWritesAndCompletesPartialWrites) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    WriteScript script{0, true, false, 5};
    auto hooks = detail::systemRunCatalogStorageHooks();
    hooks.context = &script;
    hooks.writeFile = scriptedWrite;
    const auto expected = activeSnapshot(1, 42);

    const auto result = detail::replaceRunCatalogV1WithHooks(directory.catalogPath(), expected, hooks);
    RunCatalogSnapshotV1 loaded{};

    EXPECT_EQ(result.outcome, RunCatalogReplaceOutcome::COMMITTED);
    EXPECT_GT(script.calls, 2);
    EXPECT_EQ(loadRunCatalogV1(directory.catalogPath(), loaded).outcome, RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(loaded, expected);
}

TEST(RunCatalogStorageTest, WriteFailureBeforeReplacementCleansTemporaryFileAndPreservesCanonical) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    ASSERT_EQ(replaceRunCatalogV1(directory.catalogPath(), emptySnapshot(1)).outcome,
              RunCatalogReplaceOutcome::COMMITTED);
    const auto originalBytes = readBytes(directory.catalogPath());
    WriteScript script{0, false, true, 5};
    auto hooks = detail::systemRunCatalogStorageHooks();
    hooks.context = &script;
    hooks.writeFile = scriptedWrite;

    const auto result = detail::replaceRunCatalogV1WithHooks(directory.catalogPath(), emptySnapshot(2), hooks);

    EXPECT_EQ(result.outcome, RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE);
    EXPECT_EQ(result.systemError, EIO);
    EXPECT_EQ(readBytes(directory.catalogPath()), originalBytes);
    EXPECT_EQ(temporaryFileCount(directory), 0U);
}

TEST(RunCatalogStorageTest, SyncFailureBeforeReplacementCleansTemporaryFileAndPreservesCanonical) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    ASSERT_EQ(replaceRunCatalogV1(directory.catalogPath(), emptySnapshot(1)).outcome,
              RunCatalogReplaceOutcome::COMMITTED);
    const auto originalBytes = readBytes(directory.catalogPath());
    SyncScript script{0, 1};
    auto hooks = detail::systemRunCatalogStorageHooks();
    hooks.context = &script;
    hooks.syncFile = scriptedSync;

    const auto result = detail::replaceRunCatalogV1WithHooks(directory.catalogPath(), emptySnapshot(2), hooks);

    EXPECT_EQ(result.outcome, RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE);
    EXPECT_EQ(result.systemError, EIO);
    EXPECT_EQ(readBytes(directory.catalogPath()), originalBytes);
    EXPECT_EQ(temporaryFileCount(directory), 0U);
}

TEST(RunCatalogStorageTest, InterruptedReplacementIsUncertainAndReloadsCanonicalCatalog) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    ASSERT_EQ(replaceRunCatalogV1(directory.catalogPath(), emptySnapshot(1)).outcome,
              RunCatalogReplaceOutcome::COMMITTED);
    const auto next = activeSnapshot(2, 11);
    auto hooks = detail::systemRunCatalogStorageHooks();
    hooks.replaceFile = replaceThenReportInterruption;

    const auto result = detail::replaceRunCatalogV1WithHooks(directory.catalogPath(), next, hooks);

    ASSERT_EQ(result.outcome, RunCatalogReplaceOutcome::UNCERTAIN);
    ASSERT_TRUE(result.observedSnapshot.has_value());
    EXPECT_EQ(*result.observedSnapshot, next);
}

TEST(RunCatalogStorageTest, DirectorySyncFailureIsUncertainAndReloadsCanonicalCatalog) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    ASSERT_EQ(replaceRunCatalogV1(directory.catalogPath(), emptySnapshot(1)).outcome,
              RunCatalogReplaceOutcome::COMMITTED);
    const auto next = activeSnapshot(2, 12);
    SyncScript script{0, 2};
    auto hooks = detail::systemRunCatalogStorageHooks();
    hooks.context = &script;
    hooks.syncFile = scriptedSync;

    const auto result = detail::replaceRunCatalogV1WithHooks(directory.catalogPath(), next, hooks);

    ASSERT_EQ(result.outcome, RunCatalogReplaceOutcome::UNCERTAIN);
    EXPECT_EQ(result.systemError, EIO);
    ASSERT_TRUE(result.observedSnapshot.has_value());
    EXPECT_EQ(*result.observedSnapshot, next);
}

} // namespace
} // namespace exchange::storage
