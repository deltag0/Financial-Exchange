#include "../include/matching_engine.hpp"

#include "../../core/task/include/task.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/types.h>
#include <thread>
#include <utility>

namespace exchange::matching_engine {

namespace {

constexpr std::size_t MAX_EVENTS_PER_COMMAND = 4'096;

std::size_t checkedAddEventCount(const std::size_t currentCount, const std::size_t additionalCount) {
    if (additionalCount > std::numeric_limits<std::size_t>::max() - currentCount) {
        throw std::logic_error("business-event count overflow during matching plan");
    }
    return currentCount + additionalCount;
}

domain::EventId makeEventId(const domain::CommandSequence commandSequence, const std::size_t eventIndex) {
    if (eventIndex > std::numeric_limits<domain::EventIndex::Underlying>::max()) {
        throw std::logic_error("EventIndex exhausted during one command");
    }
    return domain::EventId{
        .commandSequence = commandSequence,
        .eventIndex = domain::EventIndex{static_cast<domain::EventIndex::Underlying>(eventIndex)},
    };
}

domain::Side toDomainSide(const sequencer::orderType side) {
    if (side == sequencer::orderType::BUY) {
        return domain::Side::BUY;
    }
    if (side == sequencer::orderType::SELL) {
        return domain::Side::SELL;
    }
    throw std::logic_error("non-order side reached matching engine");
}

domain::CommandResultCorrelation resultCorrelationFrom(const sequencer::sequenceMessage& message) {
    if (message.clientId.value() == 0 || !message.clientCommandId.has_value() ||
        message.globalSequenceNumber.value() == 0) {
        throw std::logic_error("sequenced command is missing result correlation");
    }
    return domain::CommandResultCorrelation{
        .clientId = message.clientId,
        .clientCommandId = *message.clientCommandId,
        .commandSequence = message.globalSequenceNumber,
    };
}

} // namespace

MatchingEngine::MatchingEngine(core::SharedQueue<sequencer::sequenceMessage>* sequencerQueue, core::Bus& multicastBus,
                               BoundedCommandResultQueue& commandResultQueue)
    : Task(std::vector<core::SharedQueue<sequencer::sequenceMessage>*>{sequencerQueue}),
      sequencerQueue(*sequencerQueue),
      multicastBus(multicastBus),
      commandResultQueue(commandResultQueue) {}

MatchingEngine::MatchingEngine(core::SharedQueue<sequencer::sequenceMessage>* sequencerQueue, core::Bus& multicastBus,
                               const std::size_t ownedResultQueueCapacity)
    : Task(std::vector<core::SharedQueue<sequencer::sequenceMessage>*>{sequencerQueue}),
      sequencerQueue(*sequencerQueue),
      multicastBus(multicastBus),
      ownedCommandResultQueue(std::make_unique<BoundedCommandResultQueue>(ownedResultQueueCapacity)),
      commandResultQueue(*ownedCommandResultQueue) {}

void MatchingEngine::run() {
    std::cout << "[MatchingEngine] Thread started" << std::endl;
    while (true) {
        drainQueue(sequencerQueue, "Sequencer");
    }
}

void MatchingEngine::send(sequencer::sequenceMessage& message) {
    if (!multicastBus.write(message)) {
        std::cerr << "[MatchingEngine] Failed to write message ID " << message.id << " to multicast bus" << std::endl;
    }
}

void MatchingEngine::drainQueue(core::SharedQueue<sequencer::sequenceMessage>& queue, const char* source,
                                std::size_t index) {
    if (!handoffPendingResult()) {
        return;
    }

    while (!queue.empty()) {
        sequencer::sequenceMessage message{};
        if (!queue.pop(message)) {
            return;
        }

        const domain::CommandResultCorrelation correlation = resultCorrelationFrom(message);
        ProcessingOutcome outcome = processMessage(message);
        ImmutableCommandResultBatch batch =
            std::make_shared<const CommandResultBatch>(correlation, outcome.result, std::move(outcome.events));

        send(message);
        if (!commandResultQueue.tryPush(batch)) {
            pendingCommandResult = std::move(batch);
            return;
        }
    }
}

bool MatchingEngine::handoffPendingResult() {
    if (pendingCommandResult == nullptr) {
        return true;
    }
    if (!commandResultQueue.tryPush(pendingCommandResult)) {
        return false;
    }
    pendingCommandResult.reset();
    return true;
}

MatchingEngine::ProcessingOutcome MatchingEngine::processBuyOrder(const sequencer::sequenceMessage& message) {
    if (message.tif != core::task::TimeInForce::GTC && message.tif != core::task::TimeInForce::IOC) {
        throw std::logic_error("unsupported TimeInForce reached matching engine");
    }
    if (activeOrders.contains(message.orderId)) {
        throw std::logic_error("duplicate authoritative OrderId reached matching engine");
    }

    return processOrder(message, message.tif == core::task::TimeInForce::GTC);
}

MatchingEngine::ProcessingOutcome MatchingEngine::processSellOrder(const sequencer::sequenceMessage& message) {
    if (message.tif != core::task::TimeInForce::GTC && message.tif != core::task::TimeInForce::IOC) {
        throw std::logic_error("unsupported TimeInForce reached matching engine");
    }
    if (activeOrders.contains(message.orderId)) {
        throw std::logic_error("duplicate authoritative OrderId reached matching engine");
    }

    return processOrder(message, message.tif == core::task::TimeInForce::GTC);
}

MatchingEngine::ProcessingOutcome MatchingEngine::processCancel(const sequencer::sequenceMessage& message) {
    if (message.type != sequencer::orderType::CANCEL || message.globalSequenceNumber.value() == 0 ||
        message.clientId.value() == 0 || !message.clientCommandId.has_value() || message.instrumentId.value() == 0 ||
        !message.targetOrderId.has_value()) {
        throw std::logic_error("cancel command is missing required normalized fields");
    }
    if (message.orderId.value() != 0) {
        throw std::logic_error("cancel command carries an unexpected authoritative OrderId");
    }

    const domain::OrderId targetOrderId = domain::orderIdFrom(*message.targetOrderId);
    auto activeOrder = activeOrders.find(targetOrderId);
    if (activeOrder == activeOrders.end()) {
        return rejectCancel(message, targetOrderId, domain::CommandRejectionReason::ORDER_NOT_ACTIVE);
    }
    if (activeOrder->second.owner != message.clientId) {
        return rejectCancel(message, targetOrderId, domain::CommandRejectionReason::NOT_OWNER);
    }
    if (activeOrder->second.instrumentId != message.instrumentId) {
        throw std::logic_error("cancel command routed to the wrong instrument book");
    }

    const domain::ClientId owner = activeOrder->second.owner;
    const domain::InstrumentId instrumentId = activeOrder->second.instrumentId;
    const domain::Quantity cancelledQuantity = activeOrder->second.remainingQuantity;

    std::vector<domain::BusinessEvent> events;
    events.reserve(1);
    events.emplace_back(domain::OrderCancelled{
        .eventId = makeEventId(message.globalSequenceNumber, 0),
        .orderId = targetOrderId,
        .clientId = owner,
        .instrumentId = instrumentId,
        .cancelledQuantity = cancelledQuantity,
        .reason = domain::CancelReason::CLIENT_REQUESTED,
    });

    removeCancelledOrder(activeOrder);
    return {ProcessingResult::APPLIED, std::move(events)};
}

MatchingEngine::ProcessingOutcome MatchingEngine::processOrder(const sequencer::sequenceMessage& message,
                                                               const bool restRemainder) {
    const MatchPlan plan = planMatches(message);
    const std::size_t plannedEventCount =
        checkedAddEventCount(plan.executionCount, plan.remainder.value() > 0 ? 1u : 0u);
    if (plannedEventCount > MAX_EVENTS_PER_COMMAND) {
        return rejectBookCapacity(message);
    }
    if (restRemainder && !canAddOrder(message, plan.remainder)) {
        return rejectBookCapacity(message);
    }

    if (plannedEventCount > 0) {
        static_cast<void>(makeEventId(message.globalSequenceNumber, plannedEventCount - 1));
    }
    std::vector<domain::BusinessEvent> events;
    events.reserve(plannedEventCount);

    domain::Quantity remaining = message.quantity;
    matchOrder(message, remaining, events);
    if (remaining != plan.remainder || events.size() != plan.executionCount) {
        throw std::logic_error("matching result diverged from mutation-free plan");
    }

    if (restRemainder && remaining.value() > 0) {
        const ProcessingResult result = addOrder(message, remaining);
        if (result != ProcessingResult::APPLIED) {
            throw std::logic_error("price-level capacity changed during single-writer processing");
        }

        events.emplace_back(domain::OrderRested{
            .eventId = makeEventId(message.globalSequenceNumber, events.size()),
            .orderId = message.orderId,
            .clientId = message.clientId,
            .instrumentId = message.instrumentId,
            .side = toDomainSide(message.type),
            .price = message.price,
            .remainingQuantity = remaining,
        });
    } else if (remaining.value() > 0) {
        events.emplace_back(domain::OrderCancelled{
            .eventId = makeEventId(message.globalSequenceNumber, events.size()),
            .orderId = message.orderId,
            .clientId = message.clientId,
            .instrumentId = message.instrumentId,
            .cancelledQuantity = remaining,
            .reason = domain::CancelReason::IOC_REMAINDER,
        });
    }

    if (events.size() != plannedEventCount) {
        throw std::logic_error("terminal outcome diverged from mutation-free plan");
    }
    return {ProcessingResult::APPLIED, std::move(events)};
}

bool MatchingEngine::canAddOrder(const sequencer::sequenceMessage& message, const domain::Quantity quantity) const {
    if (quantity.value() == 0) {
        return true;
    }

    const core::instrument::InstrumentConfiguration* configuration =
        core::instrument::findByIdentity(message.instrumentId, message.configurationVersion);
    if (configuration == nullptr) {
        throw std::logic_error("unknown instrument configuration reached matching engine");
    }

    domain::Quantity currentAggregate{};
    const auto instrument = orderBooks.find(message.instrumentId);
    if (message.type == sequencer::orderType::BUY) {
        if (instrument != orderBooks.end()) {
            const auto priceLevel = instrument->second.bids.find(message.price);
            if (priceLevel != instrument->second.bids.end()) {
                currentAggregate = priceLevel->second.totalQuantity;
            }
        }
    } else if (message.type == sequencer::orderType::SELL) {
        if (instrument != orderBooks.end()) {
            const auto priceLevel = instrument->second.asks.find(message.price);
            if (priceLevel != instrument->second.asks.end()) {
                currentAggregate = priceLevel->second.totalQuantity;
            }
        }
    } else {
        throw std::logic_error("non-order message cannot rest in the order book");
    }

    const domain::Quantity maximum = configuration->maxPriceLevelAggregate;
    return currentAggregate <= maximum && quantity.value() <= maximum.value() - currentAggregate.value();
}

MatchingEngine::ProcessingOutcome MatchingEngine::rejectBookCapacity(const sequencer::sequenceMessage& message) const {
    if (!message.clientCommandId.has_value()) {
        throw std::logic_error("sequenced command is missing ClientCommandId");
    }

    std::vector<domain::BusinessEvent> events;
    events.reserve(1);
    events.emplace_back(domain::CommandRejected{
        .eventId =
            domain::EventId{
                .commandSequence = message.globalSequenceNumber,
                .eventIndex = domain::EventIndex{0},
            },
        .commandType = domain::CommandType::NEW_ORDER,
        .clientId = message.clientId,
        .clientCommandId = *message.clientCommandId,
        .relevantOrderId = std::optional<domain::OrderId>{message.orderId},
        .reason = domain::CommandRejectionReason::BOOK_CAPACITY_EXCEEDED,
    });
    return {ProcessingResult::BOOK_CAPACITY_EXCEEDED, std::move(events)};
}

MatchingEngine::ProcessingOutcome MatchingEngine::rejectCancel(const sequencer::sequenceMessage& message,
                                                               const domain::OrderId targetOrderId,
                                                               const domain::CommandRejectionReason reason) const {
    std::vector<domain::BusinessEvent> events;
    events.reserve(1);
    events.emplace_back(domain::CommandRejected{
        .eventId = makeEventId(message.globalSequenceNumber, 0),
        .commandType = domain::CommandType::CANCEL,
        .clientId = message.clientId,
        .clientCommandId = *message.clientCommandId,
        .relevantOrderId = std::optional<domain::OrderId>{targetOrderId},
        .reason = reason,
    });
    return {ProcessingResult::APPLIED, std::move(events)};
}

MatchingEngine::ProcessingResult MatchingEngine::addOrder(const sequencer::sequenceMessage& message,
                                                          const domain::Quantity remainingQuantity) {
    if (activeOrders.contains(message.orderId)) {
        throw std::logic_error("duplicate authoritative OrderId reached matching engine");
    }
    if (remainingQuantity.value() == 0) {
        return ProcessingResult::APPLIED;
    }
    if (!canAddOrder(message, remainingQuantity)) {
        return ProcessingResult::BOOK_CAPACITY_EXCEEDED;
    }

    auto& instrumentBook = orderBooks[message.instrumentId];
    PriceLevel* priceLevel = nullptr;
    if (message.type == sequencer::orderType::BUY) {
        priceLevel = &instrumentBook.bids[message.price];
    } else if (message.type == sequencer::orderType::SELL) {
        priceLevel = &instrumentBook.asks[message.price];
    } else {
        throw std::logic_error("non-order message cannot rest in the order book");
    }

    priceLevel->orders.push_back(OrderNode{
        .orderId = message.orderId,
        .owner = message.clientId,
        .remainingQuantity = remainingQuantity,
    });
    const auto orderLocation = std::prev(priceLevel->orders.end());
    priceLevel->totalQuantity = domain::Quantity{priceLevel->totalQuantity.value() + remainingQuantity.value()};

    const auto [activeOrder, inserted] =
        activeOrders.emplace(message.orderId, ActiveOrder{
                                                  .owner = message.clientId,
                                                  .instrumentId = message.instrumentId,
                                                  .side = message.type,
                                                  .price = message.price,
                                                  .remainingQuantity = remainingQuantity,
                                                  .orderLocation = orderLocation,
                                              });
    static_cast<void>(activeOrder);
    if (!inserted) {
        throw std::logic_error("duplicate authoritative OrderId reached matching engine");
    }
    return ProcessingResult::APPLIED;
}

void MatchingEngine::matchOrder(const sequencer::sequenceMessage& message, domain::Quantity& remaining,
                                std::vector<domain::BusinessEvent>& events) {
    if (message.type == sequencer::orderType::BUY) {
        matchBuyOrder(message, remaining, events);
    } else if (message.type == sequencer::orderType::SELL) {
        matchSellOrder(message, remaining, events);
    } else {
        throw std::logic_error("non-order message cannot enter matching");
    }
}

void MatchingEngine::matchBuyOrder(const sequencer::sequenceMessage& message, domain::Quantity& remaining,
                                   std::vector<domain::BusinessEvent>& events) {
    while (remaining.value() > 0) {
        auto instrument = orderBooks.find(message.instrumentId);
        if (instrument == orderBooks.end() || instrument->second.asks.empty()) {
            return;
        }

        auto priceLevel = instrument->second.asks.begin();
        if (priceLevel->first > message.price) {
            return;
        }
        if (priceLevel->second.orders.empty()) {
            throw std::logic_error("empty price level remained in order book");
        }

        executeTrade(message, sequencer::orderType::SELL, domain::Side::BUY, priceLevel->first, priceLevel->second,
                     remaining, events);
    }
}

void MatchingEngine::matchSellOrder(const sequencer::sequenceMessage& message, domain::Quantity& remaining,
                                    std::vector<domain::BusinessEvent>& events) {
    while (remaining.value() > 0) {
        auto instrument = orderBooks.find(message.instrumentId);
        if (instrument == orderBooks.end() || instrument->second.bids.empty()) {
            return;
        }

        auto priceLevel = instrument->second.bids.begin();
        if (priceLevel->first < message.price) {
            return;
        }
        if (priceLevel->second.orders.empty()) {
            throw std::logic_error("empty price level remained in order book");
        }

        executeTrade(message, sequencer::orderType::BUY, domain::Side::SELL, priceLevel->first, priceLevel->second,
                     remaining, events);
    }
}

void MatchingEngine::executeTrade(const sequencer::sequenceMessage& message, const sequencer::orderType makerSide,
                                  const domain::Side takerSide, const domain::Price executionPrice,
                                  PriceLevel& priceLevel, domain::Quantity& remaining,
                                  std::vector<domain::BusinessEvent>& events) {
    auto orderLocation = priceLevel.orders.begin();
    auto activeOrder = activeOrders.find(orderLocation->orderId);
    if (activeOrder == activeOrders.end() || activeOrder->second.instrumentId != message.instrumentId ||
        activeOrder->second.side != makerSide || activeOrder->second.price != executionPrice ||
        activeOrder->second.orderLocation != orderLocation ||
        activeOrder->second.remainingQuantity != orderLocation->remainingQuantity ||
        activeOrder->second.owner != orderLocation->owner) {
        throw std::logic_error("active-order index is inconsistent with resting order node");
    }

    const uint64_t tradeQuantity = std::min(remaining.value(), orderLocation->remainingQuantity.value());
    if (tradeQuantity == 0 || tradeQuantity > priceLevel.totalQuantity.value()) {
        throw std::logic_error("invalid resting quantity or price-level aggregate during matching");
    }

    const domain::Quantity makerRemaining{orderLocation->remainingQuantity.value() - tradeQuantity};
    const domain::Quantity takerRemaining{remaining.value() - tradeQuantity};
    events.emplace_back(domain::Trade{
        .eventId = makeEventId(message.globalSequenceNumber, events.size()),
        .instrumentId = message.instrumentId,
        .makerOrderId = orderLocation->orderId,
        .makerClientId = orderLocation->owner,
        .takerOrderId = message.orderId,
        .takerClientId = message.clientId,
        .takerSide = takerSide,
        .executionPrice = executionPrice,
        .executionQuantity = domain::Quantity{tradeQuantity},
        .makerRemainingQuantity = makerRemaining,
        .takerRemainingQuantity = takerRemaining,
    });

    remaining = takerRemaining;
    orderLocation->remainingQuantity = makerRemaining;
    activeOrder->second.remainingQuantity = makerRemaining;
    priceLevel.totalQuantity = domain::Quantity{priceLevel.totalQuantity.value() - tradeQuantity};

    if (makerRemaining.value() == 0) {
        removeFilledOrder(activeOrder);
    }
}

MatchingEngine::MatchPlan MatchingEngine::planMatches(const sequencer::sequenceMessage& message) const {
    MatchPlan plan{.remainder = message.quantity, .executionCount = 0};
    const auto instrument = orderBooks.find(message.instrumentId);
    if (instrument == orderBooks.end()) {
        return plan;
    }

    const auto planBook = [&](const auto& book, const auto isEligible) {
        for (auto priceLevel = book.cbegin(); plan.remainder.value() > 0 && priceLevel != book.cend(); ++priceLevel) {
            if (!isEligible(priceLevel->first)) {
                break;
            }
            for (const OrderNode& order : priceLevel->second.orders) {
                if (plan.remainder.value() == 0) {
                    break;
                }
                if (order.remainingQuantity.value() == 0) {
                    throw std::logic_error("zero-quantity order remained in order book");
                }
                const uint64_t tradeQuantity = std::min(plan.remainder.value(), order.remainingQuantity.value());
                plan.remainder = domain::Quantity{plan.remainder.value() - tradeQuantity};
                plan.executionCount = checkedAddEventCount(plan.executionCount, 1);
            }
        }
    };

    if (message.type == sequencer::orderType::BUY) {
        planBook(instrument->second.asks, [&](const domain::Price price) { return price <= message.price; });
    } else if (message.type == sequencer::orderType::SELL) {
        planBook(instrument->second.bids, [&](const domain::Price price) { return price >= message.price; });
    } else {
        throw std::logic_error("non-order message cannot plan matching");
    }
    return plan;
}

void MatchingEngine::removeFilledOrder(std::map<domain::OrderId, ActiveOrder>::iterator activeOrder) {
    if (activeOrder == activeOrders.end() || activeOrder->second.remainingQuantity.value() != 0 ||
        activeOrder->second.orderLocation->remainingQuantity.value() != 0) {
        throw std::logic_error("only a fully filled active order can use filled-order removal");
    }

    const domain::InstrumentId instrumentId = activeOrder->second.instrumentId;
    const sequencer::orderType side = activeOrder->second.side;
    const domain::Price price = activeOrder->second.price;
    const OrderList::iterator orderLocation = activeOrder->second.orderLocation;

    auto instrument = orderBooks.find(instrumentId);
    if (instrument == orderBooks.end()) {
        throw std::logic_error("active order references a missing instrument book");
    }

    if (side == sequencer::orderType::BUY) {
        auto priceLevel = instrument->second.bids.find(price);
        if (priceLevel == instrument->second.bids.end()) {
            throw std::logic_error("active order references a missing bid level");
        }
        priceLevel->second.orders.erase(orderLocation);
        activeOrders.erase(activeOrder);
        if (priceLevel->second.orders.empty()) {
            if (priceLevel->second.totalQuantity.value() != 0) {
                throw std::logic_error("empty bid level has non-zero aggregate");
            }
            instrument->second.bids.erase(priceLevel);
        }
    } else if (side == sequencer::orderType::SELL) {
        auto priceLevel = instrument->second.asks.find(price);
        if (priceLevel == instrument->second.asks.end()) {
            throw std::logic_error("active order references a missing ask level");
        }
        priceLevel->second.orders.erase(orderLocation);
        activeOrders.erase(activeOrder);
        if (priceLevel->second.orders.empty()) {
            if (priceLevel->second.totalQuantity.value() != 0) {
                throw std::logic_error("empty ask level has non-zero aggregate");
            }
            instrument->second.asks.erase(priceLevel);
        }
    } else {
        throw std::logic_error("active order has a non-resting side");
    }

    if (instrument->second.bids.empty() && instrument->second.asks.empty()) {
        orderBooks.erase(instrument);
    }
}

void MatchingEngine::removeCancelledOrder(std::map<domain::OrderId, ActiveOrder>::iterator activeOrder) {
    if (activeOrder == activeOrders.end() || activeOrder->second.remainingQuantity.value() == 0) {
        throw std::logic_error("only an active positive-quantity order can be cancelled");
    }

    const domain::OrderId orderId = activeOrder->first;
    const domain::ClientId owner = activeOrder->second.owner;
    const domain::InstrumentId instrumentId = activeOrder->second.instrumentId;
    const sequencer::orderType side = activeOrder->second.side;
    const domain::Price price = activeOrder->second.price;
    const domain::Quantity remainingQuantity = activeOrder->second.remainingQuantity;
    const OrderList::iterator orderLocation = activeOrder->second.orderLocation;

    auto instrument = orderBooks.find(instrumentId);
    if (instrument == orderBooks.end()) {
        throw std::logic_error("active order references a missing instrument book");
    }

    const auto removeFromBook = [&](auto& book, const char* missingLevelMessage) {
        auto priceLevel = book.find(price);
        if (priceLevel == book.end()) {
            throw std::logic_error(missingLevelMessage);
        }

        PriceLevel& level = priceLevel->second;
        const auto storedOrder = std::find_if(level.orders.begin(), level.orders.end(),
                                              [&](const OrderNode& node) { return &node == &*orderLocation; });
        if (storedOrder == level.orders.end() || storedOrder->orderId != orderId || storedOrder->owner != owner ||
            storedOrder->remainingQuantity != remainingQuantity || level.totalQuantity < remainingQuantity) {
            throw std::logic_error("active-order index is inconsistent with cancellation target");
        }

        const bool removeLevel = level.orders.size() == 1;
        if ((removeLevel && level.totalQuantity != remainingQuantity) ||
            (!removeLevel && level.totalQuantity <= remainingQuantity)) {
            throw std::logic_error("price-level aggregate is inconsistent with cancellation target");
        }

        level.totalQuantity = domain::Quantity{level.totalQuantity.value() - remainingQuantity.value()};
        level.orders.erase(orderLocation);
        activeOrders.erase(activeOrder);
        if (removeLevel) {
            book.erase(priceLevel);
        }
    };

    if (side == sequencer::orderType::BUY) {
        removeFromBook(instrument->second.bids, "active order references a missing bid level");
    } else if (side == sequencer::orderType::SELL) {
        removeFromBook(instrument->second.asks, "active order references a missing ask level");
    } else {
        throw std::logic_error("active order has a non-resting side");
    }

    if (instrument->second.bids.empty() && instrument->second.asks.empty()) {
        orderBooks.erase(instrument);
    }
}

MatchingEngine::ProcessingOutcome MatchingEngine::processMessage(const sequencer::sequenceMessage& message) {
    switch (message.type) {
        case sequencer::orderType::BUY:
            return processBuyOrder(message);
        case sequencer::orderType::SELL:
            return processSellOrder(message);

        case sequencer::orderType::CANCEL:
            return processCancel(message);

        case sequencer::orderType::CANCELREJ:
            throw std::logic_error("CANCELREJ is not a valid matching-engine command");
    }
    throw std::logic_error("unsupported order type reached matching engine");
}

} // namespace exchange::matching_engine
