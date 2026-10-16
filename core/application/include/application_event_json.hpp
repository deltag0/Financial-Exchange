#pragma once

#include "private_result_projection.hpp"
#include "public_trade_projection.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace exchange::application_event {

enum class ApplicationEventJsonError : std::uint8_t {
    NONE,
    EMPTY_EVENTS,
    RUN_MISMATCH,
    COMMAND_SEQUENCE_MISMATCH,
    CORRELATION_MISMATCH,
    RECIPIENT_MISMATCH,
    INVALID_ENUM,
};

[[nodiscard]] ApplicationEventJsonError serializePublicTradesV1(domain::ExchangeRunId exchangeRunId,
                                                                const std::vector<market_data::PublicTrade>& trades,
                                                                std::string& output);

[[nodiscard]] ApplicationEventJsonError serializePrivateResultV1(domain::ExchangeRunId exchangeRunId,
                                                                 const private_result::RecipientResult& result,
                                                                 std::string& output);

} // namespace exchange::application_event
