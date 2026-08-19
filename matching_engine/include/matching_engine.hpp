#pragma once

#include "../../bus/include/bus.hpp"
#include "../../core/domain/include/business_events.hpp"
#include "../../core/instrument/include/instrument_config.hpp"
#include "../../core/shared_queue/include/shared_queue.hpp"
#include "../../core/task/include/task.hpp"
#include "../../sequencer/include/sequencer.hpp"

#include <cstdint>
#include <map>
#include <queue>
#include <set>
#include <vector>

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

    struct ProcessingOutcome {
        ProcessingResult result;
        std::vector<domain::BusinessEvent> events;
    };

    struct PriceLevel {
        domain::Quantity totalQuantity{};
        std::queue<sequencer::sequenceMessage> orders;
    };

    void drainQueues(const char* source);
    void drainQueue(core::SharedQueue<sequencer::sequenceMessage>& queue, const char* source, std::size_t index = 0);

    ProcessingOutcome processMessage(sequencer::sequenceMessage& message);

    /*
    Match incoming order against existing orders.

    Must guarantee that message type is BUY before calling.
    */
    ProcessingOutcome processBuyOrder(sequencer::sequenceMessage& message);

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

    domain::Quantity calculateBuyRemainder(const sequencer::sequenceMessage& message) const;

    bool canAddOrder(const sequencer::sequenceMessage& message, domain::Quantity quantity) const;

    ProcessingOutcome rejectBookCapacity(const sequencer::sequenceMessage& message) const;

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

    std::unordered_map<std::string, std::map<domain::Price, PriceLevel, std::greater<domain::Price>>> buyOrders;

    std::unordered_map<std::string, std::map<domain::Price, PriceLevel>> sellOrders;

    std::set<domain::OrderId> orderIds;
};

} // namespace exchange::matching_engine
