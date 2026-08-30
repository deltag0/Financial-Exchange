#pragma once

#include "command_result_queue.hpp"

#include "../../bus/include/bus.hpp"
#include "../../core/domain/include/business_events.hpp"
#include "../../core/instrument/include/instrument_config.hpp"
#include "../../core/shared_queue/include/shared_queue.hpp"
#include "../../core/task/include/task.hpp"
#include "../../sequencer/include/sequencer.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <vector>

namespace exchange::matching_engine {

class MatchingEngine : public exchange::core::task::Task<sequencer::sequenceMessage> {
public:
    MatchingEngine(core::SharedQueue<sequencer::sequenceMessage>* sequencerQueue, core::Bus& multicastBus,
                   BoundedCommandResultQueue& commandResultQueue);

    void run() override;
    void send(sequencer::sequenceMessage& message) override;

protected:
    MatchingEngine(core::SharedQueue<sequencer::sequenceMessage>* sequencerQueue, core::Bus& multicastBus,
                   std::size_t ownedResultQueueCapacity);

    using ProcessingResult = exchange::matching_engine::ProcessingResult;

    struct ProcessingOutcome {
        ProcessingResult result;
        std::vector<domain::BusinessEvent> events;
    };

    struct MatchPlan {
        domain::Quantity remainder{};
        std::size_t executionCount{};
    };

    struct OrderNode {
        domain::OrderId orderId{};
        domain::ClientId owner{};
        domain::Quantity remainingQuantity{};
    };

    using OrderList = std::list<OrderNode>;

    struct PriceLevel {
        domain::Quantity totalQuantity{};
        OrderList orders;
    };

    using BidBook = std::map<domain::Price, PriceLevel, std::greater<domain::Price>>;
    using AskBook = std::map<domain::Price, PriceLevel>;

    struct InstrumentBook {
        BidBook bids;
        AskBook asks;
    };

    struct ActiveOrder {
        domain::ClientId owner{};
        domain::InstrumentId instrumentId{};
        sequencer::orderType side{};
        domain::Price price{};
        domain::Quantity remainingQuantity{};
        OrderList::iterator orderLocation;
    };

    bool drainQueue(core::SharedQueue<sequencer::sequenceMessage>& queue, const char* source, std::size_t index = 0);

    ProcessingOutcome processMessage(const sequencer::sequenceMessage& message);

    // Sequenced command data remains immutable; matching operates on a local remainder.
    ProcessingOutcome processBuyOrder(const sequencer::sequenceMessage& message);

    ProcessingOutcome processSellOrder(const sequencer::sequenceMessage& message);

    ProcessingOutcome processCancel(const sequencer::sequenceMessage& message);

    ProcessingOutcome processOrder(const sequencer::sequenceMessage& message, bool restRemainder);

    void matchOrder(const sequencer::sequenceMessage& message, domain::Quantity& remaining,
                    std::vector<domain::BusinessEvent>& events);

    void matchBuyOrder(const sequencer::sequenceMessage& message, domain::Quantity& remaining,
                       std::vector<domain::BusinessEvent>& events);

    void matchSellOrder(const sequencer::sequenceMessage& message, domain::Quantity& remaining,
                        std::vector<domain::BusinessEvent>& events);

    void executeTrade(const sequencer::sequenceMessage& message, sequencer::orderType makerSide, domain::Side takerSide,
                      domain::Price executionPrice, PriceLevel& priceLevel, domain::Quantity& remaining,
                      std::vector<domain::BusinessEvent>& events);

    MatchPlan planMatches(const sequencer::sequenceMessage& message) const;

    bool canAddOrder(const sequencer::sequenceMessage& message, domain::Quantity quantity) const;

    ProcessingOutcome rejectBookCapacity(const sequencer::sequenceMessage& message) const;

    ProcessingOutcome rejectCancel(const sequencer::sequenceMessage& message, domain::OrderId targetOrderId,
                                   domain::CommandRejectionReason reason) const;

    void removeFilledOrder(std::map<domain::OrderId, ActiveOrder>::iterator activeOrder);

    void removeCancelledOrder(std::map<domain::OrderId, ActiveOrder>::iterator activeOrder);

    ProcessingResult addOrder(const sequencer::sequenceMessage& message, domain::Quantity remainingQuantity);

    bool handoffPendingResult();

    core::SharedQueue<sequencer::sequenceMessage>& sequencerQueue;

    // Re-transmission bus for data to ports
    core::Bus& multicastBus;

    // Matching owns retry state; the event-stream boundary owns accepted immutable batches.
    std::unique_ptr<BoundedCommandResultQueue> ownedCommandResultQueue;
    BoundedCommandResultQueue& commandResultQueue;
    ImmutableCommandResultBatch pendingCommandResult;

    std::map<domain::InstrumentId, InstrumentBook> orderBooks;

    std::map<domain::OrderId, ActiveOrder> activeOrders;
};

} // namespace exchange::matching_engine
