#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "../../core/task/include/task.hpp"

namespace exchange {
namespace sequencer {

#define ANY_TICKER ""

enum class orderType : uint8_t {
    BUY = 1,
    SELL = 2,
    CANCEL = 3,
    CANCELREJ = 4,
};

/*
Message format transmitted to the matching engine

id: client-generated unique ID assigned to order
sequence_number: unique ID assigned by the sequencer
timestamp: timestamp of the order, nanoseconds since epoch
order: Order assigned by received by the server
port: port of the client
price: price of the order
quantity: quantity of the order
symbol: symbol of the order
type: type of the order
*/
struct sequenceMessage {
    uint64_t id;
    uint64_t sequence_number;
    uint64_t timestamp;
    uint64_t order;
    uint64_t port;
    uint64_t price;
    uint64_t quantity;
    char symbol[10];
    orderType type;
    uint8_t shard_id;
};

class Sequencer : public exchange::core::task::Task<sequenceMessage> {
public:
    Sequencer(std::vector<core::SharedQueue<sequenceMessage>*> mq_shards)
        : Task{mq_shards} {}

    void run() override;
    void send() override;
    uint64_t getNextSequenceNumber(uint64_t port);

private:
    void convertToSequenceMessage() {}

private:
    std::unordered_map<uint64_t, uint64_t> sequenceNumbers;
};

} // namespace sequencer
} // namespace exchange