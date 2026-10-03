#include "crc32c.hpp"
#include "run_catalog_codec.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

namespace exchange::storage {
namespace {

constexpr RunCatalogV1Bytes GOLDEN_CATALOG{
    std::byte{0x46}, std::byte{0x58}, std::byte{0x52}, std::byte{0x43}, std::byte{0x01}, std::byte{0x00},
    std::byte{0x38}, std::byte{0x00}, std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05},
    std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01}, std::byte{0x18}, std::byte{0x17},
    std::byte{0x16}, std::byte{0x15}, std::byte{0x14}, std::byte{0x13}, std::byte{0x12}, std::byte{0x11},
    std::byte{0x01}, std::byte{0x01}, std::byte{0x01}, std::byte{0x01}, std::byte{0x01}, std::byte{0x01},
    std::byte{0x01}, std::byte{0x01}, std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x11}, std::byte{0x10},
    std::byte{0x0F}, std::byte{0x0E}, std::byte{0x0D}, std::byte{0x0C}, std::byte{0x0B}, std::byte{0x0A},
    std::byte{0xE9}, std::byte{0x95}, std::byte{0xF2}, std::byte{0xCF}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00},
};

constexpr RunCatalogSnapshotV1 GOLDEN_SNAPSHOT{
    .generation = 0x0102030405060708ULL,
    .lastReservedRunId = domain::ExchangeRunId{0x1112131415161718ULL},
    .activeRunId = domain::ExchangeRunId{0x0101010101010101ULL},
    .activeDisposition = RunCatalogDisposition::CAPACITY_REACHED,
    .retainedStoppedRunId = domain::ExchangeRunId{0x0A0B0C0D0E0F1011ULL},
};

RunCatalogSnapshotV1 validSnapshot() {
    return RunCatalogSnapshotV1{
        .generation = 7,
        .lastReservedRunId = domain::ExchangeRunId{3},
        .activeRunId = domain::ExchangeRunId{3},
        .activeDisposition = RunCatalogDisposition::OPEN,
        .retainedStoppedRunId = domain::ExchangeRunId{2},
    };
}

RunCatalogV1Bytes encodeValidSnapshot() {
    RunCatalogV1Bytes bytes{};
    EXPECT_EQ(encodeRunCatalogV1(validSnapshot(), bytes), RunCatalogCodecError::NONE);
    return bytes;
}

void writeUint32LittleEndian(RunCatalogV1Bytes& bytes, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = std::byte{static_cast<std::uint8_t>(value >> (index * 8U))};
    }
}

void writeUint64LittleEndian(RunCatalogV1Bytes& bytes, const std::size_t offset, const std::uint64_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = std::byte{static_cast<std::uint8_t>(value >> (index * 8U))};
    }
}

void rewriteChecksum(RunCatalogV1Bytes& bytes) {
    Crc32cAccumulator accumulator;
    const std::span<const std::byte> view(bytes);
    accumulator.update(view.first(48));
    accumulator.update(view.subspan(52, 4));
    writeUint32LittleEndian(bytes, 48, accumulator.checksum());
}

static_assert(std::is_same_v<domain::ExchangeRunId::Underlying, std::uint64_t>);
static_assert(!std::is_same_v<domain::ExchangeRunId, domain::CommandSequence>);
static_assert(!std::is_convertible_v<domain::ExchangeRunId, domain::CommandSequence>);
static_assert(noexcept(encodeRunCatalogV1(std::declval<const RunCatalogSnapshotV1&>(),
                                          std::declval<RunCatalogV1Bytes&>())));
static_assert(noexcept(decodeRunCatalogV1(std::declval<std::span<const std::byte>>(),
                                          std::declval<RunCatalogSnapshotV1&>())));

TEST(Crc32cTest, MatchesCastagnoliReferenceVectorAndChunkedUpdates) {
    constexpr std::array<std::byte, 9> input{
        std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}, std::byte{'5'},
        std::byte{'6'}, std::byte{'7'}, std::byte{'8'}, std::byte{'9'},
    };

    Crc32cAccumulator contiguous;
    contiguous.update(input);
    EXPECT_EQ(contiguous.checksum(), 0xE3069283U);

    Crc32cAccumulator chunked;
    chunked.update(std::span<const std::byte>(input).first(4));
    chunked.update(std::span<const std::byte>(input).subspan(4));
    EXPECT_EQ(chunked.checksum(), 0xE3069283U);
}

TEST(RunCatalogCodecTest, EncodesExactGoldenBytesAndDecodesThem) {
    RunCatalogV1Bytes encoded{};
    ASSERT_EQ(encodeRunCatalogV1(GOLDEN_SNAPSHOT, encoded), RunCatalogCodecError::NONE);
    EXPECT_EQ(encoded, GOLDEN_CATALOG);

    RunCatalogSnapshotV1 decoded{};
    ASSERT_EQ(decodeRunCatalogV1(GOLDEN_CATALOG, decoded), RunCatalogCodecError::NONE);
    EXPECT_EQ(decoded, GOLDEN_SNAPSHOT);
}

TEST(RunCatalogCodecTest, RoundTripsEveryDisposition) {
    constexpr std::array dispositions{
        RunCatalogDisposition::NONE,         RunCatalogDisposition::OPEN,
        RunCatalogDisposition::PAUSED,       RunCatalogDisposition::CAPACITY_REACHED,
        RunCatalogDisposition::FAIL_STOPPED, RunCatalogDisposition::RECOVERY_FAILED,
    };

    for (const RunCatalogDisposition disposition : dispositions) {
        SCOPED_TRACE(static_cast<std::uint8_t>(disposition));
        RunCatalogSnapshotV1 snapshot{
            .generation = 1,
            .lastReservedRunId = domain::ExchangeRunId{2},
            .activeRunId = disposition == RunCatalogDisposition::NONE
                               ? std::nullopt
                               : std::optional<domain::ExchangeRunId>{domain::ExchangeRunId{2}},
            .activeDisposition = disposition,
            .retainedStoppedRunId = disposition == RunCatalogDisposition::NONE
                                        ? std::optional<domain::ExchangeRunId>{domain::ExchangeRunId{1}}
                                        : std::nullopt,
        };
        RunCatalogV1Bytes encoded{};
        ASSERT_EQ(encodeRunCatalogV1(snapshot, encoded), RunCatalogCodecError::NONE);
        EXPECT_EQ(encoded[32], std::byte{static_cast<std::uint8_t>(disposition)});
        RunCatalogSnapshotV1 decoded{};
        ASSERT_EQ(decodeRunCatalogV1(encoded, decoded), RunCatalogCodecError::NONE);
        EXPECT_EQ(decoded, snapshot);
    }
}

TEST(RunCatalogCodecTest, AcceptsFreshCatalogAndMaximumUint64Boundaries) {
    const RunCatalogSnapshotV1 fresh{
        .generation = 1,
        .lastReservedRunId = std::nullopt,
        .activeRunId = std::nullopt,
        .activeDisposition = RunCatalogDisposition::NONE,
        .retainedStoppedRunId = std::nullopt,
    };
    RunCatalogV1Bytes encoded{};
    ASSERT_EQ(encodeRunCatalogV1(fresh, encoded), RunCatalogCodecError::NONE);
    RunCatalogSnapshotV1 decoded{};
    ASSERT_EQ(decodeRunCatalogV1(encoded, decoded), RunCatalogCodecError::NONE);
    EXPECT_EQ(decoded, fresh);

    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    const RunCatalogSnapshotV1 boundary{
        .generation = maximum,
        .lastReservedRunId = domain::ExchangeRunId{maximum},
        .activeRunId = domain::ExchangeRunId{maximum - 1},
        .activeDisposition = RunCatalogDisposition::RECOVERY_FAILED,
        .retainedStoppedRunId = domain::ExchangeRunId{maximum},
    };
    ASSERT_EQ(encodeRunCatalogV1(boundary, encoded), RunCatalogCodecError::NONE);
    ASSERT_EQ(decodeRunCatalogV1(encoded, decoded), RunCatalogCodecError::NONE);
    EXPECT_EQ(decoded, boundary);
}

TEST(RunCatalogCodecTest, RejectsInvalidSizesWithoutChangingOutput) {
    const RunCatalogV1Bytes encoded = encodeValidSnapshot();
    RunCatalogSnapshotV1 output = GOLDEN_SNAPSHOT;
    const RunCatalogSnapshotV1 unchanged = output;

    EXPECT_EQ(decodeRunCatalogV1(std::span<const std::byte>(encoded).first(55), output),
              RunCatalogCodecError::INVALID_SIZE);
    EXPECT_EQ(output, unchanged);

    std::array<std::byte, 57> oversized{};
    EXPECT_EQ(decodeRunCatalogV1(oversized, output), RunCatalogCodecError::INVALID_SIZE);
    EXPECT_EQ(output, unchanged);
}

TEST(RunCatalogCodecTest, RejectsInvalidMagicVersionLengthChecksumAndReservedBytes) {
    RunCatalogSnapshotV1 output{};

    RunCatalogV1Bytes invalidMagic = encodeValidSnapshot();
    invalidMagic[0] = std::byte{'B'};
    EXPECT_EQ(decodeRunCatalogV1(invalidMagic, output), RunCatalogCodecError::INVALID_MAGIC);

    RunCatalogV1Bytes invalidVersion = encodeValidSnapshot();
    invalidVersion[4] = std::byte{2};
    EXPECT_EQ(decodeRunCatalogV1(invalidVersion, output), RunCatalogCodecError::UNSUPPORTED_VERSION);

    RunCatalogV1Bytes invalidLength = encodeValidSnapshot();
    invalidLength[6] = std::byte{55};
    EXPECT_EQ(decodeRunCatalogV1(invalidLength, output), RunCatalogCodecError::INVALID_TOTAL_LENGTH);

    RunCatalogV1Bytes invalidFirstReserved = encodeValidSnapshot();
    invalidFirstReserved[33] = std::byte{1};
    EXPECT_EQ(decodeRunCatalogV1(invalidFirstReserved, output), RunCatalogCodecError::NONZERO_RESERVED_BYTES);

    RunCatalogV1Bytes invalidFinalReserved = encodeValidSnapshot();
    invalidFinalReserved[55] = std::byte{1};
    EXPECT_EQ(decodeRunCatalogV1(invalidFinalReserved, output), RunCatalogCodecError::NONZERO_RESERVED_BYTES);

    RunCatalogV1Bytes invalidChecksum = encodeValidSnapshot();
    invalidChecksum[48] ^= std::byte{1};
    EXPECT_EQ(decodeRunCatalogV1(invalidChecksum, output), RunCatalogCodecError::CHECKSUM_MISMATCH);

    RunCatalogV1Bytes corruptedCoveredByte = encodeValidSnapshot();
    corruptedCoveredByte[8] ^= std::byte{1};
    EXPECT_EQ(decodeRunCatalogV1(corruptedCoveredByte, output), RunCatalogCodecError::CHECKSUM_MISMATCH);
}

TEST(RunCatalogCodecTest, RejectsUnknownDispositionAfterChecksumValidation) {
    RunCatalogV1Bytes encoded = encodeValidSnapshot();
    encoded[32] = std::byte{6};
    rewriteChecksum(encoded);

    RunCatalogSnapshotV1 output{};
    EXPECT_EQ(decodeRunCatalogV1(encoded, output), RunCatalogCodecError::UNKNOWN_DISPOSITION);
}

TEST(RunCatalogCodecTest, RejectsInvalidSnapshotRelationshipsWithoutChangingEncodedOutput) {
    RunCatalogV1Bytes output{};
    output.fill(std::byte{0xA5});
    const RunCatalogV1Bytes unchanged = output;

    RunCatalogSnapshotV1 snapshot = validSnapshot();
    snapshot.generation = 0;
    EXPECT_EQ(encodeRunCatalogV1(snapshot, output), RunCatalogCodecError::ZERO_GENERATION);
    EXPECT_EQ(output, unchanged);

    snapshot = validSnapshot();
    snapshot.activeDisposition = static_cast<RunCatalogDisposition>(6);
    EXPECT_EQ(encodeRunCatalogV1(snapshot, output), RunCatalogCodecError::UNKNOWN_DISPOSITION);

    snapshot = validSnapshot();
    snapshot.activeRunId = domain::ExchangeRunId{0};
    EXPECT_EQ(encodeRunCatalogV1(snapshot, output), RunCatalogCodecError::ZERO_PRESENT_RUN_ID);

    snapshot = validSnapshot();
    snapshot.lastReservedRunId = domain::ExchangeRunId{0};
    EXPECT_EQ(encodeRunCatalogV1(snapshot, output), RunCatalogCodecError::ZERO_PRESENT_RUN_ID);

    snapshot = validSnapshot();
    snapshot.retainedStoppedRunId = domain::ExchangeRunId{0};
    EXPECT_EQ(encodeRunCatalogV1(snapshot, output), RunCatalogCodecError::ZERO_PRESENT_RUN_ID);

    snapshot = validSnapshot();
    snapshot.activeRunId.reset();
    EXPECT_EQ(encodeRunCatalogV1(snapshot, output), RunCatalogCodecError::ACTIVE_DISPOSITION_MISMATCH);

    snapshot = validSnapshot();
    snapshot.activeDisposition = RunCatalogDisposition::NONE;
    EXPECT_EQ(encodeRunCatalogV1(snapshot, output), RunCatalogCodecError::ACTIVE_DISPOSITION_MISMATCH);

    snapshot = validSnapshot();
    snapshot.retainedStoppedRunId = snapshot.activeRunId;
    EXPECT_EQ(encodeRunCatalogV1(snapshot, output), RunCatalogCodecError::DUPLICATE_ACTIVE_AND_RETAINED_RUN_ID);

    snapshot = validSnapshot();
    snapshot.lastReservedRunId = domain::ExchangeRunId{2};
    EXPECT_EQ(encodeRunCatalogV1(snapshot, output), RunCatalogCodecError::RUN_ID_EXCEEDS_LAST_RESERVED);

    snapshot = validSnapshot();
    snapshot.lastReservedRunId.reset();
    EXPECT_EQ(encodeRunCatalogV1(snapshot, output), RunCatalogCodecError::RUN_ID_EXCEEDS_LAST_RESERVED);

    EXPECT_EQ(output, unchanged);
}

TEST(RunCatalogCodecTest, DecodeRejectsInvalidIdRelationshipsWithValidChecksums) {
    RunCatalogSnapshotV1 output{};

    RunCatalogV1Bytes zeroGeneration = encodeValidSnapshot();
    writeUint64LittleEndian(zeroGeneration, 8, 0);
    rewriteChecksum(zeroGeneration);
    EXPECT_EQ(decodeRunCatalogV1(zeroGeneration, output), RunCatalogCodecError::ZERO_GENERATION);

    RunCatalogV1Bytes duplicateIds = encodeValidSnapshot();
    writeUint64LittleEndian(duplicateIds, 40, 3);
    rewriteChecksum(duplicateIds);
    EXPECT_EQ(decodeRunCatalogV1(duplicateIds, output), RunCatalogCodecError::DUPLICATE_ACTIVE_AND_RETAINED_RUN_ID);

    RunCatalogV1Bytes activeAboveLastReserved = encodeValidSnapshot();
    writeUint64LittleEndian(activeAboveLastReserved, 16, 2);
    rewriteChecksum(activeAboveLastReserved);
    EXPECT_EQ(decodeRunCatalogV1(activeAboveLastReserved, output), RunCatalogCodecError::RUN_ID_EXCEEDS_LAST_RESERVED);

    RunCatalogV1Bytes retainedAboveLastReserved = encodeValidSnapshot();
    writeUint64LittleEndian(retainedAboveLastReserved, 40, 4);
    rewriteChecksum(retainedAboveLastReserved);
    EXPECT_EQ(decodeRunCatalogV1(retainedAboveLastReserved, output),
              RunCatalogCodecError::RUN_ID_EXCEEDS_LAST_RESERVED);

    RunCatalogV1Bytes dispositionWithoutActive = encodeValidSnapshot();
    writeUint64LittleEndian(dispositionWithoutActive, 24, 0);
    rewriteChecksum(dispositionWithoutActive);
    EXPECT_EQ(decodeRunCatalogV1(dispositionWithoutActive, output), RunCatalogCodecError::ACTIVE_DISPOSITION_MISMATCH);

    RunCatalogV1Bytes activeWithoutDisposition = encodeValidSnapshot();
    activeWithoutDisposition[32] = std::byte{0};
    rewriteChecksum(activeWithoutDisposition);
    EXPECT_EQ(decodeRunCatalogV1(activeWithoutDisposition, output), RunCatalogCodecError::ACTIVE_DISPOSITION_MISMATCH);
}

} // namespace
} // namespace exchange::storage
