#include "journal_command_codec.hpp"

#include "crc32c.hpp"

#include <gtest/gtest.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace exchange::storage {
namespace {

constexpr std::size_t ENVELOPE_SIZE = 36;
constexpr std::size_t CHECKSUM_OFFSET = 32;
constexpr std::size_t PAYLOAD_OFFSET = ENVELOPE_SIZE;
constexpr std::size_t RULES_VERSION_OFFSET = PAYLOAD_OFFSET;
constexpr std::size_t CONFIGURATION_VERSION_OFFSET = PAYLOAD_OFFSET + 4;
constexpr std::size_t CLIENT_ID_OFFSET = PAYLOAD_OFFSET + 8;
constexpr std::size_t INSTRUMENT_ID_OFFSET = PAYLOAD_OFFSET + 16;
constexpr std::size_t CLIENT_COMMAND_ID_LENGTH_OFFSET = PAYLOAD_OFFSET + 24;
constexpr std::size_t CLIENT_COMMAND_ID_OFFSET = PAYLOAD_OFFSET + 25;
constexpr std::size_t MAX_FRAME_SIZE = 256;

constexpr std::array<std::byte, 82> GOLDEN_NEW_ORDER{
    std::byte{0x46}, std::byte{0x58}, std::byte{0x4A}, std::byte{0x52}, std::byte{0x01}, std::byte{0x00},
    std::byte{0x02}, std::byte{0x00}, std::byte{0x52}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05}, std::byte{0x04}, std::byte{0x03},
    std::byte{0x02}, std::byte{0x01}, std::byte{0x18}, std::byte{0x17}, std::byte{0x16}, std::byte{0x15},
    std::byte{0x14}, std::byte{0x13}, std::byte{0x12}, std::byte{0x11}, std::byte{0x2E}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x28}, std::byte{0x85}, std::byte{0x62}, std::byte{0x48},
    std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x24}, std::byte{0x23},
    std::byte{0x22}, std::byte{0x21}, std::byte{0x38}, std::byte{0x37}, std::byte{0x36}, std::byte{0x35},
    std::byte{0x34}, std::byte{0x33}, std::byte{0x32}, std::byte{0x31}, std::byte{0x48}, std::byte{0x47},
    std::byte{0x46}, std::byte{0x45}, std::byte{0x44}, std::byte{0x43}, std::byte{0x42}, std::byte{0x41},
    std::byte{0x03}, std::byte{0x41}, std::byte{0x42}, std::byte{0x43}, std::byte{0x01}, std::byte{0x02},
    std::byte{0x58}, std::byte{0x57}, std::byte{0x56}, std::byte{0x55}, std::byte{0x54}, std::byte{0x53},
    std::byte{0x52}, std::byte{0x51}, std::byte{0x68}, std::byte{0x67}, std::byte{0x66}, std::byte{0x65},
    std::byte{0x64}, std::byte{0x63}, std::byte{0x62}, std::byte{0x61},
};

constexpr std::array<std::byte, 72> GOLDEN_CANCEL{
    std::byte{0x46}, std::byte{0x58}, std::byte{0x4A}, std::byte{0x52}, std::byte{0x01}, std::byte{0x00},
    std::byte{0x03}, std::byte{0x00}, std::byte{0x48}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x78}, std::byte{0x77}, std::byte{0x76}, std::byte{0x75}, std::byte{0x74}, std::byte{0x73},
    std::byte{0x72}, std::byte{0x71}, std::byte{0x01}, std::byte{0x01}, std::byte{0x01}, std::byte{0x01},
    std::byte{0x01}, std::byte{0x01}, std::byte{0x01}, std::byte{0x01}, std::byte{0x24}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x36}, std::byte{0x87}, std::byte{0x89}, std::byte{0xD3},
    std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x14}, std::byte{0x13},
    std::byte{0x12}, std::byte{0x11}, std::byte{0x28}, std::byte{0x27}, std::byte{0x26}, std::byte{0x25},
    std::byte{0x24}, std::byte{0x23}, std::byte{0x22}, std::byte{0x21}, std::byte{0x38}, std::byte{0x37},
    std::byte{0x36}, std::byte{0x35}, std::byte{0x34}, std::byte{0x33}, std::byte{0x32}, std::byte{0x31},
    std::byte{0x03}, std::byte{0x43}, std::byte{0x58}, std::byte{0x4C}, std::byte{0x48}, std::byte{0x47},
    std::byte{0x46}, std::byte{0x45}, std::byte{0x44}, std::byte{0x43}, std::byte{0x42}, std::byte{0x41},
};

JournalNewOrderV1 goldenNewOrder() {
    return {
        .exchangeRunId = domain::ExchangeRunId{0x0102030405060708ULL},
        .commandSequence = domain::CommandSequence{0x1112131415161718ULL},
        .behavioralRulesVersion = 1,
        .configurationVersion = 0x21222324U,
        .clientId = domain::ClientId{0x3132333435363738ULL},
        .instrumentId = domain::InstrumentId{0x4142434445464748ULL},
        .clientCommandId = domain::ClientCommandId{"ABC"},
        .side = domain::Side::BUY,
        .timeInForce = core::task::TimeInForce::IOC,
        .price = domain::Price{0x5152535455565758ULL},
        .quantity = domain::Quantity{0x6162636465666768ULL},
    };
}

JournalCancelV1 goldenCancel() {
    return {
        .exchangeRunId = domain::ExchangeRunId{0x7172737475767778ULL},
        .commandSequence = domain::CommandSequence{0x0101010101010101ULL},
        .behavioralRulesVersion = 1,
        .configurationVersion = 0x11121314U,
        .clientId = domain::ClientId{0x2122232425262728ULL},
        .instrumentId = domain::InstrumentId{0x3132333435363738ULL},
        .clientCommandId = domain::ClientCommandId{"CXL"},
        .targetOrderId = domain::TargetOrderId{0x4142434445464748ULL},
    };
}

JournalNewOrderV1 validNewOrder(const std::string_view clientCommandId = "N") {
    return {
        .exchangeRunId = domain::ExchangeRunId{1},
        .commandSequence = domain::CommandSequence{2},
        .behavioralRulesVersion = 1,
        .configurationVersion = 3,
        .clientId = domain::ClientId{4},
        .instrumentId = domain::InstrumentId{5},
        .clientCommandId = domain::ClientCommandId{clientCommandId},
        .side = domain::Side::SELL,
        .timeInForce = core::task::TimeInForce::GTC,
        .price = domain::Price{6},
        .quantity = domain::Quantity{7},
    };
}

JournalCancelV1 validCancel(const std::string_view clientCommandId = "C") {
    return {
        .exchangeRunId = domain::ExchangeRunId{8},
        .commandSequence = domain::CommandSequence{9},
        .behavioralRulesVersion = 1,
        .configurationVersion = 10,
        .clientId = domain::ClientId{11},
        .instrumentId = domain::InstrumentId{12},
        .clientCommandId = domain::ClientCommandId{clientCommandId},
        .targetOrderId = domain::TargetOrderId{13},
    };
}

JournalNewOrderV1 sentinelNewOrder() {
    auto command = validNewOrder("UNCHANGED-NEW");
    command.exchangeRunId = domain::ExchangeRunId{900};
    return command;
}

JournalCancelV1 sentinelCancel() {
    auto command = validCancel("UNCHANGED-CANCEL");
    command.exchangeRunId = domain::ExchangeRunId{901};
    return command;
}

std::vector<std::byte> encodeValidNewOrder() {
    std::vector<std::byte> frame;
    EXPECT_EQ(encodeNewOrderV1(validNewOrder(), frame), JournalCommandCodecError::NONE);
    return frame;
}

std::vector<std::byte> encodeValidCancel() {
    std::vector<std::byte> frame;
    EXPECT_EQ(encodeCancelV1(validCancel(), frame), JournalCommandCodecError::NONE);
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

void rewriteDeclaredLengths(std::vector<std::byte>& frame) {
    writeUint32LittleEndian(frame, 8, static_cast<std::uint32_t>(frame.size()));
    writeUint32LittleEndian(frame, 28, static_cast<std::uint32_t>(frame.size() - ENVELOPE_SIZE));
}

void expectNewOrderDecodeErrorAndUnchanged(const std::span<const std::byte> frame,
                                           const JournalCommandCodecError expectedError) {
    auto output = sentinelNewOrder();
    const auto unchanged = output;
    EXPECT_EQ(decodeNewOrderV1(frame, output), expectedError);
    EXPECT_EQ(output, unchanged);
}

void expectCancelDecodeErrorAndUnchanged(const std::span<const std::byte> frame,
                                         const JournalCommandCodecError expectedError) {
    auto output = sentinelCancel();
    const auto unchanged = output;
    EXPECT_EQ(decodeCancelV1(frame, output), expectedError);
    EXPECT_EQ(output, unchanged);
}

void expectNewOrderEncodeErrorAndUnchanged(const JournalNewOrderV1& command,
                                           const JournalCommandCodecError expectedError) {
    std::vector<std::byte> output{std::byte{0xA5}};
    const auto unchanged = output;
    EXPECT_EQ(encodeNewOrderV1(command, output), expectedError);
    EXPECT_EQ(output, unchanged);
}

void expectCancelEncodeErrorAndUnchanged(const JournalCancelV1& command, const JournalCommandCodecError expectedError) {
    std::vector<std::byte> output{std::byte{0xA5}};
    const auto unchanged = output;
    EXPECT_EQ(encodeCancelV1(command, output), expectedError);
    EXPECT_EQ(output, unchanged);
}

template <typename T>
concept HasOrderId = requires(T value) {
    value.orderId;
};

template <typename T>
concept HasSessionId = requires(T value) {
    value.sessionId;
};

template <typename T>
concept HasReceiptTime = requires(T value) {
    value.receiptTime;
};

template <typename T>
concept HasShardId = requires(T value) {
    value.shard_id;
};

template <typename T>
concept HasSymbol = requires(T value) {
    value.symbol;
};

template <typename T>
concept HasExpiry = requires(T value) {
    value.expiry;
};

static_assert(std::same_as<decltype(JournalNewOrderV1::configurationVersion), std::uint32_t>);
static_assert(std::same_as<decltype(JournalCancelV1::configurationVersion), std::uint32_t>);
static_assert(!HasOrderId<JournalNewOrderV1>);
static_assert(!HasOrderId<JournalCancelV1>);
static_assert(!HasSessionId<JournalNewOrderV1>);
static_assert(!HasSessionId<JournalCancelV1>);
static_assert(!HasReceiptTime<JournalNewOrderV1>);
static_assert(!HasReceiptTime<JournalCancelV1>);
static_assert(!HasShardId<JournalNewOrderV1>);
static_assert(!HasShardId<JournalCancelV1>);
static_assert(!HasSymbol<JournalNewOrderV1>);
static_assert(!HasSymbol<JournalCancelV1>);
static_assert(!HasExpiry<JournalNewOrderV1>);
static_assert(!HasExpiry<JournalCancelV1>);
static_assert(noexcept(encodeNewOrderV1(std::declval<const JournalNewOrderV1&>(),
                                        std::declval<std::vector<std::byte>&>())));
static_assert(noexcept(decodeNewOrderV1(std::declval<std::span<const std::byte>>(),
                                        std::declval<JournalNewOrderV1&>())));
static_assert(noexcept(encodeCancelV1(std::declval<const JournalCancelV1&>(),
                                      std::declval<std::vector<std::byte>&>())));
static_assert(noexcept(decodeCancelV1(std::declval<std::span<const std::byte>>(), std::declval<JournalCancelV1&>())));

TEST(JournalCommandCodecTest, EncodesIndependentNewOrderGoldenBytesAndDecodesThem) {
    std::vector<std::byte> encoded;
    ASSERT_EQ(encodeNewOrderV1(goldenNewOrder(), encoded), JournalCommandCodecError::NONE);
    EXPECT_EQ(encoded, std::vector<std::byte>(GOLDEN_NEW_ORDER.begin(), GOLDEN_NEW_ORDER.end()));

    auto decoded = sentinelNewOrder();
    ASSERT_EQ(decodeNewOrderV1(GOLDEN_NEW_ORDER, decoded), JournalCommandCodecError::NONE);
    EXPECT_EQ(decoded, goldenNewOrder());
}

TEST(JournalCommandCodecTest, EncodesIndependentCancelGoldenBytesAndDecodesThem) {
    std::vector<std::byte> encoded;
    ASSERT_EQ(encodeCancelV1(goldenCancel(), encoded), JournalCommandCodecError::NONE);
    EXPECT_EQ(encoded, std::vector<std::byte>(GOLDEN_CANCEL.begin(), GOLDEN_CANCEL.end()));

    auto decoded = sentinelCancel();
    ASSERT_EQ(decodeCancelV1(GOLDEN_CANCEL, decoded), JournalCommandCodecError::NONE);
    EXPECT_EQ(decoded, goldenCancel());
}

TEST(JournalCommandCodecTest, RoundTripsOneAndSixtyFourByteIdsWithUint64Boundaries) {
    const std::string maximumClientCommandId(domain::ClientCommandId::MAX_LENGTH, 'X');
    auto newOrder = validNewOrder("X");
    newOrder.exchangeRunId = domain::ExchangeRunId{std::numeric_limits<std::uint64_t>::max()};
    newOrder.commandSequence = domain::CommandSequence{std::numeric_limits<std::uint64_t>::max()};
    newOrder.configurationVersion = std::numeric_limits<std::uint32_t>::max();
    newOrder.clientId = domain::ClientId{std::numeric_limits<std::uint64_t>::max()};
    newOrder.instrumentId = domain::InstrumentId{std::numeric_limits<std::uint64_t>::max()};
    newOrder.price = domain::Price{std::numeric_limits<std::uint64_t>::max()};
    newOrder.quantity = domain::Quantity{std::numeric_limits<std::uint64_t>::max()};

    std::vector<std::byte> frame;
    ASSERT_EQ(encodeNewOrderV1(newOrder, frame), JournalCommandCodecError::NONE);
    auto decodedNewOrder = sentinelNewOrder();
    ASSERT_EQ(decodeNewOrderV1(frame, decodedNewOrder), JournalCommandCodecError::NONE);
    EXPECT_EQ(decodedNewOrder, newOrder);

    auto cancel = validCancel(maximumClientCommandId);
    cancel.exchangeRunId = domain::ExchangeRunId{std::numeric_limits<std::uint64_t>::max()};
    cancel.commandSequence = domain::CommandSequence{std::numeric_limits<std::uint64_t>::max()};
    cancel.configurationVersion = std::numeric_limits<std::uint32_t>::max();
    cancel.clientId = domain::ClientId{std::numeric_limits<std::uint64_t>::max()};
    cancel.instrumentId = domain::InstrumentId{std::numeric_limits<std::uint64_t>::max()};
    cancel.targetOrderId = domain::TargetOrderId{std::numeric_limits<std::uint64_t>::max()};

    ASSERT_EQ(encodeCancelV1(cancel, frame), JournalCommandCodecError::NONE);
    auto decodedCancel = sentinelCancel();
    ASSERT_EQ(decodeCancelV1(frame, decodedCancel), JournalCommandCodecError::NONE);
    EXPECT_EQ(decodedCancel, cancel);

    newOrder.clientCommandId = domain::ClientCommandId{maximumClientCommandId};
    ASSERT_EQ(encodeNewOrderV1(newOrder, frame), JournalCommandCodecError::NONE);
    ASSERT_EQ(decodeNewOrderV1(frame, decodedNewOrder), JournalCommandCodecError::NONE);
    EXPECT_EQ(decodedNewOrder, newOrder);

    cancel.clientCommandId = domain::ClientCommandId{"Y"};
    ASSERT_EQ(encodeCancelV1(cancel, frame), JournalCommandCodecError::NONE);
    ASSERT_EQ(decodeCancelV1(frame, decodedCancel), JournalCommandCodecError::NONE);
    EXPECT_EQ(decodedCancel, cancel);
}

TEST(JournalCommandCodecTest, RejectsCommonEncodeValuesAndPreservesOutput) {
    auto newOrder = validNewOrder();
    newOrder.exchangeRunId = domain::ExchangeRunId{0};
    expectNewOrderEncodeErrorAndUnchanged(newOrder, JournalCommandCodecError::ZERO_EXCHANGE_RUN_ID);
    newOrder = validNewOrder();
    newOrder.commandSequence = domain::CommandSequence{0};
    expectNewOrderEncodeErrorAndUnchanged(newOrder, JournalCommandCodecError::ZERO_COMMAND_SEQUENCE);
    newOrder = validNewOrder();
    newOrder.behavioralRulesVersion = 2;
    expectNewOrderEncodeErrorAndUnchanged(newOrder, JournalCommandCodecError::UNSUPPORTED_BEHAVIORAL_RULES_VERSION);
    newOrder = validNewOrder();
    newOrder.configurationVersion = 0;
    expectNewOrderEncodeErrorAndUnchanged(newOrder, JournalCommandCodecError::ZERO_CONFIGURATION_VERSION);
    newOrder = validNewOrder();
    newOrder.clientId = domain::ClientId{0};
    expectNewOrderEncodeErrorAndUnchanged(newOrder, JournalCommandCodecError::ZERO_CLIENT_ID);
    newOrder = validNewOrder();
    newOrder.instrumentId = domain::InstrumentId{0};
    expectNewOrderEncodeErrorAndUnchanged(newOrder, JournalCommandCodecError::ZERO_INSTRUMENT_ID);

    auto cancel = validCancel();
    cancel.exchangeRunId = domain::ExchangeRunId{0};
    expectCancelEncodeErrorAndUnchanged(cancel, JournalCommandCodecError::ZERO_EXCHANGE_RUN_ID);
    cancel = validCancel();
    cancel.commandSequence = domain::CommandSequence{0};
    expectCancelEncodeErrorAndUnchanged(cancel, JournalCommandCodecError::ZERO_COMMAND_SEQUENCE);
    cancel = validCancel();
    cancel.behavioralRulesVersion = 2;
    expectCancelEncodeErrorAndUnchanged(cancel, JournalCommandCodecError::UNSUPPORTED_BEHAVIORAL_RULES_VERSION);
    cancel = validCancel();
    cancel.configurationVersion = 0;
    expectCancelEncodeErrorAndUnchanged(cancel, JournalCommandCodecError::ZERO_CONFIGURATION_VERSION);
    cancel = validCancel();
    cancel.clientId = domain::ClientId{0};
    expectCancelEncodeErrorAndUnchanged(cancel, JournalCommandCodecError::ZERO_CLIENT_ID);
    cancel = validCancel();
    cancel.instrumentId = domain::InstrumentId{0};
    expectCancelEncodeErrorAndUnchanged(cancel, JournalCommandCodecError::ZERO_INSTRUMENT_ID);
}

TEST(JournalCommandCodecTest, RejectsCommandSpecificEncodeValuesAndPreservesOutput) {
    auto newOrder = validNewOrder();
    newOrder.side = static_cast<domain::Side>(0);
    expectNewOrderEncodeErrorAndUnchanged(newOrder, JournalCommandCodecError::UNSUPPORTED_SIDE);
    newOrder = validNewOrder();
    newOrder.timeInForce = core::task::TimeInForce::DAY;
    expectNewOrderEncodeErrorAndUnchanged(newOrder, JournalCommandCodecError::UNSUPPORTED_TIME_IN_FORCE);
    newOrder = validNewOrder();
    newOrder.price = domain::Price{0};
    expectNewOrderEncodeErrorAndUnchanged(newOrder, JournalCommandCodecError::ZERO_PRICE);
    newOrder = validNewOrder();
    newOrder.quantity = domain::Quantity{0};
    expectNewOrderEncodeErrorAndUnchanged(newOrder, JournalCommandCodecError::ZERO_QUANTITY);

    auto cancel = validCancel();
    cancel.targetOrderId = domain::TargetOrderId{0};
    expectCancelEncodeErrorAndUnchanged(cancel, JournalCommandCodecError::ZERO_TARGET_ORDER_ID);
}

TEST(JournalCommandCodecTest, RejectsEveryEnvelopeFailureAndPreservesOutput) {
    const auto validNew = encodeValidNewOrder();

    expectNewOrderDecodeErrorAndUnchanged(std::span<const std::byte>(validNew).first(ENVELOPE_SIZE - 1),
                                          JournalCommandCodecError::TRUNCATED_INPUT);
    expectNewOrderDecodeErrorAndUnchanged(std::span<const std::byte>(validNew).first(validNew.size() - 1),
                                          JournalCommandCodecError::TRUNCATED_INPUT);

    auto oversized = validNew;
    oversized.resize(MAX_FRAME_SIZE + 1);
    expectNewOrderDecodeErrorAndUnchanged(oversized, JournalCommandCodecError::FRAME_TOO_LARGE);

    auto trailing = validNew;
    trailing.push_back(std::byte{0});
    expectNewOrderDecodeErrorAndUnchanged(trailing, JournalCommandCodecError::TRAILING_BYTES);

    auto magic = validNew;
    magic[0] = std::byte{'?'};
    expectNewOrderDecodeErrorAndUnchanged(magic, JournalCommandCodecError::INVALID_MAGIC);

    auto formatVersion = validNew;
    writeUint16LittleEndian(formatVersion, 4, 2);
    expectNewOrderDecodeErrorAndUnchanged(formatVersion, JournalCommandCodecError::UNSUPPORTED_FORMAT_VERSION);

    const auto validCancel = encodeValidCancel();
    expectNewOrderDecodeErrorAndUnchanged(validCancel, JournalCommandCodecError::UNSUPPORTED_RECORD_TYPE);
    expectCancelDecodeErrorAndUnchanged(validNew, JournalCommandCodecError::UNSUPPORTED_RECORD_TYPE);

    auto totalTooSmall = validNew;
    writeUint32LittleEndian(totalTooSmall, 8, ENVELOPE_SIZE - 1);
    expectNewOrderDecodeErrorAndUnchanged(totalTooSmall, JournalCommandCodecError::INVALID_TOTAL_LENGTH);

    auto oversizedTotal = validNew;
    writeUint32LittleEndian(oversizedTotal, 8, MAX_FRAME_SIZE + 1);
    expectNewOrderDecodeErrorAndUnchanged(oversizedTotal, JournalCommandCodecError::FRAME_TOO_LARGE);

    auto totalLargerThanInput = validNew;
    writeUint32LittleEndian(totalLargerThanInput, 8, static_cast<std::uint32_t>(validNew.size() + 1));
    expectNewOrderDecodeErrorAndUnchanged(totalLargerThanInput, JournalCommandCodecError::TRUNCATED_INPUT);

    auto totalSmallerThanInput = validNew;
    writeUint32LittleEndian(totalSmallerThanInput, 8, static_cast<std::uint32_t>(validNew.size() - 1));
    expectNewOrderDecodeErrorAndUnchanged(totalSmallerThanInput, JournalCommandCodecError::TRAILING_BYTES);

    auto runId = validNew;
    writeUint64LittleEndian(runId, 12, 0);
    rewriteChecksum(runId);
    expectNewOrderDecodeErrorAndUnchanged(runId, JournalCommandCodecError::ZERO_EXCHANGE_RUN_ID);

    auto commandSequence = validNew;
    writeUint64LittleEndian(commandSequence, 20, 0);
    rewriteChecksum(commandSequence);
    expectNewOrderDecodeErrorAndUnchanged(commandSequence, JournalCommandCodecError::ZERO_COMMAND_SEQUENCE);

    auto payloadLength = validNew;
    writeUint32LittleEndian(payloadLength, 28, static_cast<std::uint32_t>(validNew.size() - ENVELOPE_SIZE + 1));
    rewriteChecksum(payloadLength);
    expectNewOrderDecodeErrorAndUnchanged(payloadLength, JournalCommandCodecError::INVALID_PAYLOAD_LENGTH);

    auto checksum = validNew;
    checksum[CHECKSUM_OFFSET] ^= std::byte{0x01};
    expectNewOrderDecodeErrorAndUnchanged(checksum, JournalCommandCodecError::CHECKSUM_MISMATCH);
}

TEST(JournalCommandCodecTest, RejectsMalformedClientCommandIdsAndPayloadLengths) {
    const auto validNew = encodeValidNewOrder();

    auto zeroLength = validNew;
    zeroLength.erase(zeroLength.begin() + CLIENT_COMMAND_ID_OFFSET);
    zeroLength[CLIENT_COMMAND_ID_LENGTH_OFFSET] = std::byte{0};
    rewriteDeclaredLengths(zeroLength);
    rewriteChecksum(zeroLength);
    expectNewOrderDecodeErrorAndUnchanged(zeroLength, JournalCommandCodecError::INVALID_CLIENT_COMMAND_ID);

    const std::string maximumClientCommandId(domain::ClientCommandId::MAX_LENGTH, 'X');
    std::vector<std::byte> excessiveLength;
    ASSERT_EQ(encodeNewOrderV1(validNewOrder(maximumClientCommandId), excessiveLength), JournalCommandCodecError::NONE);
    excessiveLength.insert(excessiveLength.begin() + CLIENT_COMMAND_ID_OFFSET + domain::ClientCommandId::MAX_LENGTH,
                           std::byte{'X'});
    excessiveLength[CLIENT_COMMAND_ID_LENGTH_OFFSET] = std::byte{65};
    rewriteDeclaredLengths(excessiveLength);
    rewriteChecksum(excessiveLength);
    expectNewOrderDecodeErrorAndUnchanged(excessiveLength, JournalCommandCodecError::INVALID_CLIENT_COMMAND_ID);

    auto nonAscii = validNew;
    nonAscii[CLIENT_COMMAND_ID_OFFSET] = std::byte{0x80};
    rewriteChecksum(nonAscii);
    expectNewOrderDecodeErrorAndUnchanged(nonAscii, JournalCommandCodecError::INVALID_CLIENT_COMMAND_ID);

    auto inconsistentNewLength = validNew;
    inconsistentNewLength[CLIENT_COMMAND_ID_LENGTH_OFFSET] = std::byte{2};
    rewriteChecksum(inconsistentNewLength);
    expectNewOrderDecodeErrorAndUnchanged(inconsistentNewLength, JournalCommandCodecError::INVALID_PAYLOAD_LENGTH);

    auto validCancelFrame = encodeValidCancel();
    auto cancelZeroLength = validCancelFrame;
    cancelZeroLength.erase(cancelZeroLength.begin() + CLIENT_COMMAND_ID_OFFSET);
    cancelZeroLength[CLIENT_COMMAND_ID_LENGTH_OFFSET] = std::byte{0};
    rewriteDeclaredLengths(cancelZeroLength);
    rewriteChecksum(cancelZeroLength);
    expectCancelDecodeErrorAndUnchanged(cancelZeroLength, JournalCommandCodecError::INVALID_CLIENT_COMMAND_ID);

    std::vector<std::byte> cancelExcessiveLength;
    ASSERT_EQ(encodeCancelV1(validCancel(maximumClientCommandId), cancelExcessiveLength),
              JournalCommandCodecError::NONE);
    cancelExcessiveLength.insert(
        cancelExcessiveLength.begin() + CLIENT_COMMAND_ID_OFFSET + domain::ClientCommandId::MAX_LENGTH, std::byte{'X'});
    cancelExcessiveLength[CLIENT_COMMAND_ID_LENGTH_OFFSET] = std::byte{65};
    rewriteDeclaredLengths(cancelExcessiveLength);
    rewriteChecksum(cancelExcessiveLength);
    expectCancelDecodeErrorAndUnchanged(cancelExcessiveLength, JournalCommandCodecError::INVALID_CLIENT_COMMAND_ID);

    auto cancelNonAscii = validCancelFrame;
    cancelNonAscii[CLIENT_COMMAND_ID_OFFSET] = std::byte{0x80};
    rewriteChecksum(cancelNonAscii);
    expectCancelDecodeErrorAndUnchanged(cancelNonAscii, JournalCommandCodecError::INVALID_CLIENT_COMMAND_ID);

    auto inconsistentCancelLength = validCancelFrame;
    inconsistentCancelLength[CLIENT_COMMAND_ID_LENGTH_OFFSET] = std::byte{2};
    rewriteChecksum(inconsistentCancelLength);
    expectCancelDecodeErrorAndUnchanged(inconsistentCancelLength, JournalCommandCodecError::INVALID_PAYLOAD_LENGTH);

    writeUint32LittleEndian(validCancelFrame, 28, 1);
    rewriteChecksum(validCancelFrame);
    expectCancelDecodeErrorAndUnchanged(validCancelFrame, JournalCommandCodecError::INVALID_PAYLOAD_LENGTH);
}

TEST(JournalCommandCodecTest, RejectsEveryCommonDecodedValueAndPreservesOutput) {
    const auto validNew = encodeValidNewOrder();

    auto rulesVersion = validNew;
    writeUint32LittleEndian(rulesVersion, RULES_VERSION_OFFSET, 2);
    rewriteChecksum(rulesVersion);
    expectNewOrderDecodeErrorAndUnchanged(rulesVersion, JournalCommandCodecError::UNSUPPORTED_BEHAVIORAL_RULES_VERSION);

    auto configurationVersion = validNew;
    writeUint32LittleEndian(configurationVersion, CONFIGURATION_VERSION_OFFSET, 0);
    rewriteChecksum(configurationVersion);
    expectNewOrderDecodeErrorAndUnchanged(configurationVersion, JournalCommandCodecError::ZERO_CONFIGURATION_VERSION);

    auto clientId = validNew;
    writeUint64LittleEndian(clientId, CLIENT_ID_OFFSET, 0);
    rewriteChecksum(clientId);
    expectNewOrderDecodeErrorAndUnchanged(clientId, JournalCommandCodecError::ZERO_CLIENT_ID);

    auto instrumentId = validNew;
    writeUint64LittleEndian(instrumentId, INSTRUMENT_ID_OFFSET, 0);
    rewriteChecksum(instrumentId);
    expectNewOrderDecodeErrorAndUnchanged(instrumentId, JournalCommandCodecError::ZERO_INSTRUMENT_ID);

    auto cancelRules = encodeValidCancel();
    writeUint32LittleEndian(cancelRules, RULES_VERSION_OFFSET, 2);
    rewriteChecksum(cancelRules);
    expectCancelDecodeErrorAndUnchanged(cancelRules, JournalCommandCodecError::UNSUPPORTED_BEHAVIORAL_RULES_VERSION);
}

TEST(JournalCommandCodecTest, RejectsNewOrderWireCodesAndZeroValues) {
    const auto valid = encodeValidNewOrder();
    constexpr std::size_t SIDE_OFFSET = CLIENT_COMMAND_ID_OFFSET + 1;
    constexpr std::size_t TIME_IN_FORCE_OFFSET = SIDE_OFFSET + 1;
    constexpr std::size_t PRICE_OFFSET = TIME_IN_FORCE_OFFSET + 1;
    constexpr std::size_t QUANTITY_OFFSET = PRICE_OFFSET + 8;

    auto side = valid;
    side[SIDE_OFFSET] = std::byte{3};
    rewriteChecksum(side);
    expectNewOrderDecodeErrorAndUnchanged(side, JournalCommandCodecError::UNSUPPORTED_SIDE);

    auto timeInForce = valid;
    timeInForce[TIME_IN_FORCE_OFFSET] = std::byte{3};
    rewriteChecksum(timeInForce);
    expectNewOrderDecodeErrorAndUnchanged(timeInForce, JournalCommandCodecError::UNSUPPORTED_TIME_IN_FORCE);

    auto price = valid;
    writeUint64LittleEndian(price, PRICE_OFFSET, 0);
    rewriteChecksum(price);
    expectNewOrderDecodeErrorAndUnchanged(price, JournalCommandCodecError::ZERO_PRICE);

    auto quantity = valid;
    writeUint64LittleEndian(quantity, QUANTITY_OFFSET, 0);
    rewriteChecksum(quantity);
    expectNewOrderDecodeErrorAndUnchanged(quantity, JournalCommandCodecError::ZERO_QUANTITY);
}

TEST(JournalCommandCodecTest, RejectsZeroCancelTargetOrderId) {
    auto valid = encodeValidCancel();
    constexpr std::size_t TARGET_ORDER_ID_OFFSET = CLIENT_COMMAND_ID_OFFSET + 1;
    writeUint64LittleEndian(valid, TARGET_ORDER_ID_OFFSET, 0);
    rewriteChecksum(valid);
    expectCancelDecodeErrorAndUnchanged(valid, JournalCommandCodecError::ZERO_TARGET_ORDER_ID);
}

} // namespace
} // namespace exchange::storage
