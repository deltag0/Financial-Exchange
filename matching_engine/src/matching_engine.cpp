#include "../include/matching_engine.hpp"

#include "../../core/task/include/task.hpp"
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/types.h>
#include <thread>
#include <utility>

namespace exchange::matching_engine {

namespace {

/*
 * Returns whether an order lifetime has elapsed.
 * Invariant: a default-constructed expiry means the order has no expiry.
 */
bool hasOrderExpired(const std::chrono::system_clock::time_point expiry) {
    return expiry != std::chrono::system_clock::time_point{} && expiry <= std::chrono::system_clock::now();
}

} // namespace

MatchingEngine::MatchingEngine(core::SharedQueue<sequencer::sequenceMessage>* sequencerQueue, core::Bus& multicastBus)
    : Task(std::vector<core::SharedQueue<sequencer::sequenceMessage>*>{sequencerQueue}),
      sequencerQueue(*sequencerQueue),
      multicastBus(multicastBus) {}

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
    while (!queue.empty()) {
        sequencer::sequenceMessage message{};
        if (!queue.pop(message)) {
            return;
        }

        const ProcessingOutcome outcome = processMessage(message);
        static_cast<void>(outcome);

        send(message);
    }
}

MatchingEngine::ProcessingOutcome MatchingEngine::processBuyOrder(const sequencer::sequenceMessage& message) {
    if (activeOrders.contains(message.orderId)) {
        throw std::logic_error("duplicate authoritative OrderId reached matching engine");
    }

    switch (message.tif) {
        case core::task::TimeInForce::IOC:
            return processOrder(message, false, false);
        case core::task::TimeInForce::FOK:
            return processOrder(message, false, true);
        case core::task::TimeInForce::GTX: {
            return {ProcessingResult::APPLIED, {}};
        }
        case core::task::TimeInForce::ATC:
            return {ProcessingResult::APPLIED, {}};
        case core::task::TimeInForce::GTC:
        case core::task::TimeInForce::DAY:
        case core::task::TimeInForce::GTD:
            return processOrder(message, true, false);
    }
    throw std::logic_error("unsupported TimeInForce reached matching engine");
}

MatchingEngine::ProcessingOutcome MatchingEngine::processSellOrder(const sequencer::sequenceMessage& message) {
    if (activeOrders.contains(message.orderId)) {
        throw std::logic_error("duplicate authoritative OrderId reached matching engine");
    }

    switch (message.tif) {
        case core::task::TimeInForce::IOC:
            return processOrder(message, false, false);
        case core::task::TimeInForce::GTC:
            return processOrder(message, true, false);
        case core::task::TimeInForce::FOK:
        case core::task::TimeInForce::GTX:
        case core::task::TimeInForce::ATC:
        case core::task::TimeInForce::DAY:
        case core::task::TimeInForce::GTD:
            return {ProcessingResult::APPLIED, {}};
    }
    throw std::logic_error("unsupported TimeInForce reached matching engine");
}

MatchingEngine::ProcessingOutcome MatchingEngine::processOrder(const sequencer::sequenceMessage& message,
                                                               const bool restRemainder, const bool requireFullFill) {
    const MatchPlan plan = planMatches(message);
    if (requireFullFill && plan.remainder.value() > 0) {
        return {ProcessingResult::APPLIED, {}};
    }
    if (restRemainder && !canAddOrder(message, plan.remainder)) {
        return rejectBookCapacity(message);
    }

    std::vector<domain::BusinessEvent> events;
    events.reserve(plan.executionCount);

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
    if (events.size() > std::numeric_limits<domain::EventIndex::Underlying>::max()) {
        throw std::logic_error("EventIndex exhausted during one command");
    }

    events.emplace_back(domain::Trade{
        .eventId =
            domain::EventId{
                .commandSequence = message.globalSequenceNumber,
                .eventIndex = domain::EventIndex{static_cast<domain::EventIndex::Underlying>(events.size())},
            },
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
                ++plan.executionCount;
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

MatchingEngine::ProcessingOutcome MatchingEngine::processMessage(const sequencer::sequenceMessage& message) {
    if (message.type != sequencer::orderType::CANCEL && hasOrderExpired(message.expiry)) {
        return {ProcessingResult::APPLIED, {}};
    }

    switch (message.type) {
        case sequencer::orderType::BUY:
            return processBuyOrder(message);
        case sequencer::orderType::SELL:
            return processSellOrder(message);

        case sequencer::orderType::CANCEL:
            return {ProcessingResult::APPLIED, {}};

        case sequencer::orderType::CANCELREJ:
            return {ProcessingResult::APPLIED, {}};
    }
    throw std::logic_error("unsupported order type reached matching engine");
}

} // namespace exchange::matching_engine
