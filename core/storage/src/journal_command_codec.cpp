#include "journal_command_codec.hpp"

#include "journal_v1_envelope.hpp"
#include "little_endian.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace exchange::storage {
namespace detail {

JournalCommandCodecError validateJournalV1CommandEnvelope(const std::span<const std::byte> input,
                                                          const std::uint16_t expectedRecordType,
                                                          const bool requireCompleteFrame,
                                                          JournalV1CommandEnvelope& output) noexcept {
    if (input.size() < detail::JOURNAL_V1_ENVELOPE_SIZE) {
        return JournalCommandCodecError::TRUNCATED_INPUT;
    }
    if (input.size() > detail::JOURNAL_V1_COMMAND_MAX_FRAME_SIZE) {
        return JournalCommandCodecError::FRAME_TOO_LARGE;
    }
    if (!std::equal(detail::JOURNAL_V1_MAGIC.begin(), detail::JOURNAL_V1_MAGIC.end(), input.begin())) {
        return JournalCommandCodecError::INVALID_MAGIC;
    }
    if (detail::readUint16LittleEndian(input, detail::JOURNAL_V1_FORMAT_VERSION_OFFSET) !=
        detail::JOURNAL_V1_FORMAT_VERSION) {
        return JournalCommandCodecError::UNSUPPORTED_FORMAT_VERSION;
    }
    const std::uint16_t recordType = detail::readUint16LittleEndian(input, detail::JOURNAL_V1_RECORD_TYPE_OFFSET);
    const bool supportedRecordType =
        recordType == detail::JOURNAL_V1_NEW_ORDER_RECORD_TYPE || recordType == detail::JOURNAL_V1_CANCEL_RECORD_TYPE;
    if (!supportedRecordType || (expectedRecordType != 0 && recordType != expectedRecordType)) {
        return JournalCommandCodecError::UNSUPPORTED_RECORD_TYPE;
    }

    const std::uint32_t totalLength = detail::readUint32LittleEndian(input, detail::JOURNAL_V1_TOTAL_LENGTH_OFFSET);
    if (totalLength < detail::JOURNAL_V1_ENVELOPE_SIZE) {
        return JournalCommandCodecError::INVALID_TOTAL_LENGTH;
    }
    if (totalLength > detail::JOURNAL_V1_COMMAND_MAX_FRAME_SIZE) {
        return JournalCommandCodecError::FRAME_TOO_LARGE;
    }
    if (requireCompleteFrame) {
        if (input.size() < totalLength) {
            return JournalCommandCodecError::TRUNCATED_INPUT;
        }
        if (input.size() > totalLength) {
            return JournalCommandCodecError::TRAILING_BYTES;
        }
    }

    const std::uint64_t exchangeRunId =
        detail::readUint64LittleEndian(input, detail::JOURNAL_V1_EXCHANGE_RUN_ID_OFFSET);
    if (exchangeRunId == 0) {
        return JournalCommandCodecError::ZERO_EXCHANGE_RUN_ID;
    }
    const std::uint64_t commandSequence =
        detail::readUint64LittleEndian(input, detail::JOURNAL_V1_COMMAND_SEQUENCE_OFFSET);
    if (commandSequence == 0) {
        return JournalCommandCodecError::ZERO_COMMAND_SEQUENCE;
    }

    const std::uint32_t payloadLength = detail::readUint32LittleEndian(input, detail::JOURNAL_V1_PAYLOAD_LENGTH_OFFSET);
    const std::uint64_t derivedTotalLength =
        static_cast<std::uint64_t>(detail::JOURNAL_V1_ENVELOPE_SIZE) + payloadLength;
    if (derivedTotalLength != totalLength) {
        return JournalCommandCodecError::INVALID_PAYLOAD_LENGTH;
    }
    if (requireCompleteFrame &&
        detail::readUint32LittleEndian(input, detail::JOURNAL_V1_CHECKSUM_OFFSET) != detail::journalV1Checksum(input)) {
        return JournalCommandCodecError::CHECKSUM_MISMATCH;
    }

    if (!requireCompleteFrame) {
        const std::uint32_t minimumPayloadLength =
            recordType == detail::JOURNAL_V1_NEW_ORDER_RECORD_TYPE
                ? detail::JOURNAL_V1_NEW_ORDER_PAYLOAD_SIZE_WITHOUT_CLIENT_COMMAND_ID +
                      detail::JOURNAL_V1_CLIENT_COMMAND_ID_MIN_LENGTH
                : detail::JOURNAL_V1_CANCEL_PAYLOAD_SIZE_WITHOUT_CLIENT_COMMAND_ID +
                      detail::JOURNAL_V1_CLIENT_COMMAND_ID_MIN_LENGTH;
        const std::uint32_t maximumPayloadLength =
            recordType == detail::JOURNAL_V1_NEW_ORDER_RECORD_TYPE
                ? detail::JOURNAL_V1_NEW_ORDER_PAYLOAD_SIZE_WITHOUT_CLIENT_COMMAND_ID +
                      detail::JOURNAL_V1_CLIENT_COMMAND_ID_MAX_LENGTH
                : detail::JOURNAL_V1_CANCEL_PAYLOAD_SIZE_WITHOUT_CLIENT_COMMAND_ID +
                      detail::JOURNAL_V1_CLIENT_COMMAND_ID_MAX_LENGTH;
        if (payloadLength < minimumPayloadLength || payloadLength > maximumPayloadLength) {
            return JournalCommandCodecError::INVALID_PAYLOAD_LENGTH;
        }
    }

    output = {
        .recordType = recordType,
        .totalLength = totalLength,
        .exchangeRunId = exchangeRunId,
        .commandSequence = commandSequence,
        .payloadLength = payloadLength,
    };
    return JournalCommandCodecError::NONE;
}

} // namespace detail

namespace {

constexpr std::uint32_t BEHAVIORAL_RULES_VERSION = 1;

constexpr std::size_t RULES_VERSION_PAYLOAD_OFFSET = 0;
constexpr std::size_t CONFIGURATION_VERSION_PAYLOAD_OFFSET = 4;
constexpr std::size_t CLIENT_ID_PAYLOAD_OFFSET = 8;
constexpr std::size_t INSTRUMENT_ID_PAYLOAD_OFFSET = 16;
constexpr std::size_t CLIENT_COMMAND_ID_LENGTH_PAYLOAD_OFFSET = 24;
constexpr std::size_t CLIENT_COMMAND_ID_PAYLOAD_OFFSET = 25;

constexpr std::uint8_t BUY_WIRE_CODE = 1;
constexpr std::uint8_t SELL_WIRE_CODE = 2;
constexpr std::uint8_t GTC_WIRE_CODE = 1;
constexpr std::uint8_t IOC_WIRE_CODE = 2;

JournalCommandCodecError validateCommonCommand(const domain::ExchangeRunId exchangeRunId,
                                               const domain::CommandSequence commandSequence,
                                               const std::uint32_t behavioralRulesVersion,
                                               const std::uint32_t configurationVersion,
                                               const domain::ClientId clientId,
                                               const domain::InstrumentId instrumentId) noexcept {
    if (exchangeRunId.value() == 0) {
        return JournalCommandCodecError::ZERO_EXCHANGE_RUN_ID;
    }
    if (commandSequence.value() == 0) {
        return JournalCommandCodecError::ZERO_COMMAND_SEQUENCE;
    }
    if (behavioralRulesVersion != BEHAVIORAL_RULES_VERSION) {
        return JournalCommandCodecError::UNSUPPORTED_BEHAVIORAL_RULES_VERSION;
    }
    if (configurationVersion == 0) {
        return JournalCommandCodecError::ZERO_CONFIGURATION_VERSION;
    }
    if (clientId.value() == 0) {
        return JournalCommandCodecError::ZERO_CLIENT_ID;
    }
    if (instrumentId.value() == 0) {
        return JournalCommandCodecError::ZERO_INSTRUMENT_ID;
    }
    return JournalCommandCodecError::NONE;
}

void writeEnvelope(std::span<std::byte> frame, const std::uint16_t recordType,
                   const domain::ExchangeRunId exchangeRunId, const domain::CommandSequence commandSequence,
                   const std::size_t payloadLength) noexcept {
    std::copy(detail::JOURNAL_V1_MAGIC.begin(), detail::JOURNAL_V1_MAGIC.end(), frame.begin());
    detail::writeUint16LittleEndian(frame, detail::JOURNAL_V1_FORMAT_VERSION_OFFSET, detail::JOURNAL_V1_FORMAT_VERSION);
    detail::writeUint16LittleEndian(frame, detail::JOURNAL_V1_RECORD_TYPE_OFFSET, recordType);
    detail::writeUint32LittleEndian(frame, detail::JOURNAL_V1_TOTAL_LENGTH_OFFSET,
                                    static_cast<std::uint32_t>(detail::JOURNAL_V1_ENVELOPE_SIZE + payloadLength));
    detail::writeUint64LittleEndian(frame, detail::JOURNAL_V1_EXCHANGE_RUN_ID_OFFSET, exchangeRunId.value());
    detail::writeUint64LittleEndian(frame, detail::JOURNAL_V1_COMMAND_SEQUENCE_OFFSET, commandSequence.value());
    detail::writeUint32LittleEndian(frame, detail::JOURNAL_V1_PAYLOAD_LENGTH_OFFSET,
                                    static_cast<std::uint32_t>(payloadLength));
}

} // namespace

JournalCommandCodecError encodeNewOrderV1(const JournalNewOrderV1& command, std::vector<std::byte>& output) noexcept {
    const std::string_view clientCommandId = command.clientCommandId.value();
    const auto commonError =
        validateCommonCommand(command.exchangeRunId, command.commandSequence, command.behavioralRulesVersion,
                              command.configurationVersion, command.clientId, command.instrumentId);
    if (commonError != JournalCommandCodecError::NONE) {
        return commonError;
    }

    std::uint8_t side = 0;
    switch (command.side) {
        case domain::Side::BUY:
            side = BUY_WIRE_CODE;
            break;
        case domain::Side::SELL:
            side = SELL_WIRE_CODE;
            break;
        default:
            return JournalCommandCodecError::UNSUPPORTED_SIDE;
    }
    std::uint8_t timeInForce = 0;
    switch (command.timeInForce) {
        case core::task::TimeInForce::GTC:
            timeInForce = GTC_WIRE_CODE;
            break;
        case core::task::TimeInForce::IOC:
            timeInForce = IOC_WIRE_CODE;
            break;
        default:
            return JournalCommandCodecError::UNSUPPORTED_TIME_IN_FORCE;
    }
    if (command.price.value() == 0) {
        return JournalCommandCodecError::ZERO_PRICE;
    }
    if (command.quantity.value() == 0) {
        return JournalCommandCodecError::ZERO_QUANTITY;
    }

    const std::size_t payloadLength =
        detail::JOURNAL_V1_NEW_ORDER_PAYLOAD_SIZE_WITHOUT_CLIENT_COMMAND_ID + clientCommandId.size();
    const std::size_t frameSize = detail::JOURNAL_V1_ENVELOPE_SIZE + payloadLength;

    try {
        std::vector<std::byte> encoded(frameSize);
        writeEnvelope(encoded, detail::JOURNAL_V1_NEW_ORDER_RECORD_TYPE, command.exchangeRunId, command.commandSequence,
                      payloadLength);
        detail::writeUint32LittleEndian(encoded, detail::JOURNAL_V1_PAYLOAD_OFFSET + RULES_VERSION_PAYLOAD_OFFSET,
                                        command.behavioralRulesVersion);
        detail::writeUint32LittleEndian(encoded,
                                        detail::JOURNAL_V1_PAYLOAD_OFFSET + CONFIGURATION_VERSION_PAYLOAD_OFFSET,
                                        command.configurationVersion);
        detail::writeUint64LittleEndian(encoded, detail::JOURNAL_V1_PAYLOAD_OFFSET + CLIENT_ID_PAYLOAD_OFFSET,
                                        command.clientId.value());
        detail::writeUint64LittleEndian(encoded, detail::JOURNAL_V1_PAYLOAD_OFFSET + INSTRUMENT_ID_PAYLOAD_OFFSET,
                                        command.instrumentId.value());
        encoded[detail::JOURNAL_V1_PAYLOAD_OFFSET + CLIENT_COMMAND_ID_LENGTH_PAYLOAD_OFFSET] =
            std::byte{static_cast<std::uint8_t>(clientCommandId.size())};
        std::transform(clientCommandId.begin(), clientCommandId.end(),
                       encoded.begin() + detail::JOURNAL_V1_PAYLOAD_OFFSET + CLIENT_COMMAND_ID_PAYLOAD_OFFSET,
                       [](const char byte) { return std::byte{static_cast<unsigned char>(byte)}; });

        const std::size_t variableFieldsOffset =
            detail::JOURNAL_V1_PAYLOAD_OFFSET + CLIENT_COMMAND_ID_PAYLOAD_OFFSET + clientCommandId.size();
        encoded[variableFieldsOffset] = std::byte{side};
        encoded[variableFieldsOffset + 1] = std::byte{timeInForce};
        detail::writeUint64LittleEndian(encoded, variableFieldsOffset + 2, command.price.value());
        detail::writeUint64LittleEndian(encoded, variableFieldsOffset + 10, command.quantity.value());
        detail::writeUint32LittleEndian(encoded, detail::JOURNAL_V1_CHECKSUM_OFFSET,
                                        detail::journalV1Checksum(encoded));

        output = std::move(encoded);
        return JournalCommandCodecError::NONE;
    } catch (const std::bad_alloc&) {
        return JournalCommandCodecError::ALLOCATION_FAILURE;
    }
}

JournalCommandCodecError decodeNewOrderV1(const std::span<const std::byte> input, JournalNewOrderV1& output) noexcept {
    detail::JournalV1CommandEnvelope envelope;
    const auto envelopeError =
        detail::validateJournalV1CommandEnvelope(input, detail::JOURNAL_V1_NEW_ORDER_RECORD_TYPE, true, envelope);
    if (envelopeError != JournalCommandCodecError::NONE) {
        return envelopeError;
    }
    if (envelope.payloadLength < detail::JOURNAL_V1_NEW_ORDER_PAYLOAD_SIZE_WITHOUT_CLIENT_COMMAND_ID) {
        return JournalCommandCodecError::INVALID_PAYLOAD_LENGTH;
    }
    const std::uint8_t clientCommandIdLength = std::to_integer<std::uint8_t>(
        input[detail::JOURNAL_V1_PAYLOAD_OFFSET + CLIENT_COMMAND_ID_LENGTH_PAYLOAD_OFFSET]);
    const std::size_t expectedPayloadLength =
        detail::JOURNAL_V1_NEW_ORDER_PAYLOAD_SIZE_WITHOUT_CLIENT_COMMAND_ID + clientCommandIdLength;
    if (envelope.payloadLength != expectedPayloadLength) {
        return JournalCommandCodecError::INVALID_PAYLOAD_LENGTH;
    }

    const auto clientCommandBytes =
        input.subspan(detail::JOURNAL_V1_PAYLOAD_OFFSET + CLIENT_COMMAND_ID_PAYLOAD_OFFSET, clientCommandIdLength);
    const std::string_view clientCommandId{reinterpret_cast<const char*>(clientCommandBytes.data()),
                                           clientCommandBytes.size()};

    const std::uint32_t behavioralRulesVersion =
        detail::readUint32LittleEndian(input, detail::JOURNAL_V1_PAYLOAD_OFFSET + RULES_VERSION_PAYLOAD_OFFSET);
    const std::uint32_t configurationVersion =
        detail::readUint32LittleEndian(input, detail::JOURNAL_V1_PAYLOAD_OFFSET + CONFIGURATION_VERSION_PAYLOAD_OFFSET);
    const domain::ClientId clientId{
        detail::readUint64LittleEndian(input, detail::JOURNAL_V1_PAYLOAD_OFFSET + CLIENT_ID_PAYLOAD_OFFSET)};
    const domain::InstrumentId instrumentId{
        detail::readUint64LittleEndian(input, detail::JOURNAL_V1_PAYLOAD_OFFSET + INSTRUMENT_ID_PAYLOAD_OFFSET)};
    const auto commonError = validateCommonCommand(
        domain::ExchangeRunId{envelope.exchangeRunId}, domain::CommandSequence{envelope.commandSequence},
        behavioralRulesVersion, configurationVersion, clientId, instrumentId);
    if (commonError != JournalCommandCodecError::NONE) {
        return commonError;
    }

    const std::size_t variableFieldsOffset =
        detail::JOURNAL_V1_PAYLOAD_OFFSET + CLIENT_COMMAND_ID_PAYLOAD_OFFSET + clientCommandIdLength;
    domain::Side side = domain::Side::BUY;
    switch (std::to_integer<std::uint8_t>(input[variableFieldsOffset])) {
        case BUY_WIRE_CODE:
            side = domain::Side::BUY;
            break;
        case SELL_WIRE_CODE:
            side = domain::Side::SELL;
            break;
        default:
            return JournalCommandCodecError::UNSUPPORTED_SIDE;
    }
    core::task::TimeInForce timeInForce = core::task::TimeInForce::GTC;
    switch (std::to_integer<std::uint8_t>(input[variableFieldsOffset + 1])) {
        case GTC_WIRE_CODE:
            timeInForce = core::task::TimeInForce::GTC;
            break;
        case IOC_WIRE_CODE:
            timeInForce = core::task::TimeInForce::IOC;
            break;
        default:
            return JournalCommandCodecError::UNSUPPORTED_TIME_IN_FORCE;
    }
    const domain::Price price{detail::readUint64LittleEndian(input, variableFieldsOffset + 2)};
    if (price.value() == 0) {
        return JournalCommandCodecError::ZERO_PRICE;
    }
    const domain::Quantity quantity{detail::readUint64LittleEndian(input, variableFieldsOffset + 10)};
    if (quantity.value() == 0) {
        return JournalCommandCodecError::ZERO_QUANTITY;
    }

    try {
        JournalNewOrderV1 decoded{
            .exchangeRunId = domain::ExchangeRunId{envelope.exchangeRunId},
            .commandSequence = domain::CommandSequence{envelope.commandSequence},
            .behavioralRulesVersion = behavioralRulesVersion,
            .configurationVersion = configurationVersion,
            .clientId = clientId,
            .instrumentId = instrumentId,
            .clientCommandId = domain::ClientCommandId{clientCommandId},
            .side = side,
            .timeInForce = timeInForce,
            .price = price,
            .quantity = quantity,
        };
        output = decoded;
        return JournalCommandCodecError::NONE;
    } catch (const std::invalid_argument&) {
        return JournalCommandCodecError::INVALID_CLIENT_COMMAND_ID;
    }
}

JournalCommandCodecError encodeCancelV1(const JournalCancelV1& command, std::vector<std::byte>& output) noexcept {
    const std::string_view clientCommandId = command.clientCommandId.value();
    const auto commonError =
        validateCommonCommand(command.exchangeRunId, command.commandSequence, command.behavioralRulesVersion,
                              command.configurationVersion, command.clientId, command.instrumentId);
    if (commonError != JournalCommandCodecError::NONE) {
        return commonError;
    }
    if (command.targetOrderId.value() == 0) {
        return JournalCommandCodecError::ZERO_TARGET_ORDER_ID;
    }

    const std::size_t payloadLength =
        detail::JOURNAL_V1_CANCEL_PAYLOAD_SIZE_WITHOUT_CLIENT_COMMAND_ID + clientCommandId.size();
    const std::size_t frameSize = detail::JOURNAL_V1_ENVELOPE_SIZE + payloadLength;

    try {
        std::vector<std::byte> encoded(frameSize);
        writeEnvelope(encoded, detail::JOURNAL_V1_CANCEL_RECORD_TYPE, command.exchangeRunId, command.commandSequence,
                      payloadLength);
        detail::writeUint32LittleEndian(encoded, detail::JOURNAL_V1_PAYLOAD_OFFSET + RULES_VERSION_PAYLOAD_OFFSET,
                                        command.behavioralRulesVersion);
        detail::writeUint32LittleEndian(encoded,
                                        detail::JOURNAL_V1_PAYLOAD_OFFSET + CONFIGURATION_VERSION_PAYLOAD_OFFSET,
                                        command.configurationVersion);
        detail::writeUint64LittleEndian(encoded, detail::JOURNAL_V1_PAYLOAD_OFFSET + CLIENT_ID_PAYLOAD_OFFSET,
                                        command.clientId.value());
        detail::writeUint64LittleEndian(encoded, detail::JOURNAL_V1_PAYLOAD_OFFSET + INSTRUMENT_ID_PAYLOAD_OFFSET,
                                        command.instrumentId.value());
        encoded[detail::JOURNAL_V1_PAYLOAD_OFFSET + CLIENT_COMMAND_ID_LENGTH_PAYLOAD_OFFSET] =
            std::byte{static_cast<std::uint8_t>(clientCommandId.size())};
        std::transform(clientCommandId.begin(), clientCommandId.end(),
                       encoded.begin() + detail::JOURNAL_V1_PAYLOAD_OFFSET + CLIENT_COMMAND_ID_PAYLOAD_OFFSET,
                       [](const char byte) { return std::byte{static_cast<unsigned char>(byte)}; });
        detail::writeUint64LittleEndian(
            encoded, detail::JOURNAL_V1_PAYLOAD_OFFSET + CLIENT_COMMAND_ID_PAYLOAD_OFFSET + clientCommandId.size(),
            command.targetOrderId.value());
        detail::writeUint32LittleEndian(encoded, detail::JOURNAL_V1_CHECKSUM_OFFSET,
                                        detail::journalV1Checksum(encoded));

        output = std::move(encoded);
        return JournalCommandCodecError::NONE;
    } catch (const std::bad_alloc&) {
        return JournalCommandCodecError::ALLOCATION_FAILURE;
    }
}

JournalCommandCodecError decodeCancelV1(const std::span<const std::byte> input, JournalCancelV1& output) noexcept {
    detail::JournalV1CommandEnvelope envelope;
    const auto envelopeError =
        detail::validateJournalV1CommandEnvelope(input, detail::JOURNAL_V1_CANCEL_RECORD_TYPE, true, envelope);
    if (envelopeError != JournalCommandCodecError::NONE) {
        return envelopeError;
    }
    if (envelope.payloadLength < detail::JOURNAL_V1_CANCEL_PAYLOAD_SIZE_WITHOUT_CLIENT_COMMAND_ID) {
        return JournalCommandCodecError::INVALID_PAYLOAD_LENGTH;
    }
    const std::uint8_t clientCommandIdLength = std::to_integer<std::uint8_t>(
        input[detail::JOURNAL_V1_PAYLOAD_OFFSET + CLIENT_COMMAND_ID_LENGTH_PAYLOAD_OFFSET]);
    const std::size_t expectedPayloadLength =
        detail::JOURNAL_V1_CANCEL_PAYLOAD_SIZE_WITHOUT_CLIENT_COMMAND_ID + clientCommandIdLength;
    if (envelope.payloadLength != expectedPayloadLength) {
        return JournalCommandCodecError::INVALID_PAYLOAD_LENGTH;
    }

    const auto clientCommandBytes =
        input.subspan(detail::JOURNAL_V1_PAYLOAD_OFFSET + CLIENT_COMMAND_ID_PAYLOAD_OFFSET, clientCommandIdLength);
    const std::string_view clientCommandId{reinterpret_cast<const char*>(clientCommandBytes.data()),
                                           clientCommandBytes.size()};

    const std::uint32_t behavioralRulesVersion =
        detail::readUint32LittleEndian(input, detail::JOURNAL_V1_PAYLOAD_OFFSET + RULES_VERSION_PAYLOAD_OFFSET);
    const std::uint32_t configurationVersion =
        detail::readUint32LittleEndian(input, detail::JOURNAL_V1_PAYLOAD_OFFSET + CONFIGURATION_VERSION_PAYLOAD_OFFSET);
    const domain::ClientId clientId{
        detail::readUint64LittleEndian(input, detail::JOURNAL_V1_PAYLOAD_OFFSET + CLIENT_ID_PAYLOAD_OFFSET)};
    const domain::InstrumentId instrumentId{
        detail::readUint64LittleEndian(input, detail::JOURNAL_V1_PAYLOAD_OFFSET + INSTRUMENT_ID_PAYLOAD_OFFSET)};
    const auto commonError = validateCommonCommand(
        domain::ExchangeRunId{envelope.exchangeRunId}, domain::CommandSequence{envelope.commandSequence},
        behavioralRulesVersion, configurationVersion, clientId, instrumentId);
    if (commonError != JournalCommandCodecError::NONE) {
        return commonError;
    }

    const domain::TargetOrderId targetOrderId{detail::readUint64LittleEndian(
        input, detail::JOURNAL_V1_PAYLOAD_OFFSET + CLIENT_COMMAND_ID_PAYLOAD_OFFSET + clientCommandIdLength)};
    if (targetOrderId.value() == 0) {
        return JournalCommandCodecError::ZERO_TARGET_ORDER_ID;
    }

    try {
        JournalCancelV1 decoded{
            .exchangeRunId = domain::ExchangeRunId{envelope.exchangeRunId},
            .commandSequence = domain::CommandSequence{envelope.commandSequence},
            .behavioralRulesVersion = behavioralRulesVersion,
            .configurationVersion = configurationVersion,
            .clientId = clientId,
            .instrumentId = instrumentId,
            .clientCommandId = domain::ClientCommandId{clientCommandId},
            .targetOrderId = targetOrderId,
        };
        output = decoded;
        return JournalCommandCodecError::NONE;
    } catch (const std::invalid_argument&) {
        return JournalCommandCodecError::INVALID_CLIENT_COMMAND_ID;
    }
}

} // namespace exchange::storage
