#include "run_catalog_codec.hpp"

#include "crc32c.hpp"
#include "little_endian.hpp"

#include <algorithm>

namespace exchange::storage {

namespace {

constexpr std::array<std::byte, 4> CATALOG_MAGIC{
    std::byte{'F'},
    std::byte{'X'},
    std::byte{'R'},
    std::byte{'C'},
};
constexpr std::uint16_t CATALOG_VERSION = 1;
constexpr std::size_t GENERATION_OFFSET = 8;
constexpr std::size_t LAST_RESERVED_RUN_ID_OFFSET = 16;
constexpr std::size_t ACTIVE_RUN_ID_OFFSET = 24;
constexpr std::size_t ACTIVE_DISPOSITION_OFFSET = 32;
constexpr std::size_t FIRST_RESERVED_OFFSET = 33;
constexpr std::size_t FIRST_RESERVED_LENGTH = 7;
constexpr std::size_t RETAINED_STOPPED_RUN_ID_OFFSET = 40;
constexpr std::size_t CHECKSUM_OFFSET = 48;
constexpr std::size_t FINAL_RESERVED_OFFSET = 52;
constexpr std::size_t FINAL_RESERVED_LENGTH = 4;

std::uint64_t encodedRunId(const std::optional<domain::ExchangeRunId> runId) noexcept {
    return runId.has_value() ? runId->value() : 0;
}

std::optional<domain::ExchangeRunId> decodedRunId(const std::uint64_t value) noexcept {
    if (value == 0) {
        return std::nullopt;
    }
    return domain::ExchangeRunId{value};
}

bool containsNonzeroByte(const std::span<const std::byte> bytes) noexcept {
    return std::any_of(bytes.begin(), bytes.end(), [](const std::byte byte) { return byte != std::byte{0}; });
}

std::uint32_t catalogChecksum(const std::span<const std::byte> bytes) noexcept {
    Crc32cAccumulator accumulator;
    accumulator.update(bytes.first(CHECKSUM_OFFSET));
    accumulator.update(bytes.subspan(FINAL_RESERVED_OFFSET, FINAL_RESERVED_LENGTH));
    return accumulator.checksum();
}

constexpr bool isKnownDispositionValue(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(RunCatalogDisposition::RECOVERY_FAILED);
}

bool isPresentZero(const std::optional<domain::ExchangeRunId> runId) noexcept {
    return runId.has_value() && runId->value() == 0;
}

RunCatalogCodecError validateSnapshot(const RunCatalogSnapshotV1& snapshot) noexcept {
    if (snapshot.generation == 0) {
        return RunCatalogCodecError::ZERO_GENERATION;
    }
    if (!isKnownDispositionValue(static_cast<std::uint8_t>(snapshot.activeDisposition))) {
        return RunCatalogCodecError::UNKNOWN_DISPOSITION;
    }
    if (isPresentZero(snapshot.lastReservedRunId) || isPresentZero(snapshot.activeRunId) ||
        isPresentZero(snapshot.retainedStoppedRunId)) {
        return RunCatalogCodecError::ZERO_PRESENT_RUN_ID;
    }

    const bool hasActiveRun = snapshot.activeRunId.has_value();
    const bool hasActiveDisposition = snapshot.activeDisposition != RunCatalogDisposition::NONE;
    if (hasActiveRun != hasActiveDisposition) {
        return RunCatalogCodecError::ACTIVE_DISPOSITION_MISMATCH;
    }

    if (snapshot.activeRunId.has_value() && snapshot.retainedStoppedRunId.has_value() &&
        snapshot.activeRunId == snapshot.retainedStoppedRunId) {
        return RunCatalogCodecError::DUPLICATE_ACTIVE_AND_RETAINED_RUN_ID;
    }

    if (snapshot.activeRunId.has_value() &&
        (!snapshot.lastReservedRunId.has_value() || *snapshot.activeRunId > *snapshot.lastReservedRunId)) {
        return RunCatalogCodecError::RUN_ID_EXCEEDS_LAST_RESERVED;
    }
    if (snapshot.retainedStoppedRunId.has_value() &&
        (!snapshot.lastReservedRunId.has_value() || *snapshot.retainedStoppedRunId > *snapshot.lastReservedRunId)) {
        return RunCatalogCodecError::RUN_ID_EXCEEDS_LAST_RESERVED;
    }

    return RunCatalogCodecError::NONE;
}

} // namespace

RunCatalogCodecError encodeRunCatalogV1(const RunCatalogSnapshotV1& snapshot, RunCatalogV1Bytes& output) noexcept {
    const RunCatalogCodecError validationError = validateSnapshot(snapshot);
    if (validationError != RunCatalogCodecError::NONE) {
        return validationError;
    }

    RunCatalogV1Bytes encoded{};
    std::copy(CATALOG_MAGIC.begin(), CATALOG_MAGIC.end(), encoded.begin());
    detail::writeUint16LittleEndian(encoded, 4, CATALOG_VERSION);
    detail::writeUint16LittleEndian(encoded, 6, RUN_CATALOG_V1_SIZE);
    detail::writeUint64LittleEndian(encoded, GENERATION_OFFSET, snapshot.generation);
    detail::writeUint64LittleEndian(encoded, LAST_RESERVED_RUN_ID_OFFSET, encodedRunId(snapshot.lastReservedRunId));
    detail::writeUint64LittleEndian(encoded, ACTIVE_RUN_ID_OFFSET, encodedRunId(snapshot.activeRunId));
    encoded[ACTIVE_DISPOSITION_OFFSET] = std::byte{static_cast<std::uint8_t>(snapshot.activeDisposition)};
    detail::writeUint64LittleEndian(encoded, RETAINED_STOPPED_RUN_ID_OFFSET,
                                    encodedRunId(snapshot.retainedStoppedRunId));
    detail::writeUint32LittleEndian(encoded, CHECKSUM_OFFSET, catalogChecksum(encoded));
    output = encoded;
    return RunCatalogCodecError::NONE;
}

RunCatalogCodecError decodeRunCatalogV1(const std::span<const std::byte> input, RunCatalogSnapshotV1& output) noexcept {
    if (input.size() != RUN_CATALOG_V1_SIZE) {
        return RunCatalogCodecError::INVALID_SIZE;
    }
    if (!std::equal(CATALOG_MAGIC.begin(), CATALOG_MAGIC.end(), input.begin())) {
        return RunCatalogCodecError::INVALID_MAGIC;
    }
    if (detail::readUint16LittleEndian(input, 4) != CATALOG_VERSION) {
        return RunCatalogCodecError::UNSUPPORTED_VERSION;
    }
    if (detail::readUint16LittleEndian(input, 6) != RUN_CATALOG_V1_SIZE) {
        return RunCatalogCodecError::INVALID_TOTAL_LENGTH;
    }
    if (containsNonzeroByte(input.subspan(FIRST_RESERVED_OFFSET, FIRST_RESERVED_LENGTH)) ||
        containsNonzeroByte(input.subspan(FINAL_RESERVED_OFFSET, FINAL_RESERVED_LENGTH))) {
        return RunCatalogCodecError::NONZERO_RESERVED_BYTES;
    }
    if (detail::readUint32LittleEndian(input, CHECKSUM_OFFSET) != catalogChecksum(input)) {
        return RunCatalogCodecError::CHECKSUM_MISMATCH;
    }

    const std::uint8_t dispositionValue = std::to_integer<std::uint8_t>(input[ACTIVE_DISPOSITION_OFFSET]);
    if (!isKnownDispositionValue(dispositionValue)) {
        return RunCatalogCodecError::UNKNOWN_DISPOSITION;
    }

    RunCatalogSnapshotV1 decoded{
        .generation = detail::readUint64LittleEndian(input, GENERATION_OFFSET),
        .lastReservedRunId = decodedRunId(detail::readUint64LittleEndian(input, LAST_RESERVED_RUN_ID_OFFSET)),
        .activeRunId = decodedRunId(detail::readUint64LittleEndian(input, ACTIVE_RUN_ID_OFFSET)),
        .activeDisposition = static_cast<RunCatalogDisposition>(dispositionValue),
        .retainedStoppedRunId = decodedRunId(detail::readUint64LittleEndian(input, RETAINED_STOPPED_RUN_ID_OFFSET)),
    };
    const RunCatalogCodecError validationError = validateSnapshot(decoded);
    if (validationError != RunCatalogCodecError::NONE) {
        return validationError;
    }

    output = decoded;
    return RunCatalogCodecError::NONE;
}

} // namespace exchange::storage
