#pragma once

#include "business_events.hpp"

#include <vector>

namespace exchange::matching_engine {
class CommandResultBatch;
}

namespace exchange::market_data {

struct PublicTrade final {
    const domain::EventId eventId;
    const domain::InstrumentId instrumentId;
    const domain::Price executionPrice;
    const domain::Quantity executionQuantity;
    const domain::Side aggressorSide;

    bool operator==(const PublicTrade&) const = default;
};

[[nodiscard]] std::vector<PublicTrade> projectPublicTrades(
    const matching_engine::CommandResultBatch& authoritativeResult);

} // namespace exchange::market_data
