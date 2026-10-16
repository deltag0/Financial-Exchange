#include "run_journal_recovery.hpp"

#include "run_journal_recovery_internal.hpp"
#include "run_journal_writer_internal.hpp"

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

class RecoveryTestDirectory final {
public:
    RecoveryTestDirectory() {
        std::array<char, 52> pathTemplate{};
        constexpr char TEMPLATE[] = "/tmp/exchange-journal-recovery-test-XXXXXX";
        std::copy(std::begin(TEMPLATE), std::end(TEMPLATE), pathTemplate.begin());
        char *created = ::mkdtemp(pathTemplate.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~RecoveryTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    RecoveryTestDirectory(const RecoveryTestDirectory &) = delete;
    RecoveryTestDirectory &operator=(const RecoveryTestDirectory &) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return !path_.empty();
    }

    [[nodiscard]] std::filesystem::path journalPath(const std::string &name = "run-42.fxjr") const {
        return path_ / name;
    }

private:
    std::filesystem::path path_{};
};

RunHeaderV1 recoveryHeader() {
    return {
        .exchangeRunId = domain::ExchangeRunId{42},
        .behavioralRulesVersion = 1,
        .maxEventsPerCommand = 4'096,
        .maxRunCommands = 10,
        .maxRunJournalBytes = 64 * 1024,
        .instruments = {{domain::InstrumentId{100}, 7}, {domain::InstrumentId{200}, 9}},
    };
}

JournalNewOrderV1 recoveryNewOrder(const std::uint64_t sequence = 1) {
    return {
        .exchangeRunId = domain::ExchangeRunId{42},
        .commandSequence = domain::CommandSequence{sequence},
        .behavioralRulesVersion = 1,
        .configurationVersion = 7,
        .clientId = domain::ClientId{11},
        .instrumentId = domain::InstrumentId{100},
        .clientCommandId = domain::ClientCommandId{"new-order"},
        .side = domain::Side::BUY,
        .timeInForce = core::task::TimeInForce::GTC,
        .price = domain::Price{1234},
        .quantity = domain::Quantity{50},
    };
}

JournalCancelV1 recoveryCancel(const std::uint64_t sequence = 2) {
    return {
        .exchangeRunId = domain::ExchangeRunId{42},
        .commandSequence = domain::CommandSequence{sequence},
        .behavioralRulesVersion = 1,
        .configurationVersion = 9,
        .clientId = domain::ClientId{11},
        .instrumentId = domain::InstrumentId{200},
        .clientCommandId = domain::ClientCommandId{"cancel-order"},
        .targetOrderId = domain::TargetOrderId{1},
    };
}

std::vector<std::byte> encodedHeader() {
    std::vector<std::byte> bytes;
    EXPECT_EQ(encodeRunHeaderV1(recoveryHeader(), bytes), RunHeaderCodecError::NONE);
    return bytes;
}

std::vector<std::byte> encodedNewOrder(const std::uint64_t sequence = 1) {
    std::vector<std::byte> bytes;
    EXPECT_EQ(encodeNewOrderV1(recoveryNewOrder(sequence), bytes), JournalCommandCodecError::NONE);
    return bytes;
}

std::vector<std::byte> encodedCancel(const std::uint64_t sequence = 2) {
    std::vector<std::byte> bytes;
    EXPECT_EQ(encodeCancelV1(recoveryCancel(sequence), bytes), JournalCommandCodecError::NONE);
    return bytes;
}

void append(std::vector<std::byte> &destination, const std::span<const std::byte> source) {
    destination.insert(destination.end(), source.begin(), source.end());
}

void writeBytes(const std::filesystem::path &path, const std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open());
    stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(stream.good());
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

PreparedRunJournalV1 sentinelPreparedJournal() {
    PreparedRunJournalV1 output;
    output.journal.validCommittedByteCount = 777;
    return output;
}

struct RecoverySyscallScript final {
    int readCalls{0};
    int seekCalls{0};
    int truncateCalls{0};
    int syncCalls{0};
    int closeCalls{0};
    int writerWriteCalls{0};
    int readerDescriptor{-1};
    int writerDescriptor{-1};
    int failingReadCall{0};
    int readError{EIO};
    int failingSeekCall{0};
    int seekError{EIO};
    bool failTailRead{false};
    int tailReadError{EIO};
    int failingTruncateCall{0};
    int truncateError{EIO};
    int failingSyncCall{0};
    int syncError{EIO};
    bool interruptFirstSync{false};
    bool failClose{false};
    int closeError{EIO};
    bool tailReadPhase{false};
    std::array<char, 128> operations{};
    std::size_t operationCount{0};

    void record(const char operation) noexcept {
        if (operationCount < operations.size()) {
            operations[operationCount++] = operation;
        }
    }
};

ssize_t scriptedRecoveryRead(void *context, const int descriptor, void *buffer, const std::size_t size) noexcept {
    auto &script = *static_cast<RecoverySyscallScript *>(context);
    ++script.readCalls;
    script.readerDescriptor = descriptor;
    script.record('R');
    if (script.readCalls == script.failingReadCall) {
        errno = script.readError;
        return -1;
    }
    if (script.tailReadPhase && script.failTailRead) {
        errno = script.tailReadError;
        return -1;
    }
    return ::read(descriptor, buffer, size);
}

off_t scriptedRecoverySeek(void *context, const int descriptor, const off_t offset, const int whence) noexcept {
    auto &script = *static_cast<RecoverySyscallScript *>(context);
    ++script.seekCalls;
    script.record('S');
    if (script.seekCalls == script.failingSeekCall) {
        errno = script.seekError;
        return -1;
    }
    const off_t result = ::lseek(descriptor, offset, whence);
    if (result >= 0) {
        script.tailReadPhase = true;
    }
    return result;
}

int scriptedRecoveryTruncate(void *context, const int descriptor, const off_t length) noexcept {
    auto &script = *static_cast<RecoverySyscallScript *>(context);
    ++script.truncateCalls;
    script.record('T');
    if (script.truncateCalls == script.failingTruncateCall) {
        errno = script.truncateError;
        return -1;
    }
    return ::ftruncate(descriptor, length);
}

int scriptedRecoverySync(void *context, const int descriptor) noexcept {
    auto &script = *static_cast<RecoverySyscallScript *>(context);
    ++script.syncCalls;
    script.record('Y');
    if (script.interruptFirstSync && script.syncCalls == 1) {
        errno = EINTR;
        return -1;
    }
    if (script.syncCalls == script.failingSyncCall) {
        errno = script.syncError;
        return -1;
    }
    return ::fsync(descriptor);
}

int scriptedRecoveryClose(void *context, const int descriptor) noexcept {
    auto &script = *static_cast<RecoverySyscallScript *>(context);
    ++script.closeCalls;
    script.record('C');
    const int result = ::close(descriptor);
    if (script.failClose) {
        errno = script.closeError;
        return -1;
    }
    return result;
}

ssize_t scriptedWriterWrite(void *context, const int descriptor, const void *buffer, const std::size_t size) noexcept {
    auto &script = *static_cast<RecoverySyscallScript *>(context);
    ++script.writerWriteCalls;
    script.writerDescriptor = descriptor;
    return ::write(descriptor, buffer, size);
}

int scriptedWriterSync(void *, const int descriptor) noexcept {
    return ::fdatasync(descriptor);
}

detail::RunJournalRecoveryHooks hooksFor(RecoverySyscallScript &script) {
    return {&script,
            scriptedRecoveryRead,
            scriptedRecoverySeek,
            scriptedRecoveryTruncate,
            scriptedRecoverySync,
            scriptedRecoveryClose};
}

RunJournalRecoveryResult prepareWithScript(const std::filesystem::path &path, RecoverySyscallScript &script,
                                           PreparedRunJournalV1 &output) {
    const detail::RunJournalWriterHooks writerHooks{&script, scriptedWriterWrite, scriptedWriterSync};
    return detail::prepareRunJournalV1WithHooks(path, hooksFor(script), writerHooks, output);
}

TEST(RunJournalRecoveryTest, ReopensValidJournalWithoutModificationAndAppendsAtExactRecoveredState) {
    RecoveryTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    auto bytes = encodedHeader();
    const auto first = encodedNewOrder();
    const auto second = encodedCancel();
    append(bytes, first);
    append(bytes, second);
    writeBytes(directory.journalPath(), bytes);
    const auto originalBytes = bytes;
    PreparedRunJournalV1 prepared;

    const auto result = prepareRunJournalV1(directory.journalPath(), prepared);

    EXPECT_EQ(result.outcome, RunJournalRecoveryOutcome::PREPARED);
    EXPECT_EQ(result.validation.outcome, RunJournalLoadOutcome::VALID);
    EXPECT_TRUE(result.preservedTailBytes.empty());
    ASSERT_NE(prepared.writer, nullptr);
    EXPECT_EQ(prepared.journal.commands.size(), 2U);
    EXPECT_EQ(prepared.writer->committedCommandCount(), 2U);
    EXPECT_EQ(prepared.writer->committedByteCount(), originalBytes.size());
    EXPECT_EQ(prepared.writer->nextCommandSequence(), domain::CommandSequence{3});
    EXPECT_EQ(readBytes(directory.journalPath()), originalBytes);

    const auto third = encodedNewOrder(3);
    EXPECT_EQ(prepared.writer->append(recoveryNewOrder(3)).outcome, RunJournalAppendOutcome::COMMITTED);
    prepared.writer.reset();
    append(bytes, third);
    EXPECT_EQ(readBytes(directory.journalPath()), bytes);
}

TEST(RunJournalRecoveryTest, PreservesRepairsAndReopensIncompleteTailBeforeAppending) {
    RecoveryTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    auto committedBytes = encodedHeader();
    append(committedBytes, encodedNewOrder());
    auto physicalBytes = committedBytes;
    const auto nextFrame = encodedCancel();
    const auto tail = std::span<const std::byte>(nextFrame).first(17);
    append(physicalBytes, tail);
    writeBytes(directory.journalPath(), physicalBytes);
    RecoverySyscallScript script;
    script.interruptFirstSync = true;
    PreparedRunJournalV1 prepared;

    const auto result = prepareWithScript(directory.journalPath(), script, prepared);

    EXPECT_EQ(result.outcome, RunJournalRecoveryOutcome::PREPARED);
    EXPECT_EQ(result.validation.outcome, RunJournalLoadOutcome::INCOMPLETE_TAIL);
    EXPECT_EQ(result.preservedTailBytes, std::vector<std::byte>(tail.begin(), tail.end()));
    EXPECT_EQ(readBytes(directory.journalPath()), committedBytes);
    ASSERT_NE(prepared.writer, nullptr);
    EXPECT_EQ(prepared.writer->committedCommandCount(), 1U);
    EXPECT_EQ(prepared.writer->committedByteCount(), committedBytes.size());
    EXPECT_EQ(prepared.writer->nextCommandSequence(), domain::CommandSequence{2});
    EXPECT_EQ(script.truncateCalls, 1);
    EXPECT_EQ(script.syncCalls, 2);
    ASSERT_GE(script.operationCount, 5U);
    const auto operationEnd = script.operations.begin() + static_cast<std::ptrdiff_t>(script.operationCount);
    constexpr std::array EXPECTED_REPAIR_OPERATIONS{'S', 'R', 'T', 'Y', 'Y', 'S'};
    EXPECT_TRUE(std::search(script.operations.begin(), operationEnd, EXPECTED_REPAIR_OPERATIONS.begin(),
                            EXPECTED_REPAIR_OPERATIONS.end()) != operationEnd);

    EXPECT_EQ(prepared.writer->append(recoveryCancel()).outcome, RunJournalAppendOutcome::COMMITTED);
    EXPECT_EQ(script.writerWriteCalls, 1);
    EXPECT_EQ(script.writerDescriptor, script.readerDescriptor);
    prepared.writer.reset();
    append(committedBytes, nextFrame);
    EXPECT_EQ(readBytes(directory.journalPath()), committedBytes);
}

TEST(RunJournalRecoveryTest, MissingIoAndCorruptJournalsReturnNoWriterAndPreserveCorruptBytes) {
    RecoveryTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    PreparedRunJournalV1 missingOutput;
    const auto missing = prepareRunJournalV1(directory.journalPath("missing.fxjr"), missingOutput);
    EXPECT_EQ(missing.outcome, RunJournalRecoveryOutcome::MISSING);
    EXPECT_EQ(missing.systemError, ENOENT);
    EXPECT_EQ(missingOutput.writer, nullptr);

    PreparedRunJournalV1 ioOutput;
    const auto io = prepareRunJournalV1(directory.journalPath("."), ioOutput);
    EXPECT_EQ(io.outcome, RunJournalRecoveryOutcome::IO_FAILURE);
    EXPECT_NE(io.systemError, 0);
    EXPECT_EQ(ioOutput.writer, nullptr);

    const auto validBytes = encodedHeader();
    writeBytes(directory.journalPath("read-error.fxjr"), validBytes);
    RecoverySyscallScript readScript;
    readScript.failingReadCall = 1;
    readScript.readError = EIO;
    auto readOutput = sentinelPreparedJournal();
    const auto readFailure = prepareWithScript(directory.journalPath("read-error.fxjr"), readScript, readOutput);
    EXPECT_EQ(readFailure.outcome, RunJournalRecoveryOutcome::IO_FAILURE);
    EXPECT_EQ(readFailure.validation.outcome, RunJournalLoadOutcome::IO_FAILURE);
    EXPECT_EQ(readFailure.systemError, EIO);
    EXPECT_EQ(readOutput.writer, nullptr);
    EXPECT_EQ(readOutput.journal.validCommittedByteCount, 777U);
    EXPECT_EQ(readBytes(directory.journalPath("read-error.fxjr")), validBytes);

    auto corruptBytes = encodedHeader();
    auto command = encodedNewOrder();
    command.back() ^= std::byte{1};
    append(corruptBytes, command);
    writeBytes(directory.journalPath(), corruptBytes);
    auto corruptOutput = sentinelPreparedJournal();
    const auto corruption = prepareRunJournalV1(directory.journalPath(), corruptOutput);
    EXPECT_EQ(corruption.outcome, RunJournalRecoveryOutcome::CORRUPTION);
    EXPECT_EQ(corruption.validation.outcome, RunJournalLoadOutcome::CORRUPTION);
    EXPECT_EQ(corruption.validation.commandCodecError, JournalCommandCodecError::CHECKSUM_MISMATCH);
    EXPECT_EQ(corruptOutput.writer, nullptr);
    EXPECT_EQ(corruptOutput.journal.validCommittedByteCount, 777U);
    EXPECT_EQ(readBytes(directory.journalPath()), corruptBytes);
}

TEST(RunJournalRecoveryTest, TailPreservationSeekAndReadFailuresDoNotModifyTheFile) {
    RecoveryTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    auto bytes = encodedHeader();
    const auto command = encodedNewOrder();
    append(bytes, std::span<const std::byte>(command).first(11));

    writeBytes(directory.journalPath("seek.fxjr"), bytes);
    RecoverySyscallScript seekScript;
    seekScript.failingSeekCall = 1;
    seekScript.seekError = ESPIPE;
    auto seekOutput = sentinelPreparedJournal();
    const auto seekResult = prepareWithScript(directory.journalPath("seek.fxjr"), seekScript, seekOutput);
    EXPECT_EQ(seekResult.outcome, RunJournalRecoveryOutcome::IO_FAILURE);
    EXPECT_EQ(seekResult.error, RunJournalRecoveryError::TAIL_PRESERVATION_FAILED);
    EXPECT_EQ(seekResult.systemError, ESPIPE);
    EXPECT_EQ(seekOutput.writer, nullptr);
    EXPECT_EQ(readBytes(directory.journalPath("seek.fxjr")), bytes);

    writeBytes(directory.journalPath("read.fxjr"), bytes);
    RecoverySyscallScript readScript;
    readScript.failTailRead = true;
    readScript.tailReadError = EIO;
    auto readOutput = sentinelPreparedJournal();
    const auto readResult = prepareWithScript(directory.journalPath("read.fxjr"), readScript, readOutput);
    EXPECT_EQ(readResult.outcome, RunJournalRecoveryOutcome::IO_FAILURE);
    EXPECT_EQ(readResult.error, RunJournalRecoveryError::TAIL_PRESERVATION_FAILED);
    EXPECT_EQ(readResult.systemError, EIO);
    EXPECT_EQ(readOutput.writer, nullptr);
    EXPECT_EQ(readBytes(directory.journalPath("read.fxjr")), bytes);
}

TEST(RunJournalRecoveryTest, TruncateFailureIsRecoveryRequiredAndReturnsPreservedTail) {
    RecoveryTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    auto bytes = encodedHeader();
    const auto command = encodedNewOrder();
    const auto tail = std::span<const std::byte>(command).first(13);
    append(bytes, tail);
    writeBytes(directory.journalPath(), bytes);
    RecoverySyscallScript script;
    script.failingTruncateCall = 1;
    script.truncateError = EIO;
    auto output = sentinelPreparedJournal();

    const auto result = prepareWithScript(directory.journalPath(), script, output);

    EXPECT_EQ(result.outcome, RunJournalRecoveryOutcome::RECOVERY_REQUIRED);
    EXPECT_EQ(result.error, RunJournalRecoveryError::TRUNCATE_FAILED);
    EXPECT_EQ(result.systemError, EIO);
    EXPECT_EQ(result.preservedTailBytes, std::vector<std::byte>(tail.begin(), tail.end()));
    EXPECT_EQ(output.writer, nullptr);
    EXPECT_EQ(output.journal.validCommittedByteCount, 777U);
    EXPECT_EQ(readBytes(directory.journalPath()), bytes);
}

TEST(RunJournalRecoveryTest, SyncFailureAfterTruncationIsRecoveryRequiredAndNoWriterEscapes) {
    RecoveryTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto committedBytes = encodedHeader();
    auto physicalBytes = committedBytes;
    const auto command = encodedNewOrder();
    const auto tail = std::span<const std::byte>(command).first(15);
    append(physicalBytes, tail);
    writeBytes(directory.journalPath(), physicalBytes);
    RecoverySyscallScript script;
    script.failingSyncCall = 1;
    script.syncError = ENOSPC;
    auto output = sentinelPreparedJournal();

    const auto result = prepareWithScript(directory.journalPath(), script, output);

    EXPECT_EQ(result.outcome, RunJournalRecoveryOutcome::RECOVERY_REQUIRED);
    EXPECT_EQ(result.error, RunJournalRecoveryError::SYNC_FAILED);
    EXPECT_EQ(result.systemError, ENOSPC);
    EXPECT_EQ(result.preservedTailBytes, std::vector<std::byte>(tail.begin(), tail.end()));
    EXPECT_EQ(output.writer, nullptr);
    EXPECT_EQ(readBytes(directory.journalPath()), committedBytes);
}

TEST(RunJournalRecoveryTest, FinalSeekFailuresAreClassifiedByWhetherTruncationBegan) {
    RecoveryTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto validBytes = encodedHeader();
    writeBytes(directory.journalPath("valid.fxjr"), validBytes);
    RecoverySyscallScript validScript;
    validScript.failingSeekCall = 1;
    validScript.seekError = ESPIPE;
    PreparedRunJournalV1 validOutput;
    const auto validResult = prepareWithScript(directory.journalPath("valid.fxjr"), validScript, validOutput);
    EXPECT_EQ(validResult.outcome, RunJournalRecoveryOutcome::IO_FAILURE);
    EXPECT_EQ(validResult.error, RunJournalRecoveryError::SEEK_FAILED);
    EXPECT_EQ(validOutput.writer, nullptr);
    EXPECT_EQ(readBytes(directory.journalPath("valid.fxjr")), validBytes);

    auto tailBytes = validBytes;
    const auto command = encodedNewOrder();
    append(tailBytes, std::span<const std::byte>(command).first(9));
    writeBytes(directory.journalPath("tail.fxjr"), tailBytes);
    RecoverySyscallScript tailScript;
    tailScript.failingSeekCall = 2;
    tailScript.seekError = ESPIPE;
    PreparedRunJournalV1 tailOutput;
    const auto tailResult = prepareWithScript(directory.journalPath("tail.fxjr"), tailScript, tailOutput);
    EXPECT_EQ(tailResult.outcome, RunJournalRecoveryOutcome::RECOVERY_REQUIRED);
    EXPECT_EQ(tailResult.error, RunJournalRecoveryError::SEEK_FAILED);
    EXPECT_EQ(tailOutput.writer, nullptr);
    EXPECT_EQ(readBytes(directory.journalPath("tail.fxjr")), validBytes);
}

TEST(RunJournalRecoveryTest, CloseFailureOverridesAValidationFailureAndNoWriterEscapes) {
    RecoveryTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    auto bytes = encodedHeader();
    auto command = encodedNewOrder();
    command.back() ^= std::byte{1};
    append(bytes, command);
    writeBytes(directory.journalPath(), bytes);
    RecoverySyscallScript script;
    script.failClose = true;
    script.closeError = EBADF;
    PreparedRunJournalV1 output;

    const auto result = prepareWithScript(directory.journalPath(), script, output);

    EXPECT_EQ(result.outcome, RunJournalRecoveryOutcome::IO_FAILURE);
    EXPECT_EQ(result.error, RunJournalRecoveryError::CLOSE_FAILED);
    EXPECT_EQ(result.systemError, EBADF);
    EXPECT_EQ(result.validation.outcome, RunJournalLoadOutcome::CORRUPTION);
    EXPECT_EQ(output.writer, nullptr);
    EXPECT_EQ(readBytes(directory.journalPath()), bytes);

    const auto committedBytes = encodedHeader();
    auto tailBytes = committedBytes;
    const auto validCommand = encodedNewOrder();
    const auto tail = std::span<const std::byte>(validCommand).first(7);
    append(tailBytes, tail);
    writeBytes(directory.journalPath("tail-close.fxjr"), tailBytes);
    RecoverySyscallScript postTruncateScript;
    postTruncateScript.failingSyncCall = 1;
    postTruncateScript.failClose = true;
    postTruncateScript.closeError = EBADF;
    PreparedRunJournalV1 postTruncateOutput;

    const auto postTruncateResult =
        prepareWithScript(directory.journalPath("tail-close.fxjr"), postTruncateScript, postTruncateOutput);

    EXPECT_EQ(postTruncateResult.outcome, RunJournalRecoveryOutcome::RECOVERY_REQUIRED);
    EXPECT_EQ(postTruncateResult.error, RunJournalRecoveryError::CLOSE_FAILED);
    EXPECT_EQ(postTruncateResult.systemError, EBADF);
    EXPECT_EQ(postTruncateResult.preservedTailBytes, std::vector<std::byte>(tail.begin(), tail.end()));
    EXPECT_EQ(postTruncateOutput.writer, nullptr);
    EXPECT_EQ(readBytes(directory.journalPath("tail-close.fxjr")), committedBytes);
}

} // namespace
} // namespace exchange::storage
