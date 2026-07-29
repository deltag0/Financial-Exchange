#pragma once

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

#include "../../core/task/include/time_in_force.hpp"

namespace exchange {
namespace sequencer {

namespace detail {

// Returns the raw JSON value for `key` (numeric token or unquoted string contents).
inline std::string_view jsonValue(std::string_view json, std::string_view key) {
    const std::string needle = std::string("\"") + std::string(key) + "\":";
    const std::size_t pos = json.find(needle);
    if (pos == std::string_view::npos) {
        throw std::invalid_argument("Missing JSON key: " + std::string(key));
    }

    std::size_t index = pos + needle.size();
    while (index < json.size() && std::isspace(static_cast<unsigned char>(json[index]))) {
        ++index;
    }
    if (index >= json.size()) {
        throw std::invalid_argument("Missing value for key: " + std::string(key));
    }

    if (json[index] == '"') {
        ++index;
        const std::size_t start = index;
        while (index < json.size() && json[index] != '"') {
            ++index;
        }
        if (index >= json.size()) {
            throw std::invalid_argument("Unterminated string for key: " + std::string(key));
        }
        return json.substr(start, index - start);
    }

    const std::size_t start = index;
    while (index < json.size() && json[index] != ',' && json[index] != '}') {
        ++index;
    }
    std::string_view value = json.substr(start, index - start);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    if (value.empty()) {
        throw std::invalid_argument("Empty value for key: " + std::string(key));
    }
    return value;
}

} // namespace detail

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

    static sequenceMessage jsonToSequenceMessage(std::string loggesMesssage) {
        const std::size_t brace = loggesMesssage.find('{');
        if (brace == std::string::npos) {
            throw std::invalid_argument("JSON object not found in logged message");
        }
        const std::string_view json = loggesMesssage;
        const std::string_view payload = json.substr(brace);

        auto u64 = [&](std::string_view key) {
            return std::stoull(std::string(detail::jsonValue(payload, key)));
        };

        sequenceMessage message{};
        message.id = u64("id");
        message.globalSequenceNumber = u64("globalSequenceNumber");
        message.topicSequenceNumber = u64("topicSequenceNumber");
        message.timestamp = u64("timestamp");
        message.order = u64("order");
        message.port = u64("port");
        message.topic = u64("topic");
        message.price = u64("price");
        message.quantity = u64("quantity");

        const std::string symbol = std::string(detail::jsonValue(payload, "symbol"));
        std::memset(message.symbol, 0, sizeof(message.symbol));
        std::memcpy(message.symbol, symbol.data(),
                    std::min(symbol.size(), sizeof(message.symbol) - 1));

        message.type = static_cast<orderType>(u64("type"));

        const int64_t expiryNs =
            std::stoll(std::string(detail::jsonValue(payload, "expiry")));
        message.expiry = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            std::chrono::system_clock::time_point{} + std::chrono::nanoseconds(expiryNs));

        message.shard_id = static_cast<uint8_t>(u64("shardId"));

        const std::string_view tif = detail::jsonValue(payload, "tif");
        if (tif.empty()) {
            throw std::invalid_argument("Empty tif value");
        }
        message.tif = static_cast<core::task::TimeInForce>(tif.front());

        return message;
    }
};

} // namespace sequencer
} // namespace exchange
