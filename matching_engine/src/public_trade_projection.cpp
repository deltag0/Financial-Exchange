#include "public_trade_projection.hpp"

#include <cstddef>
#include <variant>

#include "command_result.hpp"

namespace exchange::market_data {

std::vector<PublicTrade> projectPublicTrades(const matching_engine::CommandResultBatch& authoritativeResult) {
    std::size_t tradeCount = 0;
    for (const auto& event : authoritativeResult.events()) {
        if (std::holds_alternative<domain::Trade>(event)) {
            ++tradeCount;
        }
    }

    std::vector<PublicTrade> projected;
    projected.reserve(tradeCount);
    for (const auto& event : authoritativeResult.events()) {
        const auto* trade = std::get_if<domain::Trade>(&event);
        if (trade != nullptr) {
            projected.push_back({
                .eventId = trade->eventId,
                .instrumentId = trade->instrumentId,
                .executionPrice = trade->executionPrice,
                .executionQuantity = trade->executionQuantity,
                .aggressorSide = trade->takerSide,
            });
        }
    }
    return projected;
}

} // namespace exchange::market_data
