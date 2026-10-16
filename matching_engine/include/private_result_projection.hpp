#pragma once

#include "command_result.hpp"

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace exchange::private_result {

enum class TradeRole : std::uint8_t {
    MAKER,
    TAKER,
};

struct PrivateTrade final {
    const domain::EventId eventId;
    const domain::InstrumentId instrumentId;
    const domain::OrderId orderId;
    const domain::Side side;
    const TradeRole role;
    const domain::Price executionPrice;
    const domain::Quantity executionQuantity;
    const domain::Quantity remainingQuantity;

    bool operator==(const PrivateTrade&) const = default;
};

using PrivateEvent = std::variant<PrivateTrade, domain::OrderRested, domain::OrderCancelled, domain::CommandRejected>;
using PrivateResult = std::vector<PrivateEvent>;

struct RecipientResult final {
    domain::ClientId recipient;
    std::optional<domain::CommandResultCorrelation> correlation;
    PrivateResult privateResult;

    bool operator==(const RecipientResult&) const = default;
};

using RecipientResults = std::vector<RecipientResult>;

[[nodiscard]] std::optional<RecipientResult> projectPrivateResult(
    const matching_engine::CommandResultBatch& authoritativeResult, domain::ClientId recipient);
[[nodiscard]] RecipientResults projectPrivateResults(const matching_engine::CommandResultBatch& authoritativeResult);

} // namespace exchange::private_result
