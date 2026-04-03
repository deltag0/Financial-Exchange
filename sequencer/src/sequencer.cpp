#include "../include/sequencer.hpp"
#include <cstdint>
#include <iostream>
#include <chrono>
#include <thread>

void exchange::sequencer::Sequencer::run() {
    std::cout << "[Sequencer] Thread started" << std::endl;
    while (true) {
        // Sequencer instance only has one shard in its mq_shards vector
        if (!mq_shards.empty() && !mq_shards[0]->empty()) {
            sequenceMessage message;

            if (mq_shards[0]->pop(message)) {
                std::cout << "[Sequencer] Processing message ID: " << message.id 
                          << " on shard: " << (int)message.shard_id 
                          << " (Ticker: " << message.symbol << ")" << std::endl;

                message.sequence_number = getNextSequenceNumber(message.port);
                // Further message processing would go here
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

uint64_t exchange::sequencer::Sequencer::getNextSequenceNumber(uint64_t port) {
    return sequenceNumbers[port]++;
}

void exchange::sequencer::Sequencer::send() {}
