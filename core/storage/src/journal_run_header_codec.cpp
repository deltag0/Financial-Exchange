#include "journal_run_header_codec.hpp"

#include "journal_v1_envelope.hpp"
#include "little_endian.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace exchange::storage {
namespace {

constexpr std::uint16_t RUN_HEADER_RECORD_TYPE = 1;
constexpr std::uint32_t BEHAVIORAL_RULES_VERSION = 1;
constexpr std::uint32_t MAX_EVENTS_PER_COMMAND = 4'096;
constexpr std::size_t RUN_HEADER_FIXED_PAYLOAD_SIZE = 32;
constexpr std::size_t INSTRUMENT_ENTRY_SIZE = 12;
constexpr std::size_t RUN_HEADER_MAX_FRAME_SIZE = 65'536;

constexpr std::size_t MAX_EVENTS_OFFSET = detail::JOURNAL_V1_PAYLOAD_OFFSET + 4;
constexpr std::size_t MAX_RUN_COMMANDS_OFFSET = detail::JOURNAL_V1_PAYLOAD_OFFSET + 8;
constexpr std::size_t MAX_RUN_JOURNAL_BYTES_OFFSET = detail::JOURNAL_V1_PAYLOAD_OFFSET + 16;
constexpr std::size_t INSTRUMENT_COUNT_OFFSET = detail::JOURNAL_V1_PAYLOAD_OFFSET + 24;
constexpr std::size_t GENERATOR_CONFIG_LENGTH_OFFSET = detail::JOURNAL_V1_PAYLOAD_OFFSET + 28;
constexpr std::size_t INSTRUMENTS_OFFSET = detail::JOURNAL_V1_PAYLOAD_OFFSET + RUN_HEADER_FIXED_PAYLOAD_SIZE;

RunHeaderCodecError validateHeader(const RunHeaderV1& header, std::size_t& frameSize) noexcept {
    if (header.exchangeRunId.value() == 0) {
        return RunHeaderCodecError::ZERO_EXCHANGE_RUN_ID;
    }
    if (header.behavioralRulesVersion != BEHAVIORAL_RULES_VERSION) {
        return RunHeaderCodecError::UNSUPPORTED_BEHAVIORAL_RULES_VERSION;
    }
    if (header.maxEventsPerCommand != MAX_EVENTS_PER_COMMAND) {
        return RunHeaderCodecError::UNSUPPORTED_MAX_EVENTS_PER_COMMAND;
    }
    if (header.maxRunCommands == 0) {
        return RunHeaderCodecError::ZERO_MAX_RUN_COMMANDS;
    }
    std::uint64_t previousInstrumentId = 0;
    for (const auto& instrument : header.instruments) {
        if (instrument.instrumentId.value() == 0) {
            return RunHeaderCodecError::ZERO_INSTRUMENT_ID;
        }
        if (instrument.configurationVersion == 0) {
            return RunHeaderCodecError::ZERO_CONFIGURATION_VERSION;
        }
        if (instrument.instrumentId.value() <= previousInstrumentId) {
            return RunHeaderCodecError::INSTRUMENTS_NOT_STRICTLY_INCREASING;
        }
        previousInstrumentId = instrument.instrumentId.value();
    }

    constexpr std::size_t FIXED_FRAME_SIZE = detail::JOURNAL_V1_ENVELOPE_SIZE + RUN_HEADER_FIXED_PAYLOAD_SIZE;
    constexpr std::size_t MAX_INSTRUMENTS = (RUN_HEADER_MAX_FRAME_SIZE - FIXED_FRAME_SIZE) / INSTRUMENT_ENTRY_SIZE;
    if (header.instruments.size() > MAX_INSTRUMENTS) {
        return RunHeaderCodecError::FRAME_TOO_LARGE;
    }

    frameSize = FIXED_FRAME_SIZE + header.instruments.size() * INSTRUMENT_ENTRY_SIZE;
    if (header.maxRunJournalBytes < frameSize) {
        return RunHeaderCodecError::MAX_RUN_JOURNAL_BYTES_TOO_SMALL;
    }
    return RunHeaderCodecError::NONE;
}

} // namespace

RunHeaderCodecError encodeRunHeaderV1(const RunHeaderV1& header, std::vector<std::byte>& output) noexcept {
    std::size_t frameSize = 0;
    const auto validationError = validateHeader(header, frameSize);
    if (validationError != RunHeaderCodecError::NONE) {
        return validationError;
    }

    try {
        std::vector<std::byte> encoded(frameSize);
        const std::size_t payloadSize = frameSize - detail::JOURNAL_V1_ENVELOPE_SIZE;

        std::copy(detail::JOURNAL_V1_MAGIC.begin(), detail::JOURNAL_V1_MAGIC.end(), encoded.begin());
        detail::writeUint16LittleEndian(encoded, detail::JOURNAL_V1_FORMAT_VERSION_OFFSET,
                                        detail::JOURNAL_V1_FORMAT_VERSION);
        detail::writeUint16LittleEndian(encoded, detail::JOURNAL_V1_RECORD_TYPE_OFFSET, RUN_HEADER_RECORD_TYPE);
        detail::writeUint32LittleEndian(encoded, detail::JOURNAL_V1_TOTAL_LENGTH_OFFSET,
                                        static_cast<std::uint32_t>(frameSize));
        detail::writeUint64LittleEndian(encoded, detail::JOURNAL_V1_EXCHANGE_RUN_ID_OFFSET,
                                        header.exchangeRunId.value());
        detail::writeUint64LittleEndian(encoded, detail::JOURNAL_V1_COMMAND_SEQUENCE_OFFSET, 0);
        detail::writeUint32LittleEndian(encoded, detail::JOURNAL_V1_PAYLOAD_LENGTH_OFFSET,
                                        static_cast<std::uint32_t>(payloadSize));

        detail::writeUint32LittleEndian(encoded, detail::JOURNAL_V1_PAYLOAD_OFFSET, header.behavioralRulesVersion);
        detail::writeUint32LittleEndian(encoded, MAX_EVENTS_OFFSET, header.maxEventsPerCommand);
        detail::writeUint64LittleEndian(encoded, MAX_RUN_COMMANDS_OFFSET, header.maxRunCommands);
        detail::writeUint64LittleEndian(encoded, MAX_RUN_JOURNAL_BYTES_OFFSET, header.maxRunJournalBytes);
        detail::writeUint32LittleEndian(encoded, INSTRUMENT_COUNT_OFFSET,
                                        static_cast<std::uint32_t>(header.instruments.size()));
        detail::writeUint32LittleEndian(encoded, GENERATOR_CONFIG_LENGTH_OFFSET, 0);

        std::size_t offset = INSTRUMENTS_OFFSET;
        for (const auto& instrument : header.instruments) {
            detail::writeUint64LittleEndian(encoded, offset, instrument.instrumentId.value());
            detail::writeUint32LittleEndian(encoded, offset + sizeof(std::uint64_t), instrument.configurationVersion);
            offset += INSTRUMENT_ENTRY_SIZE;
        }

        detail::writeUint32LittleEndian(encoded, detail::JOURNAL_V1_CHECKSUM_OFFSET,
                                        detail::journalV1Checksum(encoded));
        output = std::move(encoded);
        return RunHeaderCodecError::NONE;
    } catch (const std::bad_alloc&) {
        return RunHeaderCodecError::ALLOCATION_FAILURE;
    }
}

RunHeaderCodecError decodeRunHeaderV1(const std::span<const std::byte> input, RunHeaderV1& output) noexcept {
    if (input.size() < detail::JOURNAL_V1_ENVELOPE_SIZE) {
        return RunHeaderCodecError::TRUNCATED_INPUT;
    }
    if (input.size() > RUN_HEADER_MAX_FRAME_SIZE) {
        return RunHeaderCodecError::FRAME_TOO_LARGE;
    }
    if (!std::equal(detail::JOURNAL_V1_MAGIC.begin(), detail::JOURNAL_V1_MAGIC.end(), input.begin())) {
        return RunHeaderCodecError::INVALID_MAGIC;
    }
    if (detail::readUint16LittleEndian(input, detail::JOURNAL_V1_FORMAT_VERSION_OFFSET) !=
        detail::JOURNAL_V1_FORMAT_VERSION) {
        return RunHeaderCodecError::UNSUPPORTED_FORMAT_VERSION;
    }
    if (detail::readUint16LittleEndian(input, detail::JOURNAL_V1_RECORD_TYPE_OFFSET) != RUN_HEADER_RECORD_TYPE) {
        return RunHeaderCodecError::UNSUPPORTED_RECORD_TYPE;
    }

    const std::uint32_t totalLength = detail::readUint32LittleEndian(input, detail::JOURNAL_V1_TOTAL_LENGTH_OFFSET);
    if (totalLength < detail::JOURNAL_V1_ENVELOPE_SIZE) {
        return RunHeaderCodecError::INVALID_TOTAL_LENGTH;
    }
    if (totalLength > RUN_HEADER_MAX_FRAME_SIZE) {
        return RunHeaderCodecError::FRAME_TOO_LARGE;
    }
    if (input.size() < totalLength) {
        return RunHeaderCodecError::TRUNCATED_INPUT;
    }
    if (input.size() > totalLength) {
        return RunHeaderCodecError::TRAILING_BYTES;
    }
    if (detail::readUint64LittleEndian(input, detail::JOURNAL_V1_COMMAND_SEQUENCE_OFFSET) != 0) {
        return RunHeaderCodecError::NONZERO_COMMAND_SEQUENCE;
    }

    const std::uint32_t payloadLength = detail::readUint32LittleEndian(input, detail::JOURNAL_V1_PAYLOAD_LENGTH_OFFSET);
    const std::uint64_t derivedTotalLength =
        static_cast<std::uint64_t>(detail::JOURNAL_V1_ENVELOPE_SIZE) + payloadLength;
    if (derivedTotalLength != totalLength || payloadLength < RUN_HEADER_FIXED_PAYLOAD_SIZE) {
        return RunHeaderCodecError::INVALID_PAYLOAD_LENGTH;
    }
    if (detail::readUint32LittleEndian(input, detail::JOURNAL_V1_CHECKSUM_OFFSET) != detail::journalV1Checksum(input)) {
        return RunHeaderCodecError::CHECKSUM_MISMATCH;
    }

    const std::uint32_t instrumentCount = detail::readUint32LittleEndian(input, INSTRUMENT_COUNT_OFFSET);
    const std::uint32_t generatorConfigLength = detail::readUint32LittleEndian(input, GENERATOR_CONFIG_LENGTH_OFFSET);
    const std::uint64_t instrumentBytes = static_cast<std::uint64_t>(instrumentCount) * INSTRUMENT_ENTRY_SIZE;
    const std::uint64_t expectedPayloadLength = RUN_HEADER_FIXED_PAYLOAD_SIZE + instrumentBytes + generatorConfigLength;
    if (expectedPayloadLength != payloadLength) {
        return RunHeaderCodecError::INVALID_PAYLOAD_LENGTH;
    }
    if (generatorConfigLength != 0) {
        return RunHeaderCodecError::UNSUPPORTED_GENERATOR_CONFIGURATION;
    }

    RunHeaderV1 decoded{
        .exchangeRunId =
            domain::ExchangeRunId{detail::readUint64LittleEndian(input, detail::JOURNAL_V1_EXCHANGE_RUN_ID_OFFSET)},
        .behavioralRulesVersion = detail::readUint32LittleEndian(input, detail::JOURNAL_V1_PAYLOAD_OFFSET),
        .maxEventsPerCommand = detail::readUint32LittleEndian(input, MAX_EVENTS_OFFSET),
        .maxRunCommands = detail::readUint64LittleEndian(input, MAX_RUN_COMMANDS_OFFSET),
        .maxRunJournalBytes = detail::readUint64LittleEndian(input, MAX_RUN_JOURNAL_BYTES_OFFSET),
        .instruments = {},
    };

    try {
        decoded.instruments.reserve(instrumentCount);
        std::size_t offset = INSTRUMENTS_OFFSET;
        for (std::uint32_t index = 0; index < instrumentCount; ++index) {
            const std::uint64_t instrumentId = detail::readUint64LittleEndian(input, offset);
            const std::uint32_t configurationVersion =
                detail::readUint32LittleEndian(input, offset + sizeof(std::uint64_t));
            decoded.instruments.push_back({domain::InstrumentId{instrumentId}, configurationVersion});
            offset += INSTRUMENT_ENTRY_SIZE;
        }
    } catch (const std::bad_alloc&) {
        return RunHeaderCodecError::ALLOCATION_FAILURE;
    }

    std::size_t validatedFrameSize = 0;
    const auto validationError = validateHeader(decoded, validatedFrameSize);
    if (validationError != RunHeaderCodecError::NONE) {
        return validationError;
    }

    output = std::move(decoded);
    return RunHeaderCodecError::NONE;
}

} // namespace exchange::storage
