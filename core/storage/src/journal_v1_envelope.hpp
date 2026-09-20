#pragma once

#include "crc32c.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace exchange::storage {

enum class JournalCommandCodecError : std::uint8_t;

namespace detail {

inline constexpr std::array<std::byte, 4> JOURNAL_V1_MAGIC{
    std::byte{'F'},
    std::byte{'X'},
    std::byte{'J'},
    std::byte{'R'},
};
inline constexpr std::uint16_t JOURNAL_V1_FORMAT_VERSION = 1;
inline constexpr std::size_t JOURNAL_V1_FORMAT_VERSION_OFFSET = 4;
inline constexpr std::size_t JOURNAL_V1_RECORD_TYPE_OFFSET = 6;
inline constexpr std::size_t JOURNAL_V1_TOTAL_LENGTH_OFFSET = 8;
inline constexpr std::size_t JOURNAL_V1_EXCHANGE_RUN_ID_OFFSET = 12;
inline constexpr std::size_t JOURNAL_V1_COMMAND_SEQUENCE_OFFSET = 20;
inline constexpr std::size_t JOURNAL_V1_PAYLOAD_LENGTH_OFFSET = 28;
inline constexpr std::size_t JOURNAL_V1_CHECKSUM_OFFSET = 32;
inline constexpr std::size_t JOURNAL_V1_ENVELOPE_SIZE = 36;
inline constexpr std::size_t JOURNAL_V1_PAYLOAD_OFFSET = JOURNAL_V1_ENVELOPE_SIZE;
inline constexpr std::uint16_t JOURNAL_V1_NEW_ORDER_RECORD_TYPE = 2;
inline constexpr std::uint16_t JOURNAL_V1_CANCEL_RECORD_TYPE = 3;
inline constexpr std::size_t JOURNAL_V1_COMMAND_MAX_FRAME_SIZE = 256;
inline constexpr std::uint32_t JOURNAL_V1_NEW_ORDER_PAYLOAD_SIZE_WITHOUT_CLIENT_COMMAND_ID = 43;
inline constexpr std::uint32_t JOURNAL_V1_CANCEL_PAYLOAD_SIZE_WITHOUT_CLIENT_COMMAND_ID = 33;
inline constexpr std::uint32_t JOURNAL_V1_CLIENT_COMMAND_ID_MIN_LENGTH = 1;
inline constexpr std::uint32_t JOURNAL_V1_CLIENT_COMMAND_ID_MAX_LENGTH = 64;

struct JournalV1CommandEnvelope final {
    std::uint16_t recordType{0};
    std::uint32_t totalLength{0};
    std::uint64_t exchangeRunId{0};
    std::uint64_t commandSequence{0};
    std::uint32_t payloadLength{0};
};

[[nodiscard]] JournalCommandCodecError validateJournalV1CommandEnvelope(std::span<const std::byte> input,
                                                                        std::uint16_t expectedRecordType,
                                                                        bool requireCompleteFrame,
                                                                        JournalV1CommandEnvelope &output) noexcept;

inline std::uint32_t journalV1Checksum(const std::span<const std::byte> frame) noexcept {
    Crc32cAccumulator accumulator;
    accumulator.update(frame.first(JOURNAL_V1_CHECKSUM_OFFSET));
    accumulator.update(frame.subspan(JOURNAL_V1_PAYLOAD_OFFSET));
    return accumulator.checksum();
}

} // namespace detail
} // namespace exchange::storage
