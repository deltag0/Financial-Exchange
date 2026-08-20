#include "../include/sequencer.hpp"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

void exchange::sequencer::Sequencer::run() {
    std::cout << "[Sequencer] Thread started" << std::endl;
    while (true) {
        // Sequencer instance only has one shard in its mq_shards vector
        if (!mq_shards.empty() && !mq_shards[0]->empty()) {
            sequenceMessage message;

            if (mq_shards[0]->pop(message)) {
                std::cout << "[Sequencer] Processing message ID: " << message.id
                          << " on shard: " << (int)message.shard_id << " (Ticker: " << message.symbol << ")"
                          << std::endl;

                message.globalSequenceNumber = getNextGlobalSequenceNumber(message);
                message.orderId = domain::orderIdFrom(message.globalSequenceNumber);
                message.topicSequenceNumber = getNextTopicSequenceNumber(message);

                if (matchingEngineQueue && !matchingEngineQueue->push(message)) {
                    std::cerr << "[Sequencer] Failed to forward message ID: " << message.id << " to matching engine"
                              << std::endl;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

exchange::domain::CommandSequence exchange::sequencer::Sequencer::getNextGlobalSequenceNumber(
    const sequenceMessage& message) {
    return domain::CommandSequence{++globalSequenceNumber};
}

uint64_t exchange::sequencer::Sequencer::getNextTopicSequenceNumber(const sequenceMessage& message) {
    topicData& data = topicSequence[message.port];
    data.lastSenderPort = message.port;
    return ++data.sequenceNumber;
}

void exchange::sequencer::Sequencer::send(sequenceMessage&) {}
