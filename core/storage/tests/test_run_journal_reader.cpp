#include "run_journal_reader.hpp"

#include "journal_v1_envelope.hpp"
#include "little_endian.hpp"
#include "run_journal_reader_internal.hpp"

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
#include <variant>
#include <vector>

namespace exchange::storage {
namespace {

class ReaderTestDirectory final {
public:
    ReaderTestDirectory() {
        std::array<char, 48> pathTemplate{};
        constexpr char TEMPLATE[] = "/tmp/exchange-journal-reader-test-XXXXXX";
        std::copy(std::begin(TEMPLATE), std::end(TEMPLATE), pathTemplate.begin());
        char *created = ::mkdtemp(pathTemplate.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~ReaderTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    ReaderTestDirectory(const ReaderTestDirectory &) = delete;
    ReaderTestDirectory &operator=(const ReaderTestDirectory &) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return !path_.empty();
    }

    [[nodiscard]] std::filesystem::path journalPath(const std::string &name = "run-42.fxjr") const {
        return path_ / name;
    }

private:
    std::filesystem::path path_{};
};

RunHeaderV1 readerHeader(const std::uint64_t maxCommands = 10, const std::uint64_t maxBytes = 64 * 1024) {
    return {
        .exchangeRunId = domain::ExchangeRunId{42},
        .behavioralRulesVersion = 1,
        .maxEventsPerCommand = 4'096,
        .maxRunCommands = maxCommands,
        .maxRunJournalBytes = maxBytes,
        .instruments = {{domain::InstrumentId{100}, 7}, {domain::InstrumentId{200}, 9}},
    };
}

JournalNewOrderV1 readerNewOrder(const std::uint64_t sequence = 1) {
    return {
        .exchangeRunId = domain::ExchangeRunId{42},
        .commandSequence = domain::CommandSequence{sequence},
        .behavioralRulesVersion = 1,
        .configurationVersion = 7,
        .clientId = domain::ClientId{11},
        .instrumentId = domain::InstrumentId{100},
        .clientCommandId = domain::ClientCommandId{"new-1"},
        .side = domain::Side::BUY,
        .timeInForce = core::task::TimeInForce::GTC,
        .price = domain::Price{1234},
        .quantity = domain::Quantity{50},
    };
}

JournalCancelV1 readerCancel(const std::uint64_t sequence = 2) {
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

std::vector<std::byte> encodedHeader(const RunHeaderV1 &header) {
    std::vector<std::byte> bytes;
    EXPECT_EQ(encodeRunHeaderV1(header, bytes), RunHeaderCodecError::NONE);
    return bytes;
}

std::vector<std::byte> encodedNewOrder(const JournalNewOrderV1 &command) {
    std::vector<std::byte> bytes;
    EXPECT_EQ(encodeNewOrderV1(command, bytes), JournalCommandCodecError::NONE);
    return bytes;
}

std::vector<std::byte> encodedCancel(const JournalCancelV1 &command) {
    std::vector<std::byte> bytes;
    EXPECT_EQ(encodeCancelV1(command, bytes), JournalCommandCodecError::NONE);
    return bytes;
}

void append(std::vector<std::byte> &journal, const std::span<const std::byte> frame) {
    journal.insert(journal.end(), frame.begin(), frame.end());
}

void writeBytes(const std::filesystem::path &path, const std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open());
    stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(stream.good());
}

LoadedRunJournalV1 sentinelJournal() {
    return {
        .header =
            {
                .exchangeRunId = domain::ExchangeRunId{99},
                .behavioralRulesVersion = 1,
                .maxEventsPerCommand = 4'096,
                .maxRunCommands = 1,
                .maxRunJournalBytes = 100,
                .instruments = {{domain::InstrumentId{999}, 1}},
            },
        .commands = {},
        .validCommittedByteCount = 77,
    };
}

void updateChecksum(std::vector<std::byte> &frame) {
    detail::writeUint32LittleEndian(frame, detail::JOURNAL_V1_CHECKSUM_OFFSET, detail::journalV1Checksum(frame));
}

void expectCorruption(const RunJournalLoadResult &result, const RunJournalLoadError error = RunJournalLoadError::NONE) {
    EXPECT_EQ(result.outcome, RunJournalLoadOutcome::CORRUPTION);
    EXPECT_EQ(result.error, error);
    EXPECT_EQ(result.systemError, 0);
}

TEST(RunJournalReaderTest, LoadsHeaderOnlyJournal) {
    ReaderTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto header = readerHeader();
    const auto bytes = encodedHeader(header);
    writeBytes(directory.journalPath(), bytes);
    LoadedRunJournalV1 loaded;

    const auto result = loadRunJournalV1(directory.journalPath(), loaded);

    EXPECT_EQ(result.outcome, RunJournalLoadOutcome::VALID);
    EXPECT_EQ(loaded.header, header);
    EXPECT_TRUE(loaded.commands.empty());
    EXPECT_EQ(loaded.validCommittedByteCount, bytes.size());
}

TEST(RunJournalReaderTest, LoadsMixedCommandsInExactOrder) {
    ReaderTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto header = readerHeader();
    const auto first = readerNewOrder();
    const auto second = readerCancel();
    const auto third = readerNewOrder(3);
    auto bytes = encodedHeader(header);
    const auto firstBytes = encodedNewOrder(first);
    const auto secondBytes = encodedCancel(second);
    const auto thirdBytes = encodedNewOrder(third);
    append(bytes, firstBytes);
    append(bytes, secondBytes);
    append(bytes, thirdBytes);
    writeBytes(directory.journalPath(), bytes);
    LoadedRunJournalV1 loaded;

    const auto result = loadRunJournalV1(directory.journalPath(), loaded);

    EXPECT_EQ(result.outcome, RunJournalLoadOutcome::VALID);
    ASSERT_EQ(loaded.commands.size(), 3U);
    ASSERT_TRUE(std::holds_alternative<JournalNewOrderV1>(loaded.commands[0]));
    ASSERT_TRUE(std::holds_alternative<JournalCancelV1>(loaded.commands[1]));
    ASSERT_TRUE(std::holds_alternative<JournalNewOrderV1>(loaded.commands[2]));
    EXPECT_EQ(std::get<JournalNewOrderV1>(loaded.commands[0]), first);
    EXPECT_EQ(std::get<JournalCancelV1>(loaded.commands[1]), second);
    EXPECT_EQ(std::get<JournalNewOrderV1>(loaded.commands[2]), third);
    EXPECT_EQ(loaded.validCommittedByteCount, bytes.size());
}

TEST(RunJournalReaderTest, MissingAndIoFailureLeaveOutputUnchanged) {
    ReaderTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    auto missingOutput = sentinelJournal();
    const auto originalMissingOutput = missingOutput;

    const auto missing = loadRunJournalV1(directory.journalPath(), missingOutput);

    EXPECT_EQ(missing.outcome, RunJournalLoadOutcome::MISSING);
    EXPECT_EQ(missing.systemError, ENOENT);
    EXPECT_EQ(missingOutput, originalMissingOutput);

    auto ioOutput = sentinelJournal();
    const auto originalIoOutput = ioOutput;
    const auto io = loadRunJournalV1(directory.journalPath("."), ioOutput);
    EXPECT_EQ(io.outcome, RunJournalLoadOutcome::IO_FAILURE);
    EXPECT_EQ(io.systemError, EISDIR);
    EXPECT_EQ(ioOutput, originalIoOutput);
}

TEST(RunJournalReaderTest, RejectsTruncatedHeaderAndPreservesOutput) {
    ReaderTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto headerBytes = encodedHeader(readerHeader());
    auto output = sentinelJournal();
    const auto originalOutput = output;

    writeBytes(directory.journalPath("short-envelope.fxjr"),
               std::span<const std::byte>(headerBytes).first(detail::JOURNAL_V1_ENVELOPE_SIZE - 1));
    const auto shortEnvelope = loadRunJournalV1(directory.journalPath("short-envelope.fxjr"), output);
    expectCorruption(shortEnvelope);
    EXPECT_EQ(shortEnvelope.headerCodecError, RunHeaderCodecError::TRUNCATED_INPUT);
    EXPECT_EQ(output, originalOutput);

    writeBytes(directory.journalPath("short-payload.fxjr"),
               std::span<const std::byte>(headerBytes).first(headerBytes.size() - 1));
    const auto shortPayload = loadRunJournalV1(directory.journalPath("short-payload.fxjr"), output);
    expectCorruption(shortPayload);
    EXPECT_EQ(shortPayload.headerCodecError, RunHeaderCodecError::TRUNCATED_INPUT);
    EXPECT_EQ(output, originalOutput);

    auto oversizedHeader =
        std::vector<std::byte>(headerBytes.begin(), headerBytes.begin() + detail::JOURNAL_V1_ENVELOPE_SIZE);
    detail::writeUint32LittleEndian(oversizedHeader, detail::JOURNAL_V1_TOTAL_LENGTH_OFFSET, 65'537);
    writeBytes(directory.journalPath("oversized-header.fxjr"), oversizedHeader);
    const auto oversized = loadRunJournalV1(directory.journalPath("oversized-header.fxjr"), output);
    expectCorruption(oversized);
    EXPECT_EQ(oversized.headerCodecError, RunHeaderCodecError::FRAME_TOO_LARGE);
    EXPECT_EQ(output, originalOutput);
}

TEST(RunJournalReaderTest, RejectsEveryCrossRecordMismatch) {
    ReaderTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto headerBytes = encodedHeader(readerHeader());

    const auto assertMismatch = [&](const std::string &name, std::vector<std::byte> frame,
                                    const RunJournalLoadError expectedError) {
        auto journal = headerBytes;
        append(journal, frame);
        writeBytes(directory.journalPath(name), journal);
        auto output = sentinelJournal();
        const auto originalOutput = output;
        const auto result = loadRunJournalV1(directory.journalPath(name), output);
        expectCorruption(result, expectedError);
        EXPECT_EQ(output, originalOutput);
        return result;
    };

    auto wrongRun = readerNewOrder();
    wrongRun.exchangeRunId = domain::ExchangeRunId{43};
    assertMismatch("run.fxjr", encodedNewOrder(wrongRun), RunJournalLoadError::EXCHANGE_RUN_ID_MISMATCH);

    auto missingInstrument = readerNewOrder();
    missingInstrument.instrumentId = domain::InstrumentId{300};
    assertMismatch("instrument.fxjr", encodedNewOrder(missingInstrument),
                   RunJournalLoadError::INSTRUMENT_CONFIGURATION_MISMATCH);

    auto wrongConfiguration = readerNewOrder();
    wrongConfiguration.configurationVersion = 8;
    assertMismatch("configuration.fxjr", encodedNewOrder(wrongConfiguration),
                   RunJournalLoadError::INSTRUMENT_CONFIGURATION_MISMATCH);

    auto wrongRules = encodedNewOrder(readerNewOrder());
    detail::writeUint32LittleEndian(wrongRules, detail::JOURNAL_V1_PAYLOAD_OFFSET, 2);
    updateChecksum(wrongRules);
    const auto rulesResult = assertMismatch("rules.fxjr", std::move(wrongRules), RunJournalLoadError::NONE);
    EXPECT_EQ(rulesResult.commandCodecError, JournalCommandCodecError::UNSUPPORTED_BEHAVIORAL_RULES_VERSION);
}

TEST(RunJournalReaderTest, RejectsSequenceGapsDuplicatesAndDecreases) {
    ReaderTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto headerBytes = encodedHeader(readerHeader());

    const auto assertSequencesCorrupt = [&](const std::string &name, const std::vector<std::uint64_t> &sequences) {
        auto journal = headerBytes;
        for (const auto sequence : sequences) {
            const auto frame = encodedNewOrder(readerNewOrder(sequence));
            append(journal, frame);
        }
        writeBytes(directory.journalPath(name), journal);
        LoadedRunJournalV1 output;
        const auto result = loadRunJournalV1(directory.journalPath(name), output);
        expectCorruption(result, RunJournalLoadError::COMMAND_SEQUENCE_MISMATCH);
    };

    assertSequencesCorrupt("gap.fxjr", {1, 3});
    assertSequencesCorrupt("duplicate.fxjr", {1, 1});
    assertSequencesCorrupt("decrease.fxjr", {1, 2, 1});
}

TEST(RunJournalReaderTest, RejectsCommandAndByteCapacityViolations) {
    ReaderTestDirectory directory;
    ASSERT_TRUE(directory.valid());

    auto countJournal = encodedHeader(readerHeader(1));
    append(countJournal, encodedNewOrder(readerNewOrder()));
    append(countJournal, encodedCancel(readerCancel()));
    writeBytes(directory.journalPath("count.fxjr"), countJournal);
    LoadedRunJournalV1 output;
    const auto countResult = loadRunJournalV1(directory.journalPath("count.fxjr"), output);
    expectCorruption(countResult, RunJournalLoadError::COMMAND_COUNT_CAPACITY_EXCEEDED);

    auto byteHeader = readerHeader();
    const auto provisionalHeader = encodedHeader(byteHeader);
    const auto commandBytes = encodedNewOrder(readerNewOrder());
    byteHeader.maxRunJournalBytes = provisionalHeader.size() + commandBytes.size() - 1;
    auto byteJournal = encodedHeader(byteHeader);
    append(byteJournal, commandBytes);
    writeBytes(directory.journalPath("bytes.fxjr"), byteJournal);
    const auto byteResult = loadRunJournalV1(directory.journalPath("bytes.fxjr"), output);
    expectCorruption(byteResult, RunJournalLoadError::FILE_BYTE_CAPACITY_EXCEEDED);
}

TEST(RunJournalReaderTest, RejectsUnknownTypesInvalidFrameLengthsAndChecksumDamage) {
    ReaderTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto headerBytes = encodedHeader(readerHeader());

    auto unknown = encodedNewOrder(readerNewOrder());
    detail::writeUint16LittleEndian(unknown, detail::JOURNAL_V1_RECORD_TYPE_OFFSET, 9);
    updateChecksum(unknown);
    auto unknownJournal = headerBytes;
    append(unknownJournal, unknown);
    writeBytes(directory.journalPath("unknown.fxjr"), unknownJournal);
    LoadedRunJournalV1 output;
    const auto unknownResult = loadRunJournalV1(directory.journalPath("unknown.fxjr"), output);
    expectCorruption(unknownResult);
    EXPECT_EQ(unknownResult.commandCodecError, JournalCommandCodecError::UNSUPPORTED_RECORD_TYPE);

    auto oversizedEnvelope = encodedNewOrder(readerNewOrder());
    oversizedEnvelope.resize(detail::JOURNAL_V1_ENVELOPE_SIZE);
    detail::writeUint32LittleEndian(oversizedEnvelope, detail::JOURNAL_V1_TOTAL_LENGTH_OFFSET, 257);
    detail::writeUint32LittleEndian(oversizedEnvelope, detail::JOURNAL_V1_PAYLOAD_LENGTH_OFFSET,
                                    257 - detail::JOURNAL_V1_ENVELOPE_SIZE);
    auto oversizedJournal = headerBytes;
    append(oversizedJournal, oversizedEnvelope);
    writeBytes(directory.journalPath("oversized.fxjr"), oversizedJournal);
    const auto oversizedResult = loadRunJournalV1(directory.journalPath("oversized.fxjr"), output);
    expectCorruption(oversizedResult);
    EXPECT_EQ(oversizedResult.commandCodecError, JournalCommandCodecError::FRAME_TOO_LARGE);

    auto undersizedEnvelope = encodedNewOrder(readerNewOrder());
    undersizedEnvelope.resize(detail::JOURNAL_V1_ENVELOPE_SIZE);
    detail::writeUint32LittleEndian(undersizedEnvelope, detail::JOURNAL_V1_TOTAL_LENGTH_OFFSET,
                                    detail::JOURNAL_V1_ENVELOPE_SIZE + 1);
    detail::writeUint32LittleEndian(undersizedEnvelope, detail::JOURNAL_V1_PAYLOAD_LENGTH_OFFSET, 1);
    auto undersizedJournal = headerBytes;
    append(undersizedJournal, undersizedEnvelope);
    writeBytes(directory.journalPath("undersized.fxjr"), undersizedJournal);
    const auto undersizedResult = loadRunJournalV1(directory.journalPath("undersized.fxjr"), output);
    expectCorruption(undersizedResult);
    EXPECT_EQ(undersizedResult.commandCodecError, JournalCommandCodecError::INVALID_PAYLOAD_LENGTH);

    auto damaged = encodedNewOrder(readerNewOrder());
    damaged.back() ^= std::byte{0x01};
    auto damagedJournal = headerBytes;
    append(damagedJournal, damaged);
    writeBytes(directory.journalPath("checksum.fxjr"), damagedJournal);
    const auto damagedResult = loadRunJournalV1(directory.journalPath("checksum.fxjr"), output);
    expectCorruption(damagedResult);
    EXPECT_EQ(damagedResult.commandCodecError, JournalCommandCodecError::CHECKSUM_MISMATCH);
}

TEST(RunJournalReaderTest, ReportsShortEnvelopeAsIncompleteTailWithValidPrefix) {
    ReaderTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto header = readerHeader();
    auto journal = encodedHeader(header);
    const auto validCommand = encodedNewOrder(readerNewOrder());
    append(journal, validCommand);
    const std::uint64_t validBytes = journal.size();
    const auto tail = encodedCancel(readerCancel());
    constexpr std::size_t TAIL_LENGTH = detail::JOURNAL_V1_ENVELOPE_SIZE - 1;
    append(journal, std::span<const std::byte>(tail).first(TAIL_LENGTH));
    writeBytes(directory.journalPath(), journal);
    LoadedRunJournalV1 loaded;

    const auto result = loadRunJournalV1(directory.journalPath(), loaded);

    EXPECT_EQ(result.outcome, RunJournalLoadOutcome::INCOMPLETE_TAIL);
    EXPECT_EQ(result.tailOffset, validBytes);
    EXPECT_EQ(result.tailLength, TAIL_LENGTH);
    EXPECT_EQ(loaded.header, header);
    ASSERT_EQ(loaded.commands.size(), 1U);
    EXPECT_EQ(std::get<JournalNewOrderV1>(loaded.commands[0]), readerNewOrder());
    EXPECT_EQ(loaded.validCommittedByteCount, validBytes);
}

TEST(RunJournalReaderTest, ReportsValidEnvelopeExtendingPastEofAsIncompleteTail) {
    ReaderTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    auto journal = encodedHeader(readerHeader());
    const std::uint64_t tailOffset = journal.size();
    const auto command = encodedNewOrder(readerNewOrder());
    const std::size_t tailLength = command.size() - 1;
    append(journal, std::span<const std::byte>(command).first(tailLength));
    writeBytes(directory.journalPath(), journal);
    LoadedRunJournalV1 loaded;

    const auto result = loadRunJournalV1(directory.journalPath(), loaded);

    EXPECT_EQ(result.outcome, RunJournalLoadOutcome::INCOMPLETE_TAIL);
    EXPECT_EQ(result.tailOffset, tailOffset);
    EXPECT_EQ(result.tailLength, tailLength);
    EXPECT_TRUE(loaded.commands.empty());
    EXPECT_EQ(loaded.validCommittedByteCount, tailOffset);
}

TEST(RunJournalReaderTest, RejectsIncompleteTailWhenCommandCountIsAlreadyExhausted) {
    ReaderTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    auto journal = encodedHeader(readerHeader(1));
    append(journal, encodedNewOrder(readerNewOrder()));
    const auto nextCommand = encodedCancel(readerCancel());
    append(journal, std::span<const std::byte>(nextCommand).first(1));
    writeBytes(directory.journalPath(), journal);
    auto output = sentinelJournal();
    const auto originalOutput = output;

    const auto result = loadRunJournalV1(directory.journalPath(), output);

    expectCorruption(result, RunJournalLoadError::COMMAND_COUNT_CAPACITY_EXCEEDED);
    EXPECT_EQ(output, originalOutput);
}

TEST(RunJournalReaderTest, RejectsIncompleteTailWhoseDeclaredFrameExceedsByteCapacity) {
    ReaderTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto command = encodedNewOrder(readerNewOrder());
    auto header = readerHeader();
    const auto provisionalHeader = encodedHeader(header);
    header.maxRunJournalBytes = provisionalHeader.size() + command.size() - 1;
    auto journal = encodedHeader(header);
    append(journal, std::span<const std::byte>(command).first(detail::JOURNAL_V1_ENVELOPE_SIZE));
    writeBytes(directory.journalPath(), journal);
    auto output = sentinelJournal();
    const auto originalOutput = output;

    const auto result = loadRunJournalV1(directory.journalPath(), output);

    expectCorruption(result, RunJournalLoadError::FILE_BYTE_CAPACITY_EXCEEDED);
    EXPECT_EQ(output, originalOutput);
}

struct ReadScript final {
    int calls{0};
    bool interruptFirst{false};
    int failingCall{0};
    int readError{EIO};
    std::size_t maximumRead{std::numeric_limits<std::size_t>::max()};
};

ssize_t scriptedRead(void *context, const int descriptor, void *buffer, const std::size_t size) noexcept {
    auto &script = *static_cast<ReadScript *>(context);
    ++script.calls;
    if (script.interruptFirst && script.calls == 1) {
        errno = EINTR;
        return -1;
    }
    if (script.calls == script.failingCall) {
        errno = script.readError;
        return -1;
    }
    return ::read(descriptor, buffer, std::min(size, script.maximumRead));
}

TEST(RunJournalReaderTest, HandlesInterruptedAndShortReadsSequentially) {
    ReaderTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    auto journal = encodedHeader(readerHeader());
    append(journal, encodedNewOrder(readerNewOrder()));
    append(journal, encodedCancel(readerCancel()));
    writeBytes(directory.journalPath(), journal);
    ReadScript script;
    script.interruptFirst = true;
    script.maximumRead = 3;
    const detail::RunJournalReaderHooks hooks{&script, scriptedRead};
    LoadedRunJournalV1 loaded;

    const auto result = detail::loadRunJournalV1WithHooks(directory.journalPath(), hooks, loaded);

    EXPECT_EQ(result.outcome, RunJournalLoadOutcome::VALID);
    EXPECT_GT(script.calls, 3);
    EXPECT_EQ(loaded.commands.size(), 2U);
    EXPECT_EQ(loaded.validCommittedByteCount, journal.size());
}

TEST(RunJournalReaderTest, ReadErrorIsIoFailureAndPreservesOutput) {
    ReaderTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto bytes = encodedHeader(readerHeader());
    writeBytes(directory.journalPath(), bytes);
    ReadScript script;
    script.failingCall = 1;
    script.readError = EIO;
    const detail::RunJournalReaderHooks hooks{&script, scriptedRead};
    auto output = sentinelJournal();
    const auto originalOutput = output;

    const auto result = detail::loadRunJournalV1WithHooks(directory.journalPath(), hooks, output);

    EXPECT_EQ(result.outcome, RunJournalLoadOutcome::IO_FAILURE);
    EXPECT_EQ(result.systemError, EIO);
    EXPECT_EQ(output, originalOutput);
}

} // namespace
} // namespace exchange::storage
