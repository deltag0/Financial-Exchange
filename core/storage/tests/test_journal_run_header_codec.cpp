#include "journal_run_header_codec.hpp"

#include "crc32c.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace exchange::storage {
namespace {

constexpr std::size_t ENVELOPE_SIZE = 36;
constexpr std::size_t CHECKSUM_OFFSET = 32;
constexpr std::size_t PAYLOAD_OFFSET = 36;
constexpr std::size_t INSTRUMENT_COUNT_OFFSET = PAYLOAD_OFFSET + 24;
constexpr std::size_t GENERATOR_CONFIG_LENGTH_OFFSET = PAYLOAD_OFFSET + 28;
constexpr std::size_t FIRST_INSTRUMENT_OFFSET = PAYLOAD_OFFSET + 32;
constexpr std::size_t MAX_FRAME_SIZE = 65'536;

constexpr std::array<std::byte, 92> GOLDEN_RUN_HEADER{
    std::byte{0x46}, std::byte{0x58}, std::byte{0x4A}, std::byte{0x52}, std::byte{0x01}, std::byte{0x00},
    std::byte{0x01}, std::byte{0x00}, std::byte{0x5C}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05}, std::byte{0x04}, std::byte{0x03},
    std::byte{0x02}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x38}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x65}, std::byte{0x15}, std::byte{0x7B}, std::byte{0x21},
    std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x10},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x18}, std::byte{0x17}, std::byte{0x16}, std::byte{0x15},
    std::byte{0x14}, std::byte{0x13}, std::byte{0x12}, std::byte{0x11}, std::byte{0x28}, std::byte{0x27},
    std::byte{0x26}, std::byte{0x25}, std::byte{0x24}, std::byte{0x23}, std::byte{0x22}, std::byte{0x21},
    std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x38}, std::byte{0x37}, std::byte{0x36}, std::byte{0x35},
    std::byte{0x34}, std::byte{0x33}, std::byte{0x32}, std::byte{0x31}, std::byte{0x44}, std::byte{0x43},
    std::byte{0x42}, std::byte{0x41}, std::byte{0x58}, std::byte{0x57}, std::byte{0x56}, std::byte{0x55},
    std::byte{0x54}, std::byte{0x53}, std::byte{0x52}, std::byte{0x51}, std::byte{0x64}, std::byte{0x63},
    std::byte{0x62}, std::byte{0x61},
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

RunHeaderV1 validHeader() {
    return {
        .exchangeRunId = domain::ExchangeRunId{7},
        .behavioralRulesVersion = 1,
        .maxEventsPerCommand = 4'096,
        .maxRunCommands = 100,
        .maxRunJournalBytes = 100'000,
        .instruments = {{domain::InstrumentId{1}, 1}},
    };
}

RunHeaderV1 sentinelHeader() {
    RunHeaderV1 header = validHeader();
    header.exchangeRunId = domain::ExchangeRunId{999};
    header.instruments = {{domain::InstrumentId{90}, 91}, {domain::InstrumentId{92}, 93}};
    return header;
}

std::vector<std::byte> encodeValidHeader() {
    std::vector<std::byte> frame;
    EXPECT_EQ(encodeRunHeaderV1(validHeader(), frame), RunHeaderCodecError::NONE);
    return frame;
}

void writeUint16LittleEndian(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint16_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = std::byte{static_cast<std::uint8_t>(value >> (index * 8U))};
    }
}

void writeUint32LittleEndian(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = std::byte{static_cast<std::uint8_t>(value >> (index * 8U))};
    }
}

void writeUint64LittleEndian(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint64_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = std::byte{static_cast<std::uint8_t>(value >> (index * 8U))};
    }
}

void rewriteChecksum(std::vector<std::byte>& frame) {
    Crc32cAccumulator accumulator;
    const std::span<const std::byte> view(frame);
    accumulator.update(view.first(CHECKSUM_OFFSET));
    accumulator.update(view.subspan(PAYLOAD_OFFSET));
    writeUint32LittleEndian(frame, CHECKSUM_OFFSET, accumulator.checksum());
}

void expectDecodeErrorAndUnchanged(const std::span<const std::byte> frame, const RunHeaderCodecError expectedError) {
    auto output = sentinelHeader();
    const auto unchanged = output;
    EXPECT_EQ(decodeRunHeaderV1(frame, output), expectedError);
    EXPECT_EQ(output, unchanged);
}

static_assert(noexcept(encodeRunHeaderV1(std::declval<const RunHeaderV1&>(), std::declval<std::vector<std::byte>&>())));
static_assert(noexcept(decodeRunHeaderV1(std::declval<std::span<const std::byte>>(), std::declval<RunHeaderV1&>())));

TEST(RunHeaderCodecTest, EncodesIndependentGoldenBytesAndDecodesThem) {
    std::vector<std::byte> encoded;

    ASSERT_EQ(encodeRunHeaderV1(goldenHeader(), encoded), RunHeaderCodecError::NONE);
    EXPECT_EQ(encoded, std::vector<std::byte>(GOLDEN_RUN_HEADER.begin(), GOLDEN_RUN_HEADER.end()));

    RunHeaderV1 decoded{};
    ASSERT_EQ(decodeRunHeaderV1(GOLDEN_RUN_HEADER, decoded), RunHeaderCodecError::NONE);
    EXPECT_EQ(decoded, goldenHeader());
}

TEST(RunHeaderCodecTest, RoundTripsMultipleStrictlySortedInstruments) {
    auto expected = validHeader();
    expected.instruments = {
        {domain::InstrumentId{1}, 11},
        {domain::InstrumentId{27}, 12},
        {domain::InstrumentId{std::numeric_limits<std::uint64_t>::max()}, std::numeric_limits<std::uint32_t>::max()},
    };
    std::vector<std::byte> encoded;
    ASSERT_EQ(encodeRunHeaderV1(expected, encoded), RunHeaderCodecError::NONE);
    RunHeaderV1 decoded{};

    ASSERT_EQ(decodeRunHeaderV1(encoded, decoded), RunHeaderCodecError::NONE);
    EXPECT_EQ(decoded, expected);
}

TEST(RunHeaderCodecTest, RoundTripsUint64CapacityBoundaries) {
    auto expected = validHeader();
    expected.exchangeRunId = domain::ExchangeRunId{std::numeric_limits<std::uint64_t>::max()};
    expected.maxRunCommands = std::numeric_limits<std::uint64_t>::max();
    expected.maxRunJournalBytes = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::byte> encoded;
    ASSERT_EQ(encodeRunHeaderV1(expected, encoded), RunHeaderCodecError::NONE);
    RunHeaderV1 decoded{};

    ASSERT_EQ(decodeRunHeaderV1(encoded, decoded), RunHeaderCodecError::NONE);
    EXPECT_EQ(decoded, expected);
}

TEST(RunHeaderCodecTest, RejectsEveryInvalidEnvelopeFieldWithoutChangingOutput) {
    const auto valid = encodeValidHeader();

    auto magic = valid;
    magic[0] = std::byte{'?'};
    expectDecodeErrorAndUnchanged(magic, RunHeaderCodecError::INVALID_MAGIC);

    auto formatVersion = valid;
    writeUint16LittleEndian(formatVersion, 4, 2);
    expectDecodeErrorAndUnchanged(formatVersion, RunHeaderCodecError::UNSUPPORTED_FORMAT_VERSION);

    auto recordType = valid;
    writeUint16LittleEndian(recordType, 6, 2);
    expectDecodeErrorAndUnchanged(recordType, RunHeaderCodecError::UNSUPPORTED_RECORD_TYPE);

    auto totalLength = valid;
    writeUint32LittleEndian(totalLength, 8, ENVELOPE_SIZE - 1);
    expectDecodeErrorAndUnchanged(totalLength, RunHeaderCodecError::INVALID_TOTAL_LENGTH);

    auto oversizedTotalLength = valid;
    writeUint32LittleEndian(oversizedTotalLength, 8, MAX_FRAME_SIZE + 1);
    expectDecodeErrorAndUnchanged(oversizedTotalLength, RunHeaderCodecError::FRAME_TOO_LARGE);

    auto runId = valid;
    writeUint64LittleEndian(runId, 12, 0);
    rewriteChecksum(runId);
    expectDecodeErrorAndUnchanged(runId, RunHeaderCodecError::ZERO_EXCHANGE_RUN_ID);

    auto commandSequence = valid;
    writeUint64LittleEndian(commandSequence, 20, 1);
    rewriteChecksum(commandSequence);
    expectDecodeErrorAndUnchanged(commandSequence, RunHeaderCodecError::NONZERO_COMMAND_SEQUENCE);

    auto payloadLength = valid;
    writeUint32LittleEndian(payloadLength, 28, static_cast<std::uint32_t>(valid.size() - ENVELOPE_SIZE + 1));
    rewriteChecksum(payloadLength);
    expectDecodeErrorAndUnchanged(payloadLength, RunHeaderCodecError::INVALID_PAYLOAD_LENGTH);

    auto checksum = valid;
    checksum[CHECKSUM_OFFSET] ^= std::byte{0x01};
    expectDecodeErrorAndUnchanged(checksum, RunHeaderCodecError::CHECKSUM_MISMATCH);
}

TEST(RunHeaderCodecTest, RejectsTruncatedOversizedAndTrailingInputWithoutChangingOutput) {
    const auto valid = encodeValidHeader();

    expectDecodeErrorAndUnchanged(std::span<const std::byte>(valid).first(ENVELOPE_SIZE - 1),
                                  RunHeaderCodecError::TRUNCATED_INPUT);
    expectDecodeErrorAndUnchanged(std::span<const std::byte>(valid).first(valid.size() - 1),
                                  RunHeaderCodecError::TRUNCATED_INPUT);

    auto oversized = valid;
    oversized.resize(MAX_FRAME_SIZE + 1);
    expectDecodeErrorAndUnchanged(oversized, RunHeaderCodecError::FRAME_TOO_LARGE);

    auto trailing = valid;
    trailing.push_back(std::byte{0});
    expectDecodeErrorAndUnchanged(trailing, RunHeaderCodecError::TRAILING_BYTES);
}

TEST(RunHeaderCodecTest, RejectsInconsistentDeclaredLengthsWithoutChangingOutput) {
    const auto valid = encodeValidHeader();

    auto totalTooLarge = valid;
    writeUint32LittleEndian(totalTooLarge, 8, static_cast<std::uint32_t>(valid.size() + 1));
    expectDecodeErrorAndUnchanged(totalTooLarge, RunHeaderCodecError::TRUNCATED_INPUT);

    auto totalTooSmall = valid;
    writeUint32LittleEndian(totalTooSmall, 8, static_cast<std::uint32_t>(valid.size() - 1));
    expectDecodeErrorAndUnchanged(totalTooSmall, RunHeaderCodecError::TRAILING_BYTES);

    auto inconsistentInstrumentCount = valid;
    writeUint32LittleEndian(inconsistentInstrumentCount, INSTRUMENT_COUNT_OFFSET, 2);
    rewriteChecksum(inconsistentInstrumentCount);
    expectDecodeErrorAndUnchanged(inconsistentInstrumentCount, RunHeaderCodecError::INVALID_PAYLOAD_LENGTH);
}

TEST(RunHeaderCodecTest, RejectsZeroAndNonIncreasingInstrumentIdentityWithoutChangingOutput) {
    const auto valid = encodeValidHeader();

    auto invalidHeader = validHeader();
    std::vector<std::byte> unchanged{std::byte{0xA5}};
    const auto original = unchanged;

    invalidHeader.instruments = {{domain::InstrumentId{0}, 1}};
    EXPECT_EQ(encodeRunHeaderV1(invalidHeader, unchanged), RunHeaderCodecError::ZERO_INSTRUMENT_ID);
    EXPECT_EQ(unchanged, original);

    invalidHeader.instruments = {{domain::InstrumentId{1}, 0}};
    EXPECT_EQ(encodeRunHeaderV1(invalidHeader, unchanged), RunHeaderCodecError::ZERO_CONFIGURATION_VERSION);
    EXPECT_EQ(unchanged, original);

    auto zeroInstrument = valid;
    writeUint64LittleEndian(zeroInstrument, FIRST_INSTRUMENT_OFFSET, 0);
    rewriteChecksum(zeroInstrument);
    expectDecodeErrorAndUnchanged(zeroInstrument, RunHeaderCodecError::ZERO_INSTRUMENT_ID);

    auto zeroConfiguration = valid;
    writeUint32LittleEndian(zeroConfiguration, FIRST_INSTRUMENT_OFFSET + 8, 0);
    rewriteChecksum(zeroConfiguration);
    expectDecodeErrorAndUnchanged(zeroConfiguration, RunHeaderCodecError::ZERO_CONFIGURATION_VERSION);

    auto duplicateHeader = validHeader();
    duplicateHeader.instruments = {{domain::InstrumentId{2}, 1}, {domain::InstrumentId{2}, 2}};
    EXPECT_EQ(encodeRunHeaderV1(duplicateHeader, unchanged), RunHeaderCodecError::INSTRUMENTS_NOT_STRICTLY_INCREASING);
    EXPECT_EQ(unchanged, original);

    auto unsortedHeader = validHeader();
    unsortedHeader.instruments = {{domain::InstrumentId{3}, 1}, {domain::InstrumentId{2}, 2}};
    EXPECT_EQ(encodeRunHeaderV1(unsortedHeader, unchanged), RunHeaderCodecError::INSTRUMENTS_NOT_STRICTLY_INCREASING);
    EXPECT_EQ(unchanged, original);

    auto sortedFrameHeader = validHeader();
    sortedFrameHeader.instruments = {{domain::InstrumentId{2}, 1}, {domain::InstrumentId{3}, 2}};
    std::vector<std::byte> sortedFrame;
    ASSERT_EQ(encodeRunHeaderV1(sortedFrameHeader, sortedFrame), RunHeaderCodecError::NONE);

    auto duplicateFrame = sortedFrame;
    writeUint64LittleEndian(duplicateFrame, FIRST_INSTRUMENT_OFFSET + 12, 2);
    rewriteChecksum(duplicateFrame);
    expectDecodeErrorAndUnchanged(duplicateFrame, RunHeaderCodecError::INSTRUMENTS_NOT_STRICTLY_INCREASING);

    auto unsortedFrame = sortedFrame;
    writeUint64LittleEndian(unsortedFrame, FIRST_INSTRUMENT_OFFSET + 12, 1);
    rewriteChecksum(unsortedFrame);
    expectDecodeErrorAndUnchanged(unsortedFrame, RunHeaderCodecError::INSTRUMENTS_NOT_STRICTLY_INCREASING);
}

TEST(RunHeaderCodecTest, RejectsUnsupportedRulesEventLimitAndGeneratorConfiguration) {
    auto header = validHeader();
    std::vector<std::byte> output{std::byte{0xA5}};
    const auto unchanged = output;

    header.behavioralRulesVersion = 2;
    EXPECT_EQ(encodeRunHeaderV1(header, output), RunHeaderCodecError::UNSUPPORTED_BEHAVIORAL_RULES_VERSION);
    EXPECT_EQ(output, unchanged);

    header = validHeader();
    header.maxEventsPerCommand = 4'095;
    EXPECT_EQ(encodeRunHeaderV1(header, output), RunHeaderCodecError::UNSUPPORTED_MAX_EVENTS_PER_COMMAND);
    EXPECT_EQ(output, unchanged);

    auto rulesFrame = encodeValidHeader();
    writeUint32LittleEndian(rulesFrame, PAYLOAD_OFFSET, 2);
    rewriteChecksum(rulesFrame);
    expectDecodeErrorAndUnchanged(rulesFrame, RunHeaderCodecError::UNSUPPORTED_BEHAVIORAL_RULES_VERSION);

    auto eventsFrame = encodeValidHeader();
    writeUint32LittleEndian(eventsFrame, PAYLOAD_OFFSET + 4, 4'095);
    rewriteChecksum(eventsFrame);
    expectDecodeErrorAndUnchanged(eventsFrame, RunHeaderCodecError::UNSUPPORTED_MAX_EVENTS_PER_COMMAND);

    auto generatorFrame = encodeValidHeader();
    generatorFrame.push_back(std::byte{0xA5});
    writeUint32LittleEndian(generatorFrame, 8, static_cast<std::uint32_t>(generatorFrame.size()));
    writeUint32LittleEndian(generatorFrame, 28, static_cast<std::uint32_t>(generatorFrame.size() - ENVELOPE_SIZE));
    writeUint32LittleEndian(generatorFrame, GENERATOR_CONFIG_LENGTH_OFFSET, 1);
    rewriteChecksum(generatorFrame);
    expectDecodeErrorAndUnchanged(generatorFrame, RunHeaderCodecError::UNSUPPORTED_GENERATOR_CONFIGURATION);
}

TEST(RunHeaderCodecTest, RejectsZeroRunAndUnusableCapacities) {
    auto header = validHeader();
    std::vector<std::byte> output;

    header.exchangeRunId = domain::ExchangeRunId{0};
    EXPECT_EQ(encodeRunHeaderV1(header, output), RunHeaderCodecError::ZERO_EXCHANGE_RUN_ID);

    header = validHeader();
    header.maxRunCommands = 0;
    EXPECT_EQ(encodeRunHeaderV1(header, output), RunHeaderCodecError::ZERO_MAX_RUN_COMMANDS);

    header = validHeader();
    header.maxRunJournalBytes = 79;
    EXPECT_EQ(encodeRunHeaderV1(header, output), RunHeaderCodecError::MAX_RUN_JOURNAL_BYTES_TOO_SMALL);

    auto zeroCommands = encodeValidHeader();
    writeUint64LittleEndian(zeroCommands, PAYLOAD_OFFSET + 8, 0);
    rewriteChecksum(zeroCommands);
    expectDecodeErrorAndUnchanged(zeroCommands, RunHeaderCodecError::ZERO_MAX_RUN_COMMANDS);

    auto insufficientBytes = encodeValidHeader();
    writeUint64LittleEndian(insufficientBytes, PAYLOAD_OFFSET + 16, insufficientBytes.size() - 1);
    rewriteChecksum(insufficientBytes);
    expectDecodeErrorAndUnchanged(insufficientBytes, RunHeaderCodecError::MAX_RUN_JOURNAL_BYTES_TOO_SMALL);
}

TEST(RunHeaderCodecTest, EnforcesFrameAndJournalByteCapacityBoundaries) {
    auto exactCapacity = validHeader();
    exactCapacity.maxRunCommands = 1;
    exactCapacity.maxRunJournalBytes = 80;
    std::vector<std::byte> encoded;
    ASSERT_EQ(encodeRunHeaderV1(exactCapacity, encoded), RunHeaderCodecError::NONE);
    ASSERT_EQ(encoded.size(), 80U);
    RunHeaderV1 decoded{};
    ASSERT_EQ(decodeRunHeaderV1(encoded, decoded), RunHeaderCodecError::NONE);
    EXPECT_EQ(decoded, exactCapacity);

    constexpr std::size_t MAX_GENERATOR_FREE_INSTRUMENTS = (MAX_FRAME_SIZE - 68) / 12;
    auto largest = validHeader();
    largest.maxRunJournalBytes = std::numeric_limits<std::uint64_t>::max();
    largest.instruments.clear();
    largest.instruments.reserve(MAX_GENERATOR_FREE_INSTRUMENTS);
    for (std::size_t index = 0; index < MAX_GENERATOR_FREE_INSTRUMENTS; ++index) {
        largest.instruments.push_back({domain::InstrumentId{index + 1}, 1});
    }
    ASSERT_EQ(encodeRunHeaderV1(largest, encoded), RunHeaderCodecError::NONE);
    EXPECT_EQ(encoded.size(), 65'528U);
    ASSERT_EQ(decodeRunHeaderV1(encoded, decoded), RunHeaderCodecError::NONE);
    EXPECT_EQ(decoded, largest);

    largest.instruments.push_back({domain::InstrumentId{MAX_GENERATOR_FREE_INSTRUMENTS + 1}, 1});
    const auto unchanged = encoded;
    EXPECT_EQ(encodeRunHeaderV1(largest, encoded), RunHeaderCodecError::FRAME_TOO_LARGE);
    EXPECT_EQ(encoded, unchanged);
}

} // namespace
} // namespace exchange::storage
