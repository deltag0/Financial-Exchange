#include "../include/fix_parser.hpp"
#include "instrument_config.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <string_view>

#include <quickfix/Fields.h>
#include <quickfix/FixFields.h>

namespace exchange::core::fix {

/* Global order counter */
std::atomic<uint64_t> orderCounter{0};

namespace {

using exchange::core::task::TimeInForce;

std::uint64_t powerOfTen(const std::uint32_t exponent) {
    std::uint64_t value = 1;
    for (std::uint32_t index = 0; index < exponent; ++index) {
        if (value > std::numeric_limits<std::uint64_t>::max() / 10) {
            throw std::logic_error("instrument price scale is too large");
        }
        value *= 10;
    }
    return value;
}

std::uint64_t parseUnsignedWhole(const std::string_view text, const std::uint64_t maximum,
                                 const std::string_view fieldName) {
    if (text.empty()) {
        throw FixValidationError(std::string(fieldName) + " must not be empty");
    }

    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            throw FixValidationError(std::string(fieldName) + " must be an unsigned decimal");
        }

        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10)) {
            throw FixValidationError(std::string(fieldName) + " exceeds the configured maximum");
        }
        value = value * 10 + digit;
    }
    return value;
}

domain::Price parsePriceTicks(const std::string_view text, const instrument::InstrumentConfiguration& configuration) {
    const std::size_t decimalPoint = text.find('.');
    if (decimalPoint != std::string_view::npos && text.find('.', decimalPoint + 1) != std::string_view::npos) {
        throw FixValidationError("Price must contain at most one decimal point");
    }

    const std::string_view wholeText = text.substr(0, decimalPoint);
    const std::string_view fractionalText =
        decimalPoint == std::string_view::npos ? std::string_view{} : text.substr(decimalPoint + 1);
    if (wholeText.empty() || (decimalPoint != std::string_view::npos && fractionalText.empty())) {
        throw FixValidationError("Price must be a positive decimal");
    }

    if (configuration.tickSizeMantissa == 0 ||
        configuration.maxPrice.value() > std::numeric_limits<std::uint64_t>::max() / configuration.tickSizeMantissa) {
        throw std::logic_error("invalid instrument tick configuration");
    }

    const std::uint64_t maxScaledPrice = configuration.maxPrice.value() * configuration.tickSizeMantissa;
    const std::uint64_t scaleFactor = powerOfTen(configuration.priceScale);
    const std::uint64_t whole = parseUnsignedWhole(wholeText, maxScaledPrice / scaleFactor, "Price");

    std::uint64_t fractional = 0;
    const std::size_t retainedDigits =
        std::min(fractionalText.size(), static_cast<std::size_t>(configuration.priceScale));
    for (std::size_t index = 0; index < retainedDigits; ++index) {
        const char character = fractionalText[index];
        if (character < '0' || character > '9') {
            throw FixValidationError("Price must be an unsigned decimal");
        }
        fractional = fractional * 10 + static_cast<std::uint64_t>(character - '0');
    }
    for (std::size_t index = retainedDigits; index < configuration.priceScale; ++index) {
        fractional *= 10;
    }
    for (std::size_t index = retainedDigits; index < fractionalText.size(); ++index) {
        const char character = fractionalText[index];
        if (character < '0' || character > '9') {
            throw FixValidationError("Price must be an unsigned decimal");
        }
        if (character != '0') {
            throw FixValidationError("Price violates the configured tick size");
        }
    }

    const std::uint64_t scaledWhole = whole * scaleFactor;
    if (fractional > maxScaledPrice - scaledWhole) {
        throw FixValidationError("Price is outside the configured range");
    }
    const std::uint64_t scaledPrice = scaledWhole + fractional;
    if (scaledPrice == 0) {
        throw FixValidationError("Price is outside the configured range");
    }
    if (scaledPrice % configuration.tickSizeMantissa != 0) {
        throw FixValidationError("Price violates the configured tick size");
    }
    return domain::Price{scaledPrice / configuration.tickSizeMantissa};
}

domain::Quantity parseQuantityUnits(const std::string_view text,
                                    const instrument::InstrumentConfiguration& configuration) {
    const std::size_t decimalPoint = text.find('.');
    if (decimalPoint != std::string_view::npos && text.find('.', decimalPoint + 1) != std::string_view::npos) {
        throw FixValidationError("OrderQty must contain at most one decimal point");
    }

    const std::string_view wholeText = text.substr(0, decimalPoint);
    const std::string_view fractionalText =
        decimalPoint == std::string_view::npos ? std::string_view{} : text.substr(decimalPoint + 1);
    if (wholeText.empty() || (decimalPoint != std::string_view::npos && fractionalText.empty())) {
        throw FixValidationError("OrderQty must be a positive whole number");
    }
    for (const char character : fractionalText) {
        if (character != '0') {
            throw FixValidationError("OrderQty violates the configured lot size");
        }
    }

    const std::uint64_t quantity = parseUnsignedWhole(wholeText, configuration.maxOrderQuantity.value(), "OrderQty");
    if (quantity == 0 || configuration.lotSize.value() == 0 || quantity % configuration.lotSize.value() != 0) {
        throw FixValidationError("OrderQty violates the configured lot size");
    }
    return domain::Quantity{quantity};
}

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
    const auto secondsSinceEpoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
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
    const auto secondsSinceEpoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const auto currentUtcMidnightSeconds = (secondsSinceEpoch / secondsPerDay) * secondsPerDay;
    return std::chrono::system_clock::time_point{std::chrono::seconds(currentUtcMidnightSeconds + atcCloseSeconds)};
}

/*
 * Extracts TimeInForce and applies the exchange default.
 * Invariant: missing tag 59 is treated as DAY, matching common FIX behavior.
 *
 * Bad combinations found here:
 * - Unsupported TIF values are rejected before they can reach the sequencer.
 */
TimeInForce extractTimeInForce(const FIX::Message& fixMessage) {
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
std::chrono::system_clock::time_point parseExpireTime(const FIX::Message& fixMessage) {
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
std::chrono::system_clock::time_point validateExpiry(const FIX::Message& fixMessage, const TimeInForce tif) {
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
void validateNewOrderType(const FIX::Message& fixMessage) {
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
sequencer::orderType extractSide(const FIX::Message& fixMessage) {
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
 * Extracts quantity using the exact decimal lot model from instrument configuration.
 */
domain::Quantity extractOrderQty(const FIX::Message& fixMessage,
                                 const instrument::InstrumentConfiguration& configuration) {
    if (!fixMessage.isSetField(FIX::FIELD::OrderQty)) {
        throw FIX::FieldNotFound(FIX::FIELD::OrderQty);
    }
    return parseQuantityUnits(fixMessage.getField(FIX::FIELD::OrderQty), configuration);
}

/*
 * Extracts a positive limit price as an exact number of configured ticks.
 */
domain::Price extractLimitPrice(const FIX::Message& fixMessage,
                                const instrument::InstrumentConfiguration& configuration) {
    if (!fixMessage.isSetField(FIX::FIELD::Price)) {
        throw FIX::FieldNotFound(FIX::FIELD::Price);
    }
    return parsePriceTicks(fixMessage.getField(FIX::FIELD::Price), configuration);
}

/*
 * Cancel requests operate on an existing client order ID and must not carry
 * placement-only lifetime fields.
 * Bad combinations found here: Cancel + TimeInForce and Cancel + ExpireTime.
 */
void validateCancel(const FIX::Message& fixMessage) {
    if (fixMessage.isSetField(FIX::FIELD::TimeInForce) || fixMessage.isSetField(FIX::FIELD::ExpireTime)) {
        throw FixValidationError("cancel messages must not specify TimeInForce or ExpireTime");
    }
}

} /* namespace */

bool isProcessableMessageType(const std::string& msgType) {
    return msgType == "D" || msgType == "F";
}

sequencer::sequenceMessage parseFixMessage(const FIX::Message& fixMessage, const FIX::SessionID& sessionID,
                                           size_t numShards) {
    try {
        FIX::MsgType msgType;
        fixMessage.getHeader().getField(msgType);

        sequencer::sequenceMessage seqMsg{};
        seqMsg.port = std::hash<std::string>{}(sessionID.toString());
        seqMsg.clientId = domain::ClientId{seqMsg.port};

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

        /* Resolve the authoritative instrument configuration. */
        FIX::Symbol symbol;
        if (!fixMessage.isSetField(symbol)) {
            throw FIX::FieldNotFound(FIX::FIELD::Symbol);
        }
        fixMessage.getField(symbol);
        const instrument::InstrumentConfiguration* configuration = instrument::findBySymbol(symbol.getValue());
        if (configuration == nullptr) {
            throw FixValidationError("unknown or inactive instrument");
        }
        std::strncpy(seqMsg.symbol, symbol.getValue().c_str(), sizeof(seqMsg.symbol) - 1);
        seqMsg.symbol[sizeof(seqMsg.symbol) - 1] = '\0';
        seqMsg.instrumentId = configuration->instrumentId;
        seqMsg.configurationVersion = configuration->configurationVersion;

        /* Determine shard based on ticker hash */
        std::string ticker{seqMsg.symbol};
        seqMsg.shard_id = std::hash<std::string>{}(ticker) % numShards;

        /* Extract order quantity */
        if (seqMsg.type != sequencer::orderType::CANCEL) {
            seqMsg.quantity = extractOrderQty(fixMessage, *configuration);
        }

        /* Extract price as an exact number of configured ticks. */
        if (seqMsg.type != sequencer::orderType::CANCEL) {
            seqMsg.price = extractLimitPrice(fixMessage, *configuration);
        }

        /* Extract order ID */
        FIX::ClOrdID clOrdID;
        fixMessage.getField(clOrdID);
        seqMsg.clientCommandId.emplace(clOrdID.getValue());
        seqMsg.id = std::hash<std::string>{}(clOrdID.getValue());

        if (seqMsg.type != sequencer::orderType::CANCEL) {
            seqMsg.tif = extractTimeInForce(fixMessage);
            seqMsg.expiry = validateExpiry(fixMessage, seqMsg.tif);
        }

        /* Assign global order counter and timestamp */
        seqMsg.order = orderCounter.fetch_add(1, std::memory_order_seq_cst);
        seqMsg.timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();

        return seqMsg;

    } catch (const FIX::FieldNotFound& e) {
        std::cerr << "[FixParser] Required field missing: " << e.field << std::endl;
        throw;
    } catch (const std::exception& e) {
        std::cerr << "[FixParser] Error parsing FIX message: " << e.what() << std::endl;
        throw;
    }
}

} /* namespace exchange::core::fix */
