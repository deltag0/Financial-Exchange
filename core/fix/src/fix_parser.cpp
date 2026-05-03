#include "../include/fix_parser.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <quickfix/Fields.h>
#include <quickfix/FixFields.h>

namespace exchange::core::fix {

/* Global order counter */
std::atomic<uint64_t> orderCounter{0};

namespace {

using exchange::core::task::TimeInForce;

/*
 * Returns whether this parser can currently preserve the requested TIF
 * semantics all the way through the matching engine.
 *
 * Bad combinations found here:
 * - GTX is rejected until the engine has explicit crossing support.
 * - Unknown enum values are rejected by the final return path instead of being
 *   silently cast into a downstream switch.
 */
bool isSupportedTimeInForce(const TimeInForce tif) {
    switch (tif) {
    case TimeInForce::DAY:
    case TimeInForce::GTC:
    case TimeInForce::IOC:
    case TimeInForce::FOK:
    case TimeInForce::ATC:
    case TimeInForce::GTD:
        return true;
    case TimeInForce::GTX:
        return false;
    }
    return false;
}

/*
 * Temporary DAY policy until we have an exchange calendar/session model.
 * Invariant: returned time is the next UTC midnight and is strictly in the
 * future for a live system clock.
 */
std::chrono::system_clock::time_point endOfCurrentUtcDay() {
    const auto now = std::chrono::system_clock::now();
    const auto secondsSinceEpoch =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const auto nextUtcMidnightSeconds = ((secondsSinceEpoch / 86400) + 1) * 86400;
    return std::chrono::system_clock::time_point{std::chrono::seconds(nextUtcMidnightSeconds)};
}

/*
 * Exchange-owned ATC expiry policy until there is a full session calendar.
 * Invariant: returned time is 16:30:00 on the current UTC date.
 */
std::chrono::system_clock::time_point atcCloseOfCurrentUtcDay() {
    constexpr int64_t secondsPerDay = 86400;
    constexpr int64_t atcCloseSeconds = (16 * 60 * 60) + (30 * 60);

    const auto now = std::chrono::system_clock::now();
    const auto secondsSinceEpoch =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const auto currentUtcMidnightSeconds = (secondsSinceEpoch / secondsPerDay) * secondsPerDay;
    return std::chrono::system_clock::time_point{
        std::chrono::seconds(currentUtcMidnightSeconds + atcCloseSeconds)};
}

/*
 * Extracts TimeInForce and applies the exchange default.
 * Invariant: missing tag 59 is treated as DAY, matching common FIX behavior.
 *
 * Bad combinations found here:
 * - Unsupported TIF values are rejected before they can reach the sequencer.
 */
TimeInForce extractTimeInForce(const FIX::Message &fixMessage) {
    FIX::TimeInForce tif;
    if (!fixMessage.isSetField(tif)) {
        return TimeInForce::DAY;
    }

    fixMessage.getField(tif);
    const auto value = static_cast<TimeInForce>(tif.getValue());
    if (!isSupportedTimeInForce(value)) {
        throw FixValidationError("unsupported TimeInForce");
    }
    return value;
}

/*
 * Converts FIX ExpireTime into the system_clock representation used by
 * sequenceMessage. Caller must ensure tag 126 is present.
 */
std::chrono::system_clock::time_point parseExpireTime(const FIX::Message &fixMessage) {
    FIX::ExpireTime expiry;
    fixMessage.getField(expiry);

    const auto ts = expiry.getValue();
    return std::chrono::system_clock::time_point{std::chrono::seconds(ts.getTimeT())};
}

/*
 * Validates tag 126 (expiry time) against the already-validated TimeInForce and returns the
 * expiry value that should be stored on the sequenced order.
 *
 * Bad combinations found here:
 * - DAY + ExpireTime: rejected because DAY expiry is exchange/session-owned.
 * - GTC/IOC/FOK + ExpireTime: rejected because only GTD should carry ExpireTime.
 * - GTD without ExpireTime: rejected because GTD needs a concrete expiry.
 * - GTD with past ExpireTime: rejected because it is expired at ingress.
 * - ATC + ExpireTime: rejected because ATC expiry is exchange/session-owned.
 * - GTX: rejected because these semantics are not implemented yet.
 */
std::chrono::system_clock::time_point validateExpiry(const FIX::Message &fixMessage,
                                                     const TimeInForce tif) {
    const bool hasExpireTime = fixMessage.isSetField(FIX::FIELD::ExpireTime);

    switch (tif) {
    case TimeInForce::DAY:
        if (hasExpireTime) {
            throw FixValidationError("DAY orders must not specify ExpireTime");
        }
        return endOfCurrentUtcDay();

    case TimeInForce::GTC:
    case TimeInForce::IOC:
    case TimeInForce::FOK:
        if (hasExpireTime) {
            throw FixValidationError("ExpireTime is only valid for GTD orders");
        }
        return {};

    case TimeInForce::ATC:
        if (hasExpireTime) {
            throw FixValidationError("ATC orders must not specify ExpireTime");
        }
        return atcCloseOfCurrentUtcDay();

    case TimeInForce::GTD: {
        if (!hasExpireTime) {
            throw FixValidationError("GTD orders require ExpireTime");
        }

        const auto expiry = parseExpireTime(fixMessage);
        if (expiry <= std::chrono::system_clock::now()) {
            throw FixValidationError("ExpireTime must be in the future");
        }
        return expiry;
    }

    case TimeInForce::GTX:
        throw FixValidationError("unsupported TimeInForce");
    }

    throw FixValidationError("unsupported TimeInForce");
}

/*
 * Validates the FIX order type for NewOrderSingle.
 * Bad combinations found here: non-limit orders are rejected because the current
 * matching engine only models priced book orders.
 */
void validateNewOrderType(const FIX::Message &fixMessage) {
    FIX::OrdType ordType;
    if (!fixMessage.isSetField(ordType)) {
        throw FIX::FieldNotFound(FIX::FIELD::OrdType);
    }

    fixMessage.getField(ordType);
    if (ordType.getValue() != FIX::OrdType_LIMIT) {
        throw FixValidationError("only limit NewOrderSingle messages are supported");
    }
}

/*
 * Maps FIX Side into the internal order direction.
 * Bad combinations found here: anything other than buy/sell is rejected because
 * the sequencer only has BUY/SELL order directions.
 */
sequencer::orderType extractSide(const FIX::Message &fixMessage) {
    FIX::Side side;
    if (!fixMessage.isSetField(side)) {
        throw FIX::FieldNotFound(FIX::FIELD::Side);
    }

    fixMessage.getField(side);
    if (side.getValue() == FIX::Side_BUY) {
        return sequencer::orderType::BUY;
    }
    if (side.getValue() == FIX::Side_SELL) {
        return sequencer::orderType::SELL;
    }

    throw FixValidationError("unsupported Side");
}

/*
 * Extracts quantity in the integer lot model currently used downstream.
 * Bad combinations found here: zero, negative, non-finite, and fractional
 * quantities are rejected before integer conversion.
 */
uint64_t extractOrderQty(const FIX::Message &fixMessage) {
    FIX::OrderQty qty;
    if (!fixMessage.isSetField(qty)) {
        throw FIX::FieldNotFound(FIX::FIELD::OrderQty);
    }

    fixMessage.getField(qty);
    const auto value = qty.getValue();
    if (!std::isfinite(value) || value <= 0 || std::floor(value) != value) {
        throw FixValidationError("OrderQty must be a positive whole number");
    }

    return static_cast<uint64_t>(value);
}

/*
 * Extracts a positive limit price and converts it into fixed-point ticks
 * scaled by 10000 for sequenceMessage.
 * Bad combinations found here: zero, negative, and non-finite prices.
 */
uint64_t extractLimitPrice(const FIX::Message &fixMessage) {
    FIX::Price price;
    if (!fixMessage.isSetField(price)) {
        throw FIX::FieldNotFound(FIX::FIELD::Price);
    }

    fixMessage.getField(price);
    const auto value = price.getValue();
    if (!std::isfinite(value) || value <= 0) {
        throw FixValidationError("Price must be positive");
    }

    return static_cast<uint64_t>(value * 10000);
}

/*
 * Cancel requests operate on an existing client order ID and must not carry
 * placement-only lifetime fields.
 * Bad combinations found here: Cancel + TimeInForce and Cancel + ExpireTime.
 */
void validateCancel(const FIX::Message &fixMessage) {
    if (fixMessage.isSetField(FIX::FIELD::TimeInForce) ||
        fixMessage.isSetField(FIX::FIELD::ExpireTime)) {
        throw FixValidationError("cancel messages must not specify TimeInForce or ExpireTime");
    }
}

} /* namespace */

bool isProcessableMessageType(const std::string &msgType) {
    return msgType == "D" || msgType == "F";
}

sequencer::sequenceMessage parseFixMessage(const FIX::Message &fixMessage,
                                           const FIX::SessionID &sessionID, size_t numShards) {
    try {
        FIX::MsgType msgType;
        fixMessage.getHeader().getField(msgType);

        sequencer::sequenceMessage seqMsg{};
        seqMsg.port = std::hash<std::string>{}(sessionID.toString());

        if (numShards == 0) {
            throw FixValidationError("numShards must be greater than zero");
        }

        /* Determine order type based on message type */
        if (msgType.getValue() == "F") {
            validateCancel(fixMessage);
            seqMsg.type = sequencer::orderType::CANCEL;
        } else if (msgType.getValue() == "D") {
            validateNewOrderType(fixMessage);
            seqMsg.type = extractSide(fixMessage);
        }

        /* Extract symbol */
        FIX::Symbol symbol;
        if (fixMessage.isSetField(symbol)) {
            fixMessage.getField(symbol);
            std::strncpy(seqMsg.symbol, symbol.getValue().c_str(), sizeof(seqMsg.symbol) - 1);
            seqMsg.symbol[sizeof(seqMsg.symbol) - 1] = '\0';

            /* Determine shard based on ticker hash */
            std::string ticker{seqMsg.symbol};
            seqMsg.shard_id = std::hash<std::string>{}(ticker) % numShards;
        }

        /* Extract order quantity */
        if (seqMsg.type != sequencer::orderType::CANCEL) {
            seqMsg.quantity = extractOrderQty(fixMessage);
        }

        /* Extract price (in basis points, multiplied by 10000) */
        if (seqMsg.type != sequencer::orderType::CANCEL) {
            seqMsg.price = extractLimitPrice(fixMessage);
        }

        /* Extract order ID */
        FIX::ClOrdID clOrdID;
        if (fixMessage.isSetField(clOrdID)) {
            fixMessage.getField(clOrdID);
            seqMsg.id = std::hash<std::string>{}(clOrdID.getValue());
        }

        if (seqMsg.type != sequencer::orderType::CANCEL) {
            seqMsg.tif = extractTimeInForce(fixMessage);
            seqMsg.expiry = validateExpiry(fixMessage, seqMsg.tif);
        }

        /* Assign global order counter and timestamp */
        seqMsg.order = orderCounter.fetch_add(1, std::memory_order_seq_cst);
        seqMsg.timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();

        return seqMsg;

    } catch (const FIX::FieldNotFound &e) {
        std::cerr << "[FixParser] Required field missing: " << e.field << std::endl;
        throw;
    } catch (const std::exception &e) {
        std::cerr << "[FixParser] Error parsing FIX message: " << e.what() << std::endl;
        throw;
    }
}

} /* namespace exchange::core::fix */
