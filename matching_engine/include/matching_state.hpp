#pragma once

#include "command_result.hpp"

#include "../../sequencer/include/sequence_message.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <vector>

namespace exchange::matching_engine {

struct RestingOrderSnapshot final {
    domain::OrderId orderId{};
    domain::ClientId owner{};
    domain::Quantity remainingQuantity{};

    bool operator==(const RestingOrderSnapshot &) const = default;
};

struct PriceLevelSnapshot final {
    domain::InstrumentId instrumentId{};
    domain::Side side{domain::Side::BUY};
    domain::Price price{};
    domain::Quantity totalQuantity{};
    std::vector<RestingOrderSnapshot> orders{};

    bool operator==(const PriceLevelSnapshot &) const = default;
};

struct ActiveOrderSnapshot final {
    domain::OrderId orderId{};
    domain::ClientId owner{};
    domain::InstrumentId instrumentId{};
    domain::Side side{domain::Side::BUY};
    domain::Price price{};
    domain::Quantity remainingQuantity{};

    bool operator==(const ActiveOrderSnapshot &) const = default;
};

struct MatchingStateSnapshot final {
    domain::ExchangeRunId exchangeRunId{};
    std::vector<PriceLevelSnapshot> priceLevels{};
    std::vector<ActiveOrderSnapshot> activeOrders{};

    bool operator==(const MatchingStateSnapshot &) const = default;
};

// Owns one run's mutable books and applies one command synchronously without publication or threading.
class MatchingState {
public:
    explicit MatchingState(domain::ExchangeRunId exchangeRunId = {}) noexcept : exchangeRunId_(exchangeRunId) {}

    MatchingState(const MatchingState &) = delete;
    MatchingState &operator=(const MatchingState &) = delete;
    MatchingState(MatchingState &&) = delete;
    MatchingState &operator=(MatchingState &&) = delete;

    [[nodiscard]] ImmutableCommandResultBatch processCommand(const sequencer::sequenceMessage &message);
    [[nodiscard]] MatchingStateSnapshot snapshot() const;
    [[nodiscard]] bool invariantsHold() const noexcept;

protected:
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

    ProcessingOutcome processMessage(const sequencer::sequenceMessage &message);
    ProcessingOutcome processBuyOrder(const sequencer::sequenceMessage &message);
    ProcessingOutcome processSellOrder(const sequencer::sequenceMessage &message);
    ProcessingOutcome processCancel(const sequencer::sequenceMessage &message);
    ProcessingOutcome processOrder(const sequencer::sequenceMessage &message, bool restRemainder);

    void matchOrder(const sequencer::sequenceMessage &message, domain::Quantity &remaining,
                    std::vector<domain::BusinessEvent> &events);
    void matchBuyOrder(const sequencer::sequenceMessage &message, domain::Quantity &remaining,
                       std::vector<domain::BusinessEvent> &events);
    void matchSellOrder(const sequencer::sequenceMessage &message, domain::Quantity &remaining,
                        std::vector<domain::BusinessEvent> &events);
    void executeTrade(const sequencer::sequenceMessage &message, sequencer::orderType makerSide, domain::Side takerSide,
                      domain::Price executionPrice, PriceLevel &priceLevel, domain::Quantity &remaining,
                      std::vector<domain::BusinessEvent> &events);

    MatchPlan planMatches(const sequencer::sequenceMessage &message) const;
    bool canAddOrder(const sequencer::sequenceMessage &message, domain::Quantity quantity) const;
    ProcessingOutcome rejectBookCapacity(const sequencer::sequenceMessage &message) const;
    ProcessingOutcome rejectCancel(const sequencer::sequenceMessage &message, domain::OrderId targetOrderId,
                                   domain::CommandRejectionReason reason) const;
    void removeFilledOrder(std::map<domain::OrderId, ActiveOrder>::iterator activeOrder);
    void removeCancelledOrder(std::map<domain::OrderId, ActiveOrder>::iterator activeOrder);
    ProcessingResult addOrder(const sequencer::sequenceMessage &message, domain::Quantity remainingQuantity);

    domain::ExchangeRunId exchangeRunId_{};
    std::map<domain::InstrumentId, InstrumentBook> orderBooks;
    std::map<domain::OrderId, ActiveOrder> activeOrders;
};

} // namespace exchange::matching_engine
