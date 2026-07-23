#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

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

id: client-generated unique ID assigned to order
globalSequenceNumber: unique ID assigned by the sequencer for each ticker globally
topicSequenceNumber: unique ID assigned by the sequencer for each ticker and client port combination
timestamp: timestamp of the order, nanoseconds since epoch
order: Order assigned by received by the server
port: port of the client
price: price of the order
quantity: quantity of the order
symbol: symbol of the order
type: type of the order
expiry: expiry time of the order
shard_id: shard ID assigned by the sequencer for the order
tif: time in force of the order
*/
struct sequenceMessage {
    uint64_t id;
    uint64_t globalSequenceNumber;
    uint64_t topicSequenceNumber;
    uint64_t timestamp;
    uint64_t order;
    uint64_t port;
    uint64_t topic;
    uint64_t price;
    uint64_t quantity;
    char symbol[10];
    orderType type;
    std::chrono::system_clock::time_point expiry;
    uint8_t shard_id;
    core::task::TimeInForce tif;

    // Serialize the message to a compact JSON object. Enums are emitted as their
    // integer underlying values so the journal round-trips losslessly. `symbol` is a
    // fixed 10-byte buffer that may not be null-terminated, so we bound its length.
    std::string toJson() const {
        std::size_t symbolLen = 0;
        while (symbolLen < sizeof(symbol) && symbol[symbolLen] != '\0') {
            ++symbolLen;
        }

        const auto expiryNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(expiry.time_since_epoch()).count();

        std::string json;
        json.reserve(256);
        json += '{';
        json += "\"id\":" + std::to_string(id);
        json += ",\"globalSequenceNumber\":" + std::to_string(globalSequenceNumber);
        json += ",\"topicSequenceNumber\":" + std::to_string(topicSequenceNumber);
        json += ",\"timestamp\":" + std::to_string(timestamp);
        json += ",\"order\":" + std::to_string(order);
        json += ",\"port\":" + std::to_string(port);
        json += ",\"topic\":" + std::to_string(topic);
        json += ",\"price\":" + std::to_string(price);
        json += ",\"quantity\":" + std::to_string(quantity);
        json += ",\"symbol\":\"" + std::string(symbol, symbolLen) + "\"";
        json += ",\"type\":" + std::to_string(static_cast<int>(type));
        json += ",\"expiry\":" + std::to_string(expiryNs);
        json += ",\"shardId\":" + std::to_string(static_cast<int>(shard_id));
        // tif is a FIX char-valued enum ('0','1',...); emit the character, not its code.
        json += ",\"tif\":\"";
        json += static_cast<char>(tif);
        json += "\"";
        json += '}';
        return json;
    }
};

} // namespace sequencer
} // namespace exchange
