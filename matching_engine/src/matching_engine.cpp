#include "../include/matching_engine.hpp"

#include "../../core/task/include/task.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <sys/types.h>
#include <thread>

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

        static_cast<void>(processMessage(message));

        send(message);
    }
}

MatchingEngine::ProcessingResult MatchingEngine::processBuyOrder(sequencer::sequenceMessage& message) {
    switch (message.tif) {
        case core::task::TimeInForce::IOC: {
            matchBuyOrder(message);
            return ProcessingResult::APPLIED;
        }
        case core::task::TimeInForce::FOK: {
            if (canFullyFillBuyOrder(message)) {
                matchBuyOrder(message);
            }
            return ProcessingResult::APPLIED;
        }
        case core::task::TimeInForce::GTX: {
            return ProcessingResult::APPLIED;
        }
        case core::task::TimeInForce::ATC:
            return ProcessingResult::APPLIED;
        case core::task::TimeInForce::GTC:
        case core::task::TimeInForce::DAY:
        case core::task::TimeInForce::GTD: {
            const uint64_t prospectiveRemainder = calculateBuyRemainder(message);
            if (!canAddOrder(message, prospectiveRemainder)) {
                return ProcessingResult::BOOK_CAPACITY_EXCEEDED;
            }
            matchBuyOrder(message);
            if (message.quantity > 0) {
                const ProcessingResult result = addOrder(message);
                if (result != ProcessingResult::APPLIED) {
                    throw std::logic_error("price-level capacity changed during single-writer processing");
                }
            }
            return ProcessingResult::APPLIED;
        }
    }
    throw std::logic_error("unsupported TimeInForce reached matching engine");
}

void MatchingEngine::matchSellOrder(sequencer::sequenceMessage& message) {}

MatchingEngine::ProcessingResult MatchingEngine::processSellOrder(sequencer::sequenceMessage& message) {
    matchSellOrder(message);
    return ProcessingResult::APPLIED;
}

bool MatchingEngine::canAddOrder(const sequencer::sequenceMessage& message, const uint64_t quantity) const {
    if (quantity == 0) {
        return true;
    }

    const core::instrument::InstrumentConfiguration* configuration =
        core::instrument::findByIdentity(message.instrumentId, message.configurationVersion);
    if (configuration == nullptr) {
        throw std::logic_error("unknown instrument configuration reached matching engine");
    }

    uint64_t currentAggregate = 0;
    if (message.type == sequencer::orderType::BUY) {
        const auto instrument = buyOrders.find(message.symbol);
        if (instrument != buyOrders.end()) {
            const auto priceLevel = instrument->second.find(message.price);
            if (priceLevel != instrument->second.end()) {
                currentAggregate = priceLevel->second.totalQuantity;
            }
        }
    } else if (message.type == sequencer::orderType::SELL) {
        const auto instrument = sellOrders.find(message.symbol);
        if (instrument != sellOrders.end()) {
            const auto priceLevel = instrument->second.find(message.price);
            if (priceLevel != instrument->second.end()) {
                currentAggregate = priceLevel->second.totalQuantity;
            }
        }
    } else {
        throw std::logic_error("non-order message cannot rest in the order book");
    }

    const uint64_t maximum = configuration->maxPriceLevelAggregate;
    return currentAggregate <= maximum && quantity <= maximum - currentAggregate;
}

MatchingEngine::ProcessingResult MatchingEngine::addOrder(const sequencer::sequenceMessage& message) {
    if (!canAddOrder(message, message.quantity)) {
        return ProcessingResult::BOOK_CAPACITY_EXCEEDED;
    }

    if (message.type == sequencer::orderType::BUY) {
        if (!buyOrders.contains(message.symbol)) {
            buyOrders[message.symbol] = {};
        }
        auto& priceLevel = buyOrders[message.symbol][message.price];
        priceLevel.orders.push(message);
        priceLevel.totalQuantity += message.quantity;

        orderIds.insert(message.id);
    } else if (message.type == sequencer::orderType::SELL) {
        if (!sellOrders.contains(message.symbol)) {
            sellOrders[message.symbol] = {};
        }
        auto& priceLevel = sellOrders[message.symbol][message.price];
        priceLevel.orders.push(message);
        priceLevel.totalQuantity += message.quantity;

        orderIds.insert(message.id);
    }
    return ProcessingResult::APPLIED;
}

bool MatchingEngine::checkOrderExpiry(const sequencer::sequenceMessage& message) {
    if (hasOrderExpired(message.expiry)) {
        removeOrder(message);
        return true;
    }
    return false;
}

void MatchingEngine::removeOrder(const sequencer::sequenceMessage& message,
                                 std::queue<sequencer::sequenceMessage>* orderQueue) {
    if (!orderIds.contains(message.id)) {
        return;
    }

    if (message.type == sequencer::orderType::BUY) {
        auto& priceLevel = buyOrders[message.symbol][message.price];
        auto& orders = orderQueue != nullptr ? *orderQueue : priceLevel.orders;
        if (!orders.empty()) {
            priceLevel.totalQuantity -= message.quantity;
            orders.pop();
        }
    } else if (message.type == sequencer::orderType::SELL) {
        auto& priceLevel = sellOrders[message.symbol][message.price];
        auto& orders = orderQueue != nullptr ? *orderQueue : priceLevel.orders;
        if (!orders.empty()) {
            priceLevel.totalQuantity -= message.quantity;
            orders.pop();
        }
    }

    cleanBook(message);
    orderIds.erase(message.id);
}

void MatchingEngine::matchBuyOrder(sequencer::sequenceMessage& message) {
    uint64_t& remaining = message.quantity;

    while (remaining > 0 && sellOrders.contains(message.symbol) && !sellOrders[message.symbol].empty() &&
           sellOrders[message.symbol].cbegin()->first <= message.price) {
        while (remaining > 0 && !sellOrders[message.symbol].begin()->second.orders.empty()) {
            auto& bestSellLevel = sellOrders[message.symbol].begin()->second;
            auto& bestSellQueue = bestSellLevel.orders;
            auto& bestSell = bestSellQueue.front();
            uint64_t tradeQty = std::min(remaining, bestSell.quantity);

            remaining -= tradeQty;
            bestSell.quantity -= tradeQty;
            bestSellLevel.totalQuantity -= tradeQty;

            if (bestSell.quantity == 0) {
                removeOrder(bestSell, &bestSellQueue);
                break;
            }
        }
    }
}

bool MatchingEngine::canFullyFillBuyOrder(const sequencer::sequenceMessage& message) const {
    uint64_t remaining = message.quantity;
    const auto symbolSellOrders = sellOrders.find(message.symbol);
    if (symbolSellOrders == sellOrders.end()) {
        return false;
    }

    for (auto priceLevel = symbolSellOrders->second.cbegin();
         remaining > 0 && priceLevel != symbolSellOrders->second.cend() && priceLevel->first <= message.price;
         ++priceLevel) {
        const uint64_t availableAtPrice = priceLevel->second.totalQuantity;
        const uint64_t tradeQty = std::min(remaining, availableAtPrice);
        remaining -= tradeQty;
    }

    return remaining == 0;
}

uint64_t MatchingEngine::calculateBuyRemainder(const sequencer::sequenceMessage& message) const {
    uint64_t remaining = message.quantity;
    const auto symbolSellOrders = sellOrders.find(message.symbol);
    if (symbolSellOrders == sellOrders.end()) {
        return remaining;
    }

    for (auto priceLevel = symbolSellOrders->second.cbegin();
         remaining > 0 && priceLevel != symbolSellOrders->second.cend() && priceLevel->first <= message.price;
         ++priceLevel) {
        const uint64_t tradeQuantity = std::min(remaining, priceLevel->second.totalQuantity);
        remaining -= tradeQuantity;
    }
    return remaining;
}

bool MatchingEngine::cleanBook(const sequencer::sequenceMessage& message) {
    if (message.type == sequencer::orderType::BUY) {
        if (!buyOrders.contains(message.symbol)) {
            return false;
        }

        auto& priceMap = buyOrders[message.symbol];

        if (priceMap[message.price].orders.empty()) {
            priceMap.erase(message.price);
        }
    } else if (message.type == sequencer::orderType::SELL) {
        if (!sellOrders.contains(message.symbol)) {
            return false;
        }

        auto& priceMap = sellOrders[message.symbol];

        if (priceMap[message.price].orders.empty()) {
            priceMap.erase(message.price);
        }
    }
    return true;
}

MatchingEngine::ProcessingResult MatchingEngine::processMessage(sequencer::sequenceMessage& message) {
    if (message.type != sequencer::orderType::CANCEL && hasOrderExpired(message.expiry)) {
        return ProcessingResult::APPLIED;
    }

    switch (message.type) {
        case sequencer::orderType::BUY:
            return processBuyOrder(message);
        case sequencer::orderType::SELL:
            return processSellOrder(message);

        case sequencer::orderType::CANCEL:
            return ProcessingResult::APPLIED;

        case sequencer::orderType::CANCELREJ:
            return ProcessingResult::APPLIED;
    }
    throw std::logic_error("unsupported order type reached matching engine");
}

} // namespace exchange::matching_engine
