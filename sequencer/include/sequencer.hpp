#pragma once
#include <atomic>
#include <cstdint>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "../../core/task/include/task.hpp"

namespace exchange {
namespace sequencer {

#define ANY_TICKER ""
#define MAX_CLIENT_PORTS 65536

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
    uint8_t shard_id;
    core::task::TimeInForce tif;
};

/*
globalSequenceNumber: sequence number for each ticker globally across all clients
topicSequenceNumbers: most recent sequence number for each ticker and client port combination
*/
struct topicData {
    uint64_t lastSenderPort;
    uint64_t sequenceNumber;
};

class Sequencer : public exchange::core::task::Task<sequenceMessage> {
  public:
    Sequencer(std::vector<core::SharedQueue<sequenceMessage> *> mq_shards,
              core::SharedQueue<sequenceMessage> *matchingEngineQueue = nullptr)
        : Task{std::move(mq_shards)}, matchingEngineQueue(matchingEngineQueue) {}

    void run() override;
    void send(sequenceMessage &message) override;
    uint64_t getNextGlobalSequenceNumber(const sequenceMessage &message);
    uint64_t getNextTopicSequenceNumber(const sequenceMessage &message);

  private:
    void convertToSequenceMessage() {}

  private:
    // Global sequence number
    uint64_t globalSequenceNumber = 0;

    std::unordered_map<uint64_t, topicData> topicSequence;
    core::SharedQueue<sequenceMessage> *matchingEngineQueue = nullptr;
};

} // namespace sequencer
} // namespace exchange