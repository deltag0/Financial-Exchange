#pragma once

#include "business_events.hpp"
#include "domain_types.hpp"
#include "time_in_force.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace exchange::storage {

struct JournalNewOrderV1 final {
    domain::ExchangeRunId exchangeRunId;
    domain::CommandSequence commandSequence;
    std::uint32_t behavioralRulesVersion;
    std::uint32_t configurationVersion;
    domain::ClientId clientId;
    domain::InstrumentId instrumentId;
    domain::ClientCommandId clientCommandId;
    domain::Side side;
    core::task::TimeInForce timeInForce;
    domain::Price price;
    domain::Quantity quantity;

    bool operator==(const JournalNewOrderV1&) const = default;
};

struct JournalCancelV1 final {
    domain::ExchangeRunId exchangeRunId;
    domain::CommandSequence commandSequence;
    std::uint32_t behavioralRulesVersion;
    std::uint32_t configurationVersion;
    domain::ClientId clientId;
    domain::InstrumentId instrumentId;
    domain::ClientCommandId clientCommandId;
    domain::TargetOrderId targetOrderId;

    bool operator==(const JournalCancelV1&) const = default;
};

enum class JournalCommandCodecError : std::uint8_t {
    NONE,
    TRUNCATED_INPUT,
    FRAME_TOO_LARGE,
    TRAILING_BYTES,
    INVALID_MAGIC,
    UNSUPPORTED_FORMAT_VERSION,
    UNSUPPORTED_RECORD_TYPE,
    INVALID_TOTAL_LENGTH,
    ZERO_EXCHANGE_RUN_ID,
    ZERO_COMMAND_SEQUENCE,
    INVALID_PAYLOAD_LENGTH,
    CHECKSUM_MISMATCH,
    UNSUPPORTED_BEHAVIORAL_RULES_VERSION,
    ZERO_CONFIGURATION_VERSION,
    ZERO_CLIENT_ID,
    ZERO_INSTRUMENT_ID,
    INVALID_CLIENT_COMMAND_ID,
    UNSUPPORTED_SIDE,
    UNSUPPORTED_TIME_IN_FORCE,
    ZERO_PRICE,
    ZERO_QUANTITY,
    ZERO_TARGET_ORDER_ID,
    ALLOCATION_FAILURE,
};

[[nodiscard]] JournalCommandCodecError encodeNewOrderV1(const JournalNewOrderV1& command,
                                                        std::vector<std::byte>& output) noexcept;

[[nodiscard]] JournalCommandCodecError decodeNewOrderV1(std::span<const std::byte> input,
                                                        JournalNewOrderV1& output) noexcept;

[[nodiscard]] JournalCommandCodecError encodeCancelV1(const JournalCancelV1& command,
                                                      std::vector<std::byte>& output) noexcept;

[[nodiscard]] JournalCommandCodecError decodeCancelV1(std::span<const std::byte> input,
                                                      JournalCancelV1& output) noexcept;

} // namespace exchange::storage
