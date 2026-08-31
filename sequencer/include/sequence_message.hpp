#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "../../core/domain/include/domain_types.hpp"
#include "../../core/task/include/time_in_force.hpp"

namespace exchange {
namespace sequencer {

enum class orderType : uint8_t {
    BUY = 1,
    SELL = 2,
    CANCEL = 3,
    CANCELREJ = 4,
};

/*
Message format transmitted to the matching engine

targetOrderId: authoritative exchange OrderId targeted by a normalized cancel
globalSequenceNumber: checked process-local sequence assigned by the sole sequencer
price: price of the order
quantity: quantity of the order
symbol: symbol of the order
type: type of the order
expiry: expiry time of the order
shard_id: reserved non-authoritative partition metadata; the FIX adapter does not route on it
tif: time in force of the order
*/
struct sequenceMessage {
    domain::OrderId orderId;
    std::optional<domain::TargetOrderId> targetOrderId;
    domain::CommandSequence globalSequenceNumber;
    domain::ClientId clientId;
    std::optional<domain::ClientCommandId> clientCommandId;
    domain::InstrumentId instrumentId;
    uint64_t configurationVersion;
    domain::Price price;
    domain::Quantity quantity;
    char symbol[10];
    orderType type;
    std::chrono::system_clock::time_point expiry;
    uint8_t shard_id;
    core::task::TimeInForce tif;
};

} // namespace sequencer
} // namespace exchange
