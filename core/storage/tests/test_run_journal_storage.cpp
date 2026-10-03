#include "run_journal_storage.hpp"

#include "run_journal_storage_internal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace exchange::storage {
namespace {

class IsolatedDirectory final {
public:
    IsolatedDirectory() {
        std::array<char, 48> pathTemplate{};
        constexpr char TEMPLATE[] = "/tmp/exchange-run-journal-test-XXXXXX";
        std::copy(std::begin(TEMPLATE), std::end(TEMPLATE), pathTemplate.begin());
        char *created = ::mkdtemp(pathTemplate.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~IsolatedDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    IsolatedDirectory(const IsolatedDirectory &) = delete;
    IsolatedDirectory &operator=(const IsolatedDirectory &) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return !path_.empty();
    }

    [[nodiscard]] std::filesystem::path journalPath(const std::string &name = "run-1.fxjr") const {
        return path_ / name;
    }

private:
    std::filesystem::path path_{};
};

RunHeaderV1 goldenHeader() {
    return {
        .exchangeRunId = domain::ExchangeRunId{0x0102030405060708ULL},
        .behavioralRulesVersion = 1,
        .maxEventsPerCommand = 4'096,
        .maxRunCommands = 0x1112131415161718ULL,
        .maxRunJournalBytes = 0x2122232425262728ULL,
        .instruments =
            {
                {domain::InstrumentId{0x3132333435363738ULL}, 0x41424344U},
                {domain::InstrumentId{0x5152535455565758ULL}, 0x61626364U},
            },
    };
}

std::vector<std::byte> readBytes(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(stream.is_open());
    const auto size = stream.tellg();
    EXPECT_GE(size, 0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(bytes.data()), size);
    EXPECT_TRUE(stream.good());
    return bytes;
}

void writeBytes(const std::filesystem::path &path, const std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open());
    stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(stream.good());
}

enum class Operation : std::uint8_t {
    WRITE,
    SYNC,
    CLOSE,
};

struct SyscallScript final {
    std::array<Operation, 128> operations{};
    std::size_t operationCount{0};
    int writeCalls{0};
    int syncCalls{0};
    int closeCalls{0};
    bool interruptFirstWrite{false};
    int zeroWriteCall{0};
    int failingWriteCall{0};
    int writeError{EIO};
    std::size_t maximumWrite{std::numeric_limits<std::size_t>::max()};
    int failingSyncCall{0};
    int syncError{EIO};
    int failingCloseCall{0};
    int closeError{EIO};

    void record(const Operation operation) noexcept {
        if (operationCount < operations.size()) {
            operations[operationCount++] = operation;
        }
    }
};

ssize_t scriptedWrite(void *context, const int descriptor, const void *buffer, const std::size_t size) noexcept {
    auto &script = *static_cast<SyscallScript *>(context);
    script.record(Operation::WRITE);
    ++script.writeCalls;
    if (script.interruptFirstWrite && script.writeCalls == 1) {
        errno = EINTR;
        return -1;
    }
    if (script.writeCalls == script.zeroWriteCall) {
        return 0;
    }
    if (script.writeCalls == script.failingWriteCall) {
        errno = script.writeError;
        return -1;
    }
    return ::write(descriptor, buffer, std::min(size, script.maximumWrite));
}

int scriptedSync(void *context, const int descriptor) noexcept {
    auto &script = *static_cast<SyscallScript *>(context);
    script.record(Operation::SYNC);
    ++script.syncCalls;
    if (script.syncCalls == script.failingSyncCall) {
        errno = script.syncError;
        return -1;
    }
    return ::fsync(descriptor);
}

int scriptedClose(void *context, const int descriptor) noexcept {
    auto &script = *static_cast<SyscallScript *>(context);
    script.record(Operation::CLOSE);
    ++script.closeCalls;
    const int result = ::close(descriptor);
    if (result != 0) {
        return result;
    }
    if (script.closeCalls == script.failingCloseCall) {
        errno = script.closeError;
        return -1;
    }
    return 0;
}

detail::RunJournalStorageHooks hooksFor(SyscallScript &script) {
    return {
        &script,
        scriptedWrite,
        scriptedSync,
        scriptedClose,
    };
}

void expectSystemFailure(const RunJournalCreateResult &result, const RunJournalCreateOutcome outcome,
                         const int systemError) {
    EXPECT_EQ(result.outcome, outcome);
    EXPECT_EQ(result.codecError, RunHeaderCodecError::NONE);
    EXPECT_EQ(result.systemError, systemError);
}

TEST(RunJournalStorageTest, CreatesEncodedHeaderAndCompletesDurabilityStepsInOrder) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    SyscallScript script;
    const auto hooks = hooksFor(script);
    const auto header = goldenHeader();
    std::vector<std::byte> encodedHeader;
    ASSERT_EQ(encodeRunHeaderV1(header, encodedHeader), RunHeaderCodecError::NONE);

    const auto result = detail::createRunJournalV1WithHooks(directory.journalPath(), header, hooks);

    EXPECT_EQ(result.outcome, RunJournalCreateOutcome::CREATED);
    EXPECT_EQ(result.codecError, RunHeaderCodecError::NONE);
    EXPECT_EQ(result.systemError, 0);
    const auto persisted = readBytes(directory.journalPath());
    EXPECT_EQ(persisted, encodedHeader);
    RunHeaderV1 decoded{};
    ASSERT_EQ(decodeRunHeaderV1(persisted, decoded), RunHeaderCodecError::NONE);
    EXPECT_EQ(decoded, header);
    ASSERT_EQ(script.operationCount, static_cast<std::size_t>(script.writeCalls) + 4U);
    for (int writeCall = 0; writeCall < script.writeCalls; ++writeCall) {
        EXPECT_EQ(script.operations[static_cast<std::size_t>(writeCall)], Operation::WRITE);
    }
    const auto firstSync = static_cast<std::size_t>(script.writeCalls);
    EXPECT_EQ(script.operations[firstSync], Operation::SYNC);
    EXPECT_EQ(script.operations[firstSync + 1U], Operation::CLOSE);
    EXPECT_EQ(script.operations[firstSync + 2U], Operation::SYNC);
    EXPECT_EQ(script.operations[firstSync + 3U], Operation::CLOSE);
}

TEST(RunJournalStorageTest, ExistingPathIsRejectedWithoutChangingItsBytes) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    const std::array<std::byte, 4> evidence{std::byte{'K'}, std::byte{'E'}, std::byte{'E'}, std::byte{'P'}};
    writeBytes(directory.journalPath(), evidence);

    const auto result = createRunJournalV1(directory.journalPath(), goldenHeader());

    expectSystemFailure(result, RunJournalCreateOutcome::PATH_ALREADY_EXISTS, EEXIST);
    EXPECT_EQ(readBytes(directory.journalPath()), std::vector<std::byte>(evidence.begin(), evidence.end()));
}

TEST(RunJournalStorageTest, InvalidHeaderCreatesNoFileAndReportsCodecError) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    auto invalid = goldenHeader();
    invalid.exchangeRunId = domain::ExchangeRunId{0};

    const auto result = createRunJournalV1(directory.journalPath(), invalid);

    EXPECT_EQ(result.outcome, RunJournalCreateOutcome::INVALID_HEADER);
    EXPECT_EQ(result.codecError, RunHeaderCodecError::ZERO_EXCHANGE_RUN_ID);
    EXPECT_EQ(result.systemError, 0);
    EXPECT_FALSE(std::filesystem::exists(directory.journalPath()));
}

TEST(RunJournalStorageTest, RetriesInterruptedWritesAndCompletesShortWrites) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    SyscallScript script;
    script.interruptFirstWrite = true;
    script.maximumWrite = 5;
    const auto hooks = hooksFor(script);
    const auto header = goldenHeader();

    const auto result = detail::createRunJournalV1WithHooks(directory.journalPath(), header, hooks);

    EXPECT_EQ(result.outcome, RunJournalCreateOutcome::CREATED);
    EXPECT_GT(script.writeCalls, 2);
    RunHeaderV1 decoded{};
    ASSERT_EQ(decodeRunHeaderV1(readBytes(directory.journalPath()), decoded), RunHeaderCodecError::NONE);
    EXPECT_EQ(decoded, header);
}

TEST(RunJournalStorageTest, ZeroAndErrorWritesAreDefiniteFailuresAndPreserveFiles) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto header = goldenHeader();

    SyscallScript zeroScript;
    zeroScript.zeroWriteCall = 1;
    const auto zeroResult =
        detail::createRunJournalV1WithHooks(directory.journalPath("zero.fxjr"), header, hooksFor(zeroScript));

    expectSystemFailure(zeroResult, RunJournalCreateOutcome::IO_FAILURE, EIO);
    EXPECT_TRUE(std::filesystem::exists(directory.journalPath("zero.fxjr")));
    EXPECT_TRUE(readBytes(directory.journalPath("zero.fxjr")).empty());

    SyscallScript errorScript;
    errorScript.maximumWrite = 5;
    errorScript.failingWriteCall = 2;
    errorScript.writeError = ENOSPC;
    const auto errorResult =
        detail::createRunJournalV1WithHooks(directory.journalPath("error.fxjr"), header, hooksFor(errorScript));

    expectSystemFailure(errorResult, RunJournalCreateOutcome::IO_FAILURE, ENOSPC);
    EXPECT_TRUE(std::filesystem::exists(directory.journalPath("error.fxjr")));
    EXPECT_EQ(std::filesystem::file_size(directory.journalPath("error.fxjr")), 5U);
}

TEST(RunJournalStorageTest, FileSyncFailureIsUncertainAndPreservesCompleteHeader) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    SyscallScript script;
    script.failingSyncCall = 1;
    script.syncError = EIO;
    const auto header = goldenHeader();

    const auto result = detail::createRunJournalV1WithHooks(directory.journalPath(), header, hooksFor(script));

    expectSystemFailure(result, RunJournalCreateOutcome::UNCERTAIN, EIO);
    EXPECT_EQ(script.syncCalls, 1);
    RunHeaderV1 decoded{};
    ASSERT_EQ(decodeRunHeaderV1(readBytes(directory.journalPath()), decoded), RunHeaderCodecError::NONE);
    EXPECT_EQ(decoded, header);
}

TEST(RunJournalStorageTest, FileCloseFailureIsUncertainAndStopsBeforeDirectorySync) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    SyscallScript script;
    script.failingCloseCall = 1;
    script.closeError = EIO;
    const auto header = goldenHeader();

    const auto result = detail::createRunJournalV1WithHooks(directory.journalPath(), header, hooksFor(script));

    expectSystemFailure(result, RunJournalCreateOutcome::UNCERTAIN, EIO);
    EXPECT_EQ(script.syncCalls, 1);
    EXPECT_EQ(script.closeCalls, 1);
    RunHeaderV1 decoded{};
    ASSERT_EQ(decodeRunHeaderV1(readBytes(directory.journalPath()), decoded), RunHeaderCodecError::NONE);
    EXPECT_EQ(decoded, header);
}

TEST(RunJournalStorageTest, DirectorySyncFailureIsUncertainAndCannotReportSuccess) {
    IsolatedDirectory directory;
    ASSERT_TRUE(directory.valid());
    SyscallScript script;
    script.failingSyncCall = 2;
    script.syncError = EIO;
    const auto header = goldenHeader();

    const auto result = detail::createRunJournalV1WithHooks(directory.journalPath(), header, hooksFor(script));

    expectSystemFailure(result, RunJournalCreateOutcome::UNCERTAIN, EIO);
    EXPECT_EQ(script.syncCalls, 2);
    EXPECT_EQ(script.closeCalls, 2);
    RunHeaderV1 decoded{};
    ASSERT_EQ(decodeRunHeaderV1(readBytes(directory.journalPath()), decoded), RunHeaderCodecError::NONE);
    EXPECT_EQ(decoded, header);
}

} // namespace
} // namespace exchange::storage
