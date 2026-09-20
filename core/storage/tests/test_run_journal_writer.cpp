#include "run_journal_writer.hpp"

#include "run_journal_storage.hpp"
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
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace exchange::storage {
namespace {

static_assert(!std::is_copy_constructible_v<RunJournalWriterV1>);
static_assert(!std::is_copy_assignable_v<RunJournalWriterV1>);

class WriterTestDirectory final {
public:
    WriterTestDirectory() {
        std::array<char, 48> pathTemplate{};
        constexpr char TEMPLATE[] = "/tmp/exchange-journal-writer-test-XXXXXX";
        std::copy(std::begin(TEMPLATE), std::end(TEMPLATE), pathTemplate.begin());
        char *created = ::mkdtemp(pathTemplate.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~WriterTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    WriterTestDirectory(const WriterTestDirectory &) = delete;
    WriterTestDirectory &operator=(const WriterTestDirectory &) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return !path_.empty();
    }

    [[nodiscard]] std::filesystem::path journalPath(const std::string &name = "run-42.fxjr") const {
        return path_ / name;
    }

private:
    std::filesystem::path path_{};
};

RunHeaderV1 writerHeader(const std::uint64_t maxCommands = 10, const std::uint64_t maxBytes = 64 * 1024) {
    return {
        .exchangeRunId = domain::ExchangeRunId{42},
        .behavioralRulesVersion = 1,
        .maxEventsPerCommand = 4'096,
        .maxRunCommands = maxCommands,
        .maxRunJournalBytes = maxBytes,
        .instruments = {{domain::InstrumentId{100}, 7}, {domain::InstrumentId{200}, 9}},
    };
}

JournalNewOrderV1 newOrder(const std::uint64_t sequence = 1, const std::string_view clientCommandId = "new-1") {
    return {
        .exchangeRunId = domain::ExchangeRunId{42},
        .commandSequence = domain::CommandSequence{sequence},
        .behavioralRulesVersion = 1,
        .configurationVersion = 7,
        .clientId = domain::ClientId{11},
        .instrumentId = domain::InstrumentId{100},
        .clientCommandId = domain::ClientCommandId{clientCommandId},
        .side = domain::Side::BUY,
        .timeInForce = core::task::TimeInForce::GTC,
        .price = domain::Price{1234},
        .quantity = domain::Quantity{50},
    };
}

JournalCancelV1 cancel(const std::uint64_t sequence = 2) {
    return {
        .exchangeRunId = domain::ExchangeRunId{42},
        .commandSequence = domain::CommandSequence{sequence},
        .behavioralRulesVersion = 1,
        .configurationVersion = 9,
        .clientId = domain::ClientId{11},
        .instrumentId = domain::InstrumentId{200},
        .clientCommandId = domain::ClientCommandId{"cancel-1"},
        .targetOrderId = domain::TargetOrderId{1},
    };
}

std::vector<std::byte> readJournal(const std::filesystem::path &path) {
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

struct WriterSyscallScript final {
    int writeCalls{0};
    int syncCalls{0};
    bool interruptFirstWrite{false};
    bool interruptFirstSync{false};
    int zeroWriteCall{0};
    int failingWriteCall{0};
    int writeError{EIO};
    int failingSyncCall{0};
    int syncError{EIO};
    std::size_t maximumWrite{std::numeric_limits<std::size_t>::max()};
    RunJournalWriterV1 *observedWriter{nullptr};
    bool observedStateDuringSync{false};
    std::uint64_t observedCommandCount{0};
    std::uint64_t observedByteCount{0};
    domain::CommandSequence observedNextSequence{};
};

ssize_t scriptedWrite(void *context, const int descriptor, const void *buffer, const std::size_t size) noexcept {
    auto &script = *static_cast<WriterSyscallScript *>(context);
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
    auto &script = *static_cast<WriterSyscallScript *>(context);
    ++script.syncCalls;
    if (script.observedWriter != nullptr) {
        script.observedStateDuringSync = true;
        script.observedCommandCount = script.observedWriter->committedCommandCount();
        script.observedByteCount = script.observedWriter->committedByteCount();
        script.observedNextSequence = script.observedWriter->nextCommandSequence();
    }
    if (script.interruptFirstSync && script.syncCalls == 1) {
        errno = EINTR;
        return -1;
    }
    if (script.syncCalls == script.failingSyncCall) {
        errno = script.syncError;
        return -1;
    }
    return ::fdatasync(descriptor);
}

detail::RunJournalWriterHooks hooksFor(WriterSyscallScript &script) {
    return {&script, scriptedWrite, scriptedSync};
}

std::unique_ptr<RunJournalWriterV1> createWriter(const std::filesystem::path &path, const RunHeaderV1 &header,
                                                 WriterSyscallScript &script) {
    std::unique_ptr<RunJournalWriterV1> writer;
    const auto createResult = detail::createRunJournalWriterV1WithHooks(path, header, hooksFor(script), writer);
    EXPECT_EQ(createResult.outcome, RunJournalCreateOutcome::CREATED);
    EXPECT_EQ(createResult.codecError, RunHeaderCodecError::NONE);
    EXPECT_EQ(createResult.systemError, 0);
    EXPECT_NE(writer, nullptr);
    return writer;
}

void expectAppendResult(const RunJournalAppendResult &result, const RunJournalAppendOutcome outcome,
                        const RunJournalAppendError error = RunJournalAppendError::NONE,
                        const JournalCommandCodecError codecError = JournalCommandCodecError::NONE,
                        const int systemError = 0) {
    EXPECT_EQ(result.outcome, outcome);
    EXPECT_EQ(result.error, error);
    EXPECT_EQ(result.codecError, codecError);
    EXPECT_EQ(result.systemError, systemError);
}

TEST(RunJournalWriterTest, AppendsExactNewOrderAndCancelFramesWithConsecutiveSequences) {
    WriterTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto header = writerHeader();
    std::vector<std::byte> encodedHeader;
    ASSERT_EQ(encodeRunHeaderV1(header, encodedHeader), RunHeaderCodecError::NONE);
    const auto first = newOrder();
    std::vector<std::byte> encodedFirst;
    ASSERT_EQ(encodeNewOrderV1(first, encodedFirst), JournalCommandCodecError::NONE);
    const auto second = cancel();
    std::vector<std::byte> encodedSecond;
    ASSERT_EQ(encodeCancelV1(second, encodedSecond), JournalCommandCodecError::NONE);
    WriterSyscallScript script;
    auto writer = createWriter(directory.journalPath(), header, script);
    ASSERT_NE(writer, nullptr);

    expectAppendResult(writer->append(first), RunJournalAppendOutcome::COMMITTED);
    expectAppendResult(writer->append(second), RunJournalAppendOutcome::COMMITTED);

    EXPECT_EQ(writer->committedCommandCount(), 2U);
    EXPECT_EQ(writer->committedByteCount(), encodedHeader.size() + encodedFirst.size() + encodedSecond.size());
    EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{3});
    EXPECT_EQ(script.syncCalls, 2);
    writer.reset();

    auto expected = encodedHeader;
    expected.insert(expected.end(), encodedFirst.begin(), encodedFirst.end());
    expected.insert(expected.end(), encodedSecond.begin(), encodedSecond.end());
    EXPECT_EQ(readJournal(directory.journalPath()), expected);
}

TEST(RunJournalWriterTest, RejectsCrossHeaderMismatchesWithoutWritingOrAdvancing) {
    WriterTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto header = writerHeader();
    std::vector<std::byte> encodedHeader;
    ASSERT_EQ(encodeRunHeaderV1(header, encodedHeader), RunHeaderCodecError::NONE);
    WriterSyscallScript script;
    auto writer = createWriter(directory.journalPath(), header, script);
    ASSERT_NE(writer, nullptr);

    auto command = newOrder(2);
    expectAppendResult(writer->append(command), RunJournalAppendOutcome::INVALID_COMMAND,
                       RunJournalAppendError::UNEXPECTED_COMMAND_SEQUENCE);
    command = newOrder();
    command.exchangeRunId = domain::ExchangeRunId{43};
    expectAppendResult(writer->append(command), RunJournalAppendOutcome::INVALID_COMMAND,
                       RunJournalAppendError::EXCHANGE_RUN_ID_MISMATCH);
    command = newOrder();
    command.behavioralRulesVersion = 2;
    expectAppendResult(writer->append(command), RunJournalAppendOutcome::INVALID_COMMAND,
                       RunJournalAppendError::BEHAVIORAL_RULES_VERSION_MISMATCH);
    command = newOrder();
    command.instrumentId = domain::InstrumentId{300};
    expectAppendResult(writer->append(command), RunJournalAppendOutcome::INVALID_COMMAND,
                       RunJournalAppendError::INSTRUMENT_CONFIGURATION_MISMATCH);
    command = newOrder();
    command.configurationVersion = 8;
    expectAppendResult(writer->append(command), RunJournalAppendOutcome::INVALID_COMMAND,
                       RunJournalAppendError::INSTRUMENT_CONFIGURATION_MISMATCH);

    EXPECT_EQ(script.writeCalls, 0);
    EXPECT_EQ(script.syncCalls, 0);
    EXPECT_EQ(writer->committedCommandCount(), 0U);
    EXPECT_EQ(writer->committedByteCount(), encodedHeader.size());
    EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{1});
    EXPECT_EQ(readJournal(directory.journalPath()), encodedHeader);
}

TEST(RunJournalWriterTest, RejectsCodecInvalidCommandWithoutWritingOrConsumingSequence) {
    WriterTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto header = writerHeader();
    WriterSyscallScript script;
    auto writer = createWriter(directory.journalPath(), header, script);
    ASSERT_NE(writer, nullptr);
    auto command = newOrder();
    command.quantity = domain::Quantity{0};

    expectAppendResult(writer->append(command), RunJournalAppendOutcome::INVALID_COMMAND, RunJournalAppendError::NONE,
                       JournalCommandCodecError::ZERO_QUANTITY);

    EXPECT_EQ(script.writeCalls, 0);
    EXPECT_EQ(script.syncCalls, 0);
    EXPECT_EQ(writer->committedCommandCount(), 0U);
    EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{1});
}

TEST(RunJournalWriterTest, ExistingSameLengthDifferentHeaderCannotCreateWriter) {
    WriterTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto originalHeader = writerHeader();
    WriterSyscallScript originalScript;
    auto originalWriter = createWriter(directory.journalPath(), originalHeader, originalScript);
    ASSERT_NE(originalWriter, nullptr);
    originalWriter.reset();
    const auto originalBytes = readJournal(directory.journalPath());

    auto differentHeader = originalHeader;
    differentHeader.exchangeRunId = domain::ExchangeRunId{43};
    std::vector<std::byte> originalEncoded;
    std::vector<std::byte> differentEncoded;
    ASSERT_EQ(encodeRunHeaderV1(originalHeader, originalEncoded), RunHeaderCodecError::NONE);
    ASSERT_EQ(encodeRunHeaderV1(differentHeader, differentEncoded), RunHeaderCodecError::NONE);
    ASSERT_EQ(originalEncoded.size(), differentEncoded.size());
    ASSERT_NE(originalEncoded, differentEncoded);

    std::unique_ptr<RunJournalWriterV1> rejectedWriter;
    const auto result = createRunJournalWriterV1(directory.journalPath(), differentHeader, rejectedWriter);

    EXPECT_EQ(result.outcome, RunJournalCreateOutcome::PATH_ALREADY_EXISTS);
    EXPECT_EQ(result.codecError, RunHeaderCodecError::NONE);
    EXPECT_EQ(result.systemError, EEXIST);
    EXPECT_EQ(rejectedWriter, nullptr);
    EXPECT_EQ(readJournal(directory.journalPath()), originalBytes);
}

TEST(RunJournalWriterTest, EnforcesExactCommandAndByteCapacityBoundariesWithoutWriting) {
    WriterTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto command = newOrder();
    std::vector<std::byte> encodedCommand;
    ASSERT_EQ(encodeNewOrderV1(command, encodedCommand), JournalCommandCodecError::NONE);

    auto exactHeader = writerHeader(2);
    std::vector<std::byte> encodedHeader;
    ASSERT_EQ(encodeRunHeaderV1(exactHeader, encodedHeader), RunHeaderCodecError::NONE);
    exactHeader.maxRunJournalBytes = encodedHeader.size() + encodedCommand.size();
    WriterSyscallScript exactScript;
    auto exactWriter = createWriter(directory.journalPath("exact.fxjr"), exactHeader, exactScript);
    ASSERT_NE(exactWriter, nullptr);

    expectAppendResult(exactWriter->append(command), RunJournalAppendOutcome::COMMITTED);
    expectAppendResult(exactWriter->append(cancel(2)), RunJournalAppendOutcome::CAPACITY_REACHED);
    EXPECT_EQ(exactScript.writeCalls, 1);
    EXPECT_EQ(exactWriter->committedCommandCount(), 1U);
    EXPECT_EQ(exactWriter->nextCommandSequence(), domain::CommandSequence{2});

    auto shortHeader = exactHeader;
    --shortHeader.maxRunJournalBytes;
    WriterSyscallScript shortScript;
    auto shortWriter = createWriter(directory.journalPath("short.fxjr"), shortHeader, shortScript);
    ASSERT_NE(shortWriter, nullptr);
    expectAppendResult(shortWriter->append(command), RunJournalAppendOutcome::CAPACITY_REACHED);
    EXPECT_EQ(shortScript.writeCalls, 0);
    EXPECT_EQ(shortWriter->committedCommandCount(), 0U);

    const auto countHeader = writerHeader(1);
    WriterSyscallScript countScript;
    auto countWriter = createWriter(directory.journalPath("count.fxjr"), countHeader, countScript);
    ASSERT_NE(countWriter, nullptr);
    expectAppendResult(countWriter->append(command), RunJournalAppendOutcome::COMMITTED);
    expectAppendResult(countWriter->append(cancel(2)), RunJournalAppendOutcome::CAPACITY_REACHED);
    EXPECT_EQ(countScript.writeCalls, 1);
}

TEST(RunJournalWriterTest, RetriesInterruptedShortWritesAndSyncWithoutAdvancingEarly) {
    WriterTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto header = writerHeader();
    std::vector<std::byte> encodedHeader;
    ASSERT_EQ(encodeRunHeaderV1(header, encodedHeader), RunHeaderCodecError::NONE);
    const auto command = newOrder();
    std::vector<std::byte> encodedCommand;
    ASSERT_EQ(encodeNewOrderV1(command, encodedCommand), JournalCommandCodecError::NONE);
    WriterSyscallScript script;
    script.interruptFirstWrite = true;
    script.interruptFirstSync = true;
    script.maximumWrite = 5;
    auto writer = createWriter(directory.journalPath(), header, script);
    ASSERT_NE(writer, nullptr);
    script.observedWriter = writer.get();

    expectAppendResult(writer->append(command), RunJournalAppendOutcome::COMMITTED);

    EXPECT_GT(script.writeCalls, 2);
    EXPECT_EQ(script.syncCalls, 2);
    EXPECT_TRUE(script.observedStateDuringSync);
    EXPECT_EQ(script.observedCommandCount, 0U);
    EXPECT_EQ(script.observedByteCount, encodedHeader.size());
    EXPECT_EQ(script.observedNextSequence, domain::CommandSequence{1});
    EXPECT_EQ(writer->committedCommandCount(), 1U);
    EXPECT_EQ(writer->committedByteCount(), encodedHeader.size() + encodedCommand.size());
    EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{2});
}

TEST(RunJournalWriterTest, ZeroOrImmediateErrorWriteIsDefiniteAndPoisonsWriter) {
    WriterTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto header = writerHeader();

    WriterSyscallScript zeroScript;
    zeroScript.zeroWriteCall = 1;
    auto zeroWriter = createWriter(directory.journalPath("zero.fxjr"), header, zeroScript);
    ASSERT_NE(zeroWriter, nullptr);
    expectAppendResult(zeroWriter->append(newOrder()), RunJournalAppendOutcome::IO_FAILURE, RunJournalAppendError::NONE,
                       JournalCommandCodecError::NONE, EIO);
    const int zeroWriteCalls = zeroScript.writeCalls;
    expectAppendResult(zeroWriter->append(newOrder()), RunJournalAppendOutcome::RECOVERY_REQUIRED,
                       RunJournalAppendError::WRITER_UNUSABLE);
    EXPECT_EQ(zeroScript.writeCalls, zeroWriteCalls);
    EXPECT_EQ(zeroWriter->committedCommandCount(), 0U);
    EXPECT_EQ(zeroWriter->nextCommandSequence(), domain::CommandSequence{1});

    WriterSyscallScript errorScript;
    errorScript.failingWriteCall = 1;
    errorScript.writeError = ENOSPC;
    auto errorWriter = createWriter(directory.journalPath("error.fxjr"), header, errorScript);
    ASSERT_NE(errorWriter, nullptr);
    expectAppendResult(errorWriter->append(newOrder()), RunJournalAppendOutcome::IO_FAILURE,
                       RunJournalAppendError::NONE, JournalCommandCodecError::NONE, ENOSPC);
    EXPECT_EQ(errorWriter->committedCommandCount(), 0U);
}

TEST(RunJournalWriterTest, PartialWriteFailureRequiresRecoveryAndRetainsTailWithoutAdvancing) {
    WriterTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto header = writerHeader();
    std::vector<std::byte> encodedHeader;
    ASSERT_EQ(encodeRunHeaderV1(header, encodedHeader), RunHeaderCodecError::NONE);
    WriterSyscallScript script;
    script.maximumWrite = 5;
    script.failingWriteCall = 2;
    script.writeError = ENOSPC;
    auto writer = createWriter(directory.journalPath(), header, script);
    ASSERT_NE(writer, nullptr);

    expectAppendResult(writer->append(newOrder()), RunJournalAppendOutcome::RECOVERY_REQUIRED,
                       RunJournalAppendError::NONE, JournalCommandCodecError::NONE, ENOSPC);

    EXPECT_EQ(writer->committedCommandCount(), 0U);
    EXPECT_EQ(writer->committedByteCount(), encodedHeader.size());
    EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{1});
    EXPECT_EQ(std::filesystem::file_size(directory.journalPath()), encodedHeader.size() + 5U);
}

TEST(RunJournalWriterTest, SyncFailureRequiresRecoveryAndDoesNotAdvanceCommittedState) {
    WriterTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto header = writerHeader();
    std::vector<std::byte> encodedHeader;
    ASSERT_EQ(encodeRunHeaderV1(header, encodedHeader), RunHeaderCodecError::NONE);
    const auto command = newOrder();
    std::vector<std::byte> encodedCommand;
    ASSERT_EQ(encodeNewOrderV1(command, encodedCommand), JournalCommandCodecError::NONE);
    WriterSyscallScript script;
    script.failingSyncCall = 1;
    script.syncError = EIO;
    auto writer = createWriter(directory.journalPath(), header, script);
    ASSERT_NE(writer, nullptr);

    expectAppendResult(writer->append(command), RunJournalAppendOutcome::RECOVERY_REQUIRED, RunJournalAppendError::NONE,
                       JournalCommandCodecError::NONE, EIO);

    EXPECT_EQ(writer->committedCommandCount(), 0U);
    EXPECT_EQ(writer->committedByteCount(), encodedHeader.size());
    EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{1});
    EXPECT_EQ(std::filesystem::file_size(directory.journalPath()), encodedHeader.size() + encodedCommand.size());
    const int writeCalls = script.writeCalls;
    const int syncCalls = script.syncCalls;
    expectAppendResult(writer->append(command), RunJournalAppendOutcome::RECOVERY_REQUIRED,
                       RunJournalAppendError::WRITER_UNUSABLE);
    EXPECT_EQ(script.writeCalls, writeCalls);
    EXPECT_EQ(script.syncCalls, syncCalls);
}

} // namespace
} // namespace exchange::storage
