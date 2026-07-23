#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "../../core/task/include/task.hpp"
#include "sequence_message.hpp"

namespace exchange {
namespace sequencer {

#define ANY_TICKER ""
#define MAX_CLIENT_PORTS 65536

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