#include "../include/matching_engine.hpp"

#include "../../core/task/include/task.hpp"
#include <chrono>
#include <iostream>
#include <sys/types.h>
#include <thread>

namespace exchange::matching_engine {

namespace {

/*
 * Returns whether an order lifetime has elapsed.
 * Invariant: a default-constructed expiry means the order has no expiry.
 */
bool hasOrderExpired(const std::chrono::system_clock::time_point expiry) {
    return expiry != std::chrono::system_clock::time_point{} &&
           expiry <= std::chrono::system_clock::now();
}

} // namespace

MatchingEngine::MatchingEngine(core::SharedQueue<sequencer::sequenceMessage> *sequencerQueue,
                               core::Bus &multicastBus)
    : Task(std::vector<core::SharedQueue<sequencer::sequenceMessage> *>{sequencerQueue}),
      sequencerQueue(*sequencerQueue), multicastBus(multicastBus) {}

void MatchingEngine::run() {
    std::cout << "[MatchingEngine] Thread started" << std::endl;
    while (true) {
        drainQueue(sequencerQueue, "Sequencer");
    }
}

void MatchingEngine::send(sequencer::sequenceMessage &message) {
    if (!multicastBus.write(message)) {
        std::cerr << "[MatchingEngine] Failed to write message ID " << message.id
                  << " to multicast bus" << std::endl;
    }
}

void MatchingEngine::drainQueue(core::SharedQueue<sequencer::sequenceMessage> &queue,
                                const char *source, std::size_t index) {
    while (!queue.empty()) {
        sequencer::sequenceMessage message{};
        if (!queue.pop(message)) {
            return;
        }

        processMessage(message);

        send(message);
    }
}

void MatchingEngine::processBuyOrder(sequencer::sequenceMessage &message) {
    switch (message.tif) {
    case core::task::TimeInForce::IOC: {
        matchBuyOrder(message);
        break;
    }
    case core::task::TimeInForce::FOK: {
        if (canFullyFillBuyOrder(message)) {
            matchBuyOrder(message);
        }
        break;
    }
    case core::task::TimeInForce::GTX: {
        break;
    }
    case core::task::TimeInForce::ATC:
        break;
    case core::task::TimeInForce::GTC:
    case core::task::TimeInForce::DAY:
    case core::task::TimeInForce::GTD:
        matchBuyOrder(message);
        if (message.quantity > 0) {
            addOrder(message);
        }
        break;
    }
}

void MatchingEngine::matchSellOrder(sequencer::sequenceMessage &message) {}

void MatchingEngine::processSellOrder(sequencer::sequenceMessage &message) {
    matchSellOrder(message);
}

void MatchingEngine::addOrder(const sequencer::sequenceMessage &message) {
    if (message.type == sequencer::orderType::BUY) {
        if (!buyOrders.contains(message.symbol)) {
            buyOrders[message.symbol] = {};
        }
        auto &priceLevel = buyOrders[message.symbol][message.price];
        priceLevel.orders.push(message);
        priceLevel.totalQuantity += message.quantity;

        orderIds.insert(message.id);
    } else if (message.type == sequencer::orderType::SELL) {
        if (!sellOrders.contains(message.symbol)) {
            sellOrders[message.symbol] = {};
        }
        auto &priceLevel = sellOrders[message.symbol][message.price];
        priceLevel.orders.push(message);
        priceLevel.totalQuantity += message.quantity;

        orderIds.insert(message.id);
    }
}

bool MatchingEngine::checkOrderExpiry(const sequencer::sequenceMessage &message) {
    if (hasOrderExpired(message.expiry)) {
        removeOrder(message);
        return true;
    }
    return false;
}

void MatchingEngine::removeOrder(const sequencer::sequenceMessage &message,
                                 std::queue<sequencer::sequenceMessage> *orderQueue) {
    if (!orderIds.contains(message.id)) {
        return;
    }

    if (message.type == sequencer::orderType::BUY) {
        auto &priceLevel = buyOrders[message.symbol][message.price];
        auto &orders = orderQueue != nullptr ? *orderQueue : priceLevel.orders;
        if (!orders.empty()) {
            priceLevel.totalQuantity -= message.quantity;
            orders.pop();
        }
    } else if (message.type == sequencer::orderType::SELL) {
        auto &priceLevel = sellOrders[message.symbol][message.price];
        auto &orders = orderQueue != nullptr ? *orderQueue : priceLevel.orders;
        if (!orders.empty()) {
            priceLevel.totalQuantity -= message.quantity;
            orders.pop();
        }
    }

    cleanBook(message);
    orderIds.erase(message.id);
}

void MatchingEngine::matchBuyOrder(sequencer::sequenceMessage &message) {
    uint64_t &remaining = message.quantity;

    while (remaining > 0 && sellOrders.contains(message.symbol) &&
           !sellOrders[message.symbol].empty() &&
           sellOrders[message.symbol].cbegin()->first <= message.price) {

        while (remaining > 0 && !sellOrders[message.symbol].begin()->second.orders.empty()) {

            auto &bestSellLevel = sellOrders[message.symbol].begin()->second;
            auto &bestSellQueue = bestSellLevel.orders;
            auto &bestSell = bestSellQueue.front();
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

bool MatchingEngine::canFullyFillBuyOrder(const sequencer::sequenceMessage &message) const {
    uint64_t remaining = message.quantity;
    const auto symbolSellOrders = sellOrders.find(message.symbol);
    if (symbolSellOrders == sellOrders.end()) {
        return false;
    }

    for (auto priceLevel = symbolSellOrders->second.cbegin();
         remaining > 0 && priceLevel != symbolSellOrders->second.cend() &&
         priceLevel->first <= message.price;
         ++priceLevel) {
        const uint64_t availableAtPrice = priceLevel->second.totalQuantity;
        const uint64_t tradeQty = std::min(remaining, availableAtPrice);
        remaining -= tradeQty;
    }

    return remaining == 0;
}

bool MatchingEngine::cleanBook(const sequencer::sequenceMessage &message) {
    if (message.type == sequencer::orderType::BUY) {
        if (!buyOrders.contains(message.symbol)) {
            return false;
        }

        auto &priceMap = buyOrders[message.symbol];

        if (priceMap[message.price].orders.empty()) {
            priceMap.erase(message.price);
        }
    } else if (message.type == sequencer::orderType::SELL) {
        if (!sellOrders.contains(message.symbol)) {
            return false;
        }

        auto &priceMap = sellOrders[message.symbol];

        if (priceMap[message.price].orders.empty()) {
            priceMap.erase(message.price);
        }
    }
    return true;
}

void MatchingEngine::processMessage(sequencer::sequenceMessage &message) {
    if (message.type != sequencer::orderType::CANCEL && hasOrderExpired(message.expiry)) {
        return;
    }

    switch (message.type) {
    case sequencer::orderType::BUY:
        processBuyOrder(message);

        break;
    case sequencer::orderType::SELL:
        processSellOrder(message);

        break;

    case sequencer::orderType::CANCEL:

        break;

    case sequencer::orderType::CANCELREJ:

        break;
    }
}

} // namespace exchange::matching_engine
