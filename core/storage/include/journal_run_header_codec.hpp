#pragma once

#include "domain_types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace exchange::storage {

struct RunHeaderInstrumentV1 final {
    domain::InstrumentId instrumentId{};
    std::uint32_t configurationVersion{0};

    bool operator==(const RunHeaderInstrumentV1&) const = default;
};

struct RunHeaderV1 final {
    domain::ExchangeRunId exchangeRunId{};
    std::uint32_t behavioralRulesVersion{0};
    std::uint32_t maxEventsPerCommand{0};
    std::uint64_t maxRunCommands{0};
    std::uint64_t maxRunJournalBytes{0};
    std::vector<RunHeaderInstrumentV1> instruments{};

    bool operator==(const RunHeaderV1&) const = default;
};

enum class RunHeaderCodecError : std::uint8_t {
    NONE,
    TRUNCATED_INPUT,
    FRAME_TOO_LARGE,
    TRAILING_BYTES,
    INVALID_MAGIC,
    UNSUPPORTED_FORMAT_VERSION,
    UNSUPPORTED_RECORD_TYPE,
    INVALID_TOTAL_LENGTH,
    ZERO_EXCHANGE_RUN_ID,
    NONZERO_COMMAND_SEQUENCE,
    INVALID_PAYLOAD_LENGTH,
    CHECKSUM_MISMATCH,
    UNSUPPORTED_BEHAVIORAL_RULES_VERSION,
    UNSUPPORTED_MAX_EVENTS_PER_COMMAND,
    ZERO_MAX_RUN_COMMANDS,
    MAX_RUN_JOURNAL_BYTES_TOO_SMALL,
    ZERO_INSTRUMENT_ID,
    ZERO_CONFIGURATION_VERSION,
    INSTRUMENTS_NOT_STRICTLY_INCREASING,
    UNSUPPORTED_GENERATOR_CONFIGURATION,
    ALLOCATION_FAILURE,
};

[[nodiscard]] RunHeaderCodecError encodeRunHeaderV1(const RunHeaderV1& header, std::vector<std::byte>& output) noexcept;

[[nodiscard]] RunHeaderCodecError decodeRunHeaderV1(std::span<const std::byte> input, RunHeaderV1& output) noexcept;

} // namespace exchange::storage
