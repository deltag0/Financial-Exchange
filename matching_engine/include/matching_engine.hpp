#pragma once

#include "../../bus/include/bus.hpp"
#include "../../core/shared_queue/include/shared_queue.hpp"
#include "../../core/task/include/task.hpp"
#include "../../core/instrument/include/instrument_config.hpp"
#include "../../sequencer/include/sequencer.hpp"
#include <cstdint>
#include <map>
#include <queue>
#include <set>

namespace exchange::matching_engine {

class MatchingEngine : public exchange::core::task::Task<sequencer::sequenceMessage> {
public:
    MatchingEngine(core::SharedQueue<sequencer::sequenceMessage>* sequencerQueue, core::Bus& multicastBus);

    void run() override;
    void send(sequencer::sequenceMessage& message) override;

protected:
    enum class ProcessingResult {
        APPLIED,
        BOOK_CAPACITY_EXCEEDED,
    };

    struct PriceLevel {
        uint64_t totalQuantity = 0;
        std::queue<sequencer::sequenceMessage> orders;
    };

    void drainQueues(const char* source);
    void drainQueue(core::SharedQueue<sequencer::sequenceMessage>& queue, const char* source, std::size_t index = 0);

    ProcessingResult processMessage(sequencer::sequenceMessage& message);

    /*
    Match incoming order against existing orders.

    Must guarantee that message type is BUY before calling.
    */
    ProcessingResult processBuyOrder(sequencer::sequenceMessage& message);

    ProcessingResult processSellOrder(sequencer::sequenceMessage& message);

    void matchBuyOrder(sequencer::sequenceMessage& message);

    void matchSellOrder(sequencer::sequenceMessage& message);

    /*
    Returns whether the current sell book can fully fill an incoming buy order.

    Invariants:
    - Caller provides a BUY order.
    - This method must not mutate resting orders or the incoming order.
    */
    bool canFullyFillBuyOrder(const sequencer::sequenceMessage& message) const;

    uint64_t calculateBuyRemainder(const sequencer::sequenceMessage& message) const;

    bool canAddOrder(const sequencer::sequenceMessage& message, uint64_t quantity) const;

    bool cleanBook(const sequencer::sequenceMessage& message);

    /*
    Remove first order from the order book.
    */
    void removeOrder(const sequencer::sequenceMessage& message,
                     std::queue<sequencer::sequenceMessage>* orderQueue = nullptr);

    /*
    Check if order has expired based on current time and order expiry time.
    */
    bool checkOrderExpiry(const sequencer::sequenceMessage& message);

    ProcessingResult addOrder(const sequencer::sequenceMessage& message);

    core::SharedQueue<sequencer::sequenceMessage>& sequencerQueue;

    // Re-transmission bus for data to ports
    core::Bus& multicastBus;

    std::unordered_map<std::string, std::map<uint64_t, PriceLevel, std::greater<uint64_t>>> buyOrders;

    std::unordered_map<std::string, std::map<uint64_t, PriceLevel>> sellOrders;

    std::set<uint64_t> orderIds;
};

} // namespace exchange::matching_engine
