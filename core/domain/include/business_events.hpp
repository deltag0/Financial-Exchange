#pragma once

#include "domain_types.hpp"

#include <optional>
#include <variant>

namespace exchange::domain {

enum class Side : std::uint8_t {
    BUY = 1,
    SELL = 2,
};

enum class CommandType : std::uint8_t {
    NEW_ORDER = 1,
    CANCEL = 2,
};

enum class AdmissionRejectionReason : std::uint8_t {
    DUPLICATE_COMMAND_CONFLICT = 1,
};

enum class CommandRejectionReason : std::uint8_t {
    ORDER_NOT_ACTIVE = 2,
    NOT_OWNER = 3,
    BOOK_CAPACITY_EXCEEDED = 4,
};

enum class CancelReason : std::uint8_t {
    CLIENT_REQUESTED = 1,
    IOC_REMAINDER = 2,
};

struct EventId final {
    const CommandSequence commandSequence;
    const EventIndex eventIndex;

    bool operator==(const EventId&) const = default;
};

struct CommandRejected final {
    const EventId eventId;
    const CommandType commandType;
    const ClientId clientId;
    const ClientCommandId clientCommandId;
    const std::optional<OrderId> relevantOrderId;
    const CommandRejectionReason reason;

    bool operator==(const CommandRejected&) const = default;
};

struct Trade final {
    const EventId eventId;
    const InstrumentId instrumentId;
    const OrderId makerOrderId;
    const ClientId makerClientId;
    const OrderId takerOrderId;
    const ClientId takerClientId;
    const Side takerSide;
    const Price executionPrice;
    const Quantity executionQuantity;
    const Quantity makerRemainingQuantity;
    const Quantity takerRemainingQuantity;

    bool operator==(const Trade&) const = default;
};

struct OrderRested final {
    const EventId eventId;
    const OrderId orderId;
    const ClientId clientId;
    const InstrumentId instrumentId;
    const Side side;
    const Price price;
    const Quantity remainingQuantity;

    bool operator==(const OrderRested&) const = default;
};

struct OrderCancelled final {
    const EventId eventId;
    const OrderId orderId;
    const ClientId clientId;
    const InstrumentId instrumentId;
    const Quantity cancelledQuantity;
    const CancelReason reason;

    bool operator==(const OrderCancelled&) const = default;
};

using BusinessEvent = std::variant<CommandRejected, Trade, OrderRested, OrderCancelled>;

} // namespace exchange::domain
