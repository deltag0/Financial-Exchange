#include "application_event_json.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace exchange::application_event {
namespace {

void appendUnsigned(std::string& output, const std::uint64_t value) {
    std::array<char, 20> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    output.append(buffer.data(), result.ptr);
}

void appendQuotedUnsigned(std::string& output, const std::uint64_t value) {
    output.push_back('"');
    appendUnsigned(output, value);
    output.push_back('"');
}

void appendJsonString(std::string& output, const std::string_view value) {
    constexpr char HEX[] = "0123456789abcdef";
    output.push_back('"');
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
            case '"':
                output.append("\\\"");
                break;
            case '\\':
                output.append("\\\\");
                break;
            case '\b':
                output.append("\\b");
                break;
            case '\f':
                output.append("\\f");
                break;
            case '\n':
                output.append("\\n");
                break;
            case '\r':
                output.append("\\r");
                break;
            case '\t':
                output.append("\\t");
                break;
            default:
                if (byte < 0x20 || byte == 0x7f) {
                    output.append("\\u00");
                    output.push_back(HEX[byte >> 4]);
                    output.push_back(HEX[byte & 0x0f]);
                } else {
                    output.push_back(character);
                }
                break;
        }
    }
    output.push_back('"');
}

std::optional<std::string_view> enumName(const domain::Side value) noexcept {
    switch (value) {
        case domain::Side::BUY:
            return "BUY";
        case domain::Side::SELL:
            return "SELL";
    }
    return std::nullopt;
}

std::optional<std::string_view> enumName(const private_result::TradeRole value) noexcept {
    switch (value) {
        case private_result::TradeRole::MAKER:
            return "MAKER";
        case private_result::TradeRole::TAKER:
            return "TAKER";
    }
    return std::nullopt;
}

std::optional<std::string_view> enumName(const domain::CommandType value) noexcept {
    switch (value) {
        case domain::CommandType::NEW_ORDER:
            return "NEW_ORDER";
        case domain::CommandType::CANCEL:
            return "CANCEL";
    }
    return std::nullopt;
}

std::optional<std::string_view> enumName(const domain::CancelReason value) noexcept {
    switch (value) {
        case domain::CancelReason::CLIENT_REQUESTED:
            return "CLIENT_REQUESTED";
        case domain::CancelReason::IOC_REMAINDER:
            return "IOC_REMAINDER";
    }
    return std::nullopt;
}

std::optional<std::string_view> enumName(const domain::CommandRejectionReason value) noexcept {
    switch (value) {
        case domain::CommandRejectionReason::ORDER_NOT_ACTIVE:
            return "ORDER_NOT_ACTIVE";
        case domain::CommandRejectionReason::NOT_OWNER:
            return "NOT_OWNER";
        case domain::CommandRejectionReason::BOOK_CAPACITY_EXCEEDED:
            return "BOOK_CAPACITY_EXCEEDED";
    }
    return std::nullopt;
}

void appendEventId(std::string& output, const domain::EventId& eventId) {
    output.append("{\"exchangeRunId\":");
    appendQuotedUnsigned(output, eventId.exchangeRunId.value());
    output.append(",\"commandSequence\":");
    appendQuotedUnsigned(output, eventId.commandSequence.value());
    output.append(",\"eventIndex\":");
    appendUnsigned(output, eventId.eventIndex.value());
    output.push_back('}');
}

void appendPublicTrade(std::string& output, const market_data::PublicTrade& trade,
                       const std::string_view aggressorSide) {
    output.append("{\"eventId\":");
    appendEventId(output, trade.eventId);
    output.append(",\"instrumentId\":");
    appendQuotedUnsigned(output, trade.instrumentId.value());
    output.append(",\"executionPrice\":");
    appendQuotedUnsigned(output, trade.executionPrice.value());
    output.append(",\"executionQuantity\":");
    appendQuotedUnsigned(output, trade.executionQuantity.value());
    output.append(",\"aggressorSide\":");
    appendJsonString(output, aggressorSide);
    output.push_back('}');
}

void appendPrivateTrade(std::string& output, const private_result::PrivateTrade& trade, const std::string_view side,
                        const std::string_view role) {
    output.append("{\"type\":\"privateTrade\",\"eventId\":");
    appendEventId(output, trade.eventId);
    output.append(",\"instrumentId\":");
    appendQuotedUnsigned(output, trade.instrumentId.value());
    output.append(",\"orderId\":");
    appendQuotedUnsigned(output, trade.orderId.value());
    output.append(",\"side\":");
    appendJsonString(output, side);
    output.append(",\"role\":");
    appendJsonString(output, role);
    output.append(",\"executionPrice\":");
    appendQuotedUnsigned(output, trade.executionPrice.value());
    output.append(",\"executionQuantity\":");
    appendQuotedUnsigned(output, trade.executionQuantity.value());
    output.append(",\"remainingQuantity\":");
    appendQuotedUnsigned(output, trade.remainingQuantity.value());
    output.push_back('}');
}

void appendOrderRested(std::string& output, const domain::OrderRested& rested, const std::string_view side) {
    output.append("{\"type\":\"orderRested\",\"eventId\":");
    appendEventId(output, rested.eventId);
    output.append(",\"orderId\":");
    appendQuotedUnsigned(output, rested.orderId.value());
    output.append(",\"clientId\":");
    appendQuotedUnsigned(output, rested.clientId.value());
    output.append(",\"instrumentId\":");
    appendQuotedUnsigned(output, rested.instrumentId.value());
    output.append(",\"side\":");
    appendJsonString(output, side);
    output.append(",\"price\":");
    appendQuotedUnsigned(output, rested.price.value());
    output.append(",\"remainingQuantity\":");
    appendQuotedUnsigned(output, rested.remainingQuantity.value());
    output.push_back('}');
}

void appendOrderCancelled(std::string& output, const domain::OrderCancelled& cancelled, const std::string_view reason) {
    output.append("{\"type\":\"orderCancelled\",\"eventId\":");
    appendEventId(output, cancelled.eventId);
    output.append(",\"orderId\":");
    appendQuotedUnsigned(output, cancelled.orderId.value());
    output.append(",\"clientId\":");
    appendQuotedUnsigned(output, cancelled.clientId.value());
    output.append(",\"instrumentId\":");
    appendQuotedUnsigned(output, cancelled.instrumentId.value());
    output.append(",\"cancelledQuantity\":");
    appendQuotedUnsigned(output, cancelled.cancelledQuantity.value());
    output.append(",\"reason\":");
    appendJsonString(output, reason);
    output.push_back('}');
}

void appendCommandRejected(std::string& output, const domain::CommandRejected& rejected,
                           const std::string_view commandType, const std::string_view reason) {
    output.append("{\"type\":\"commandRejected\",\"eventId\":");
    appendEventId(output, rejected.eventId);
    output.append(",\"commandType\":");
    appendJsonString(output, commandType);
    output.append(",\"clientId\":");
    appendQuotedUnsigned(output, rejected.clientId.value());
    output.append(",\"clientCommandId\":");
    appendJsonString(output, rejected.clientCommandId.value());
    if (rejected.relevantOrderId.has_value()) {
        output.append(",\"relevantOrderId\":");
        appendQuotedUnsigned(output, rejected.relevantOrderId->value());
    }
    output.append(",\"reason\":");
    appendJsonString(output, reason);
    output.push_back('}');
}

ApplicationEventJsonError appendPrivateEvent(std::string& output, const private_result::PrivateEvent& event,
                                             const domain::ExchangeRunId exchangeRunId,
                                             std::optional<domain::CommandSequence>& commandSequence,
                                             const domain::ClientId recipient) {
    return std::visit(
        [&output, exchangeRunId, &commandSequence, recipient](const auto& typedEvent) {
            if (typedEvent.eventId.exchangeRunId != exchangeRunId) {
                return ApplicationEventJsonError::RUN_MISMATCH;
            }
            if (!commandSequence.has_value()) {
                commandSequence = typedEvent.eventId.commandSequence;
            } else if (typedEvent.eventId.commandSequence != *commandSequence) {
                return ApplicationEventJsonError::COMMAND_SEQUENCE_MISMATCH;
            }

            using Event = std::decay_t<decltype(typedEvent)>;
            if constexpr (std::is_same_v<Event, private_result::PrivateTrade>) {
                const auto side = enumName(typedEvent.side);
                const auto role = enumName(typedEvent.role);
                if (!side.has_value() || !role.has_value()) {
                    return ApplicationEventJsonError::INVALID_ENUM;
                }
                appendPrivateTrade(output, typedEvent, *side, *role);
            } else if constexpr (std::is_same_v<Event, domain::OrderRested>) {
                if (typedEvent.clientId != recipient) {
                    return ApplicationEventJsonError::RECIPIENT_MISMATCH;
                }
                const auto side = enumName(typedEvent.side);
                if (!side.has_value()) {
                    return ApplicationEventJsonError::INVALID_ENUM;
                }
                appendOrderRested(output, typedEvent, *side);
            } else if constexpr (std::is_same_v<Event, domain::OrderCancelled>) {
                if (typedEvent.clientId != recipient) {
                    return ApplicationEventJsonError::RECIPIENT_MISMATCH;
                }
                const auto reason = enumName(typedEvent.reason);
                if (!reason.has_value()) {
                    return ApplicationEventJsonError::INVALID_ENUM;
                }
                appendOrderCancelled(output, typedEvent, *reason);
            } else if constexpr (std::is_same_v<Event, domain::CommandRejected>) {
                if (typedEvent.clientId != recipient) {
                    return ApplicationEventJsonError::RECIPIENT_MISMATCH;
                }
                const auto commandType = enumName(typedEvent.commandType);
                const auto reason = enumName(typedEvent.reason);
                if (!commandType.has_value() || !reason.has_value()) {
                    return ApplicationEventJsonError::INVALID_ENUM;
                }
                appendCommandRejected(output, typedEvent, *commandType, *reason);
            }
            return ApplicationEventJsonError::NONE;
        },
        event);
}

} // namespace

ApplicationEventJsonError serializePublicTradesV1(const domain::ExchangeRunId exchangeRunId,
                                                  const std::vector<market_data::PublicTrade>& trades,
                                                  std::string& output) {
    if (trades.empty()) {
        return ApplicationEventJsonError::EMPTY_EVENTS;
    }

    std::string serialized;
    serialized.append("{\"schemaVersion\":1,\"type\":\"publicTrades\",\"exchangeRunId\":");
    appendQuotedUnsigned(serialized, exchangeRunId.value());
    serialized.append(",\"trades\":[");
    const auto commandSequence = trades.front().eventId.commandSequence;
    for (std::size_t index = 0; index < trades.size(); ++index) {
        const auto& trade = trades[index];
        if (trade.eventId.exchangeRunId != exchangeRunId) {
            return ApplicationEventJsonError::RUN_MISMATCH;
        }
        if (trade.eventId.commandSequence != commandSequence) {
            return ApplicationEventJsonError::COMMAND_SEQUENCE_MISMATCH;
        }
        const auto aggressorSide = enumName(trade.aggressorSide);
        if (!aggressorSide.has_value()) {
            return ApplicationEventJsonError::INVALID_ENUM;
        }
        if (index != 0) {
            serialized.push_back(',');
        }
        appendPublicTrade(serialized, trade, *aggressorSide);
    }
    serialized.append("]}");
    output = std::move(serialized);
    return ApplicationEventJsonError::NONE;
}

ApplicationEventJsonError serializePrivateResultV1(const domain::ExchangeRunId exchangeRunId,
                                                   const private_result::RecipientResult& result, std::string& output) {
    if (result.privateResult.empty()) {
        return ApplicationEventJsonError::EMPTY_EVENTS;
    }

    std::string serialized;
    serialized.append("{\"schemaVersion\":1,\"type\":\"privateResult\",\"exchangeRunId\":");
    appendQuotedUnsigned(serialized, exchangeRunId.value());
    serialized.append(",\"recipientClientId\":");
    appendQuotedUnsigned(serialized, result.recipient.value());
    if (result.correlation.has_value()) {
        serialized.append(",\"correlation\":{\"exchangeRunId\":");
        appendQuotedUnsigned(serialized, result.correlation->exchangeRunId.value());
        serialized.append(",\"clientId\":");
        appendQuotedUnsigned(serialized, result.correlation->clientId.value());
        serialized.append(",\"clientCommandId\":");
        appendJsonString(serialized, result.correlation->clientCommandId.value());
        serialized.append(",\"commandSequence\":");
        appendQuotedUnsigned(serialized, result.correlation->commandSequence.value());
        serialized.push_back('}');
    }
    serialized.append(",\"events\":[");
    std::optional<domain::CommandSequence> commandSequence;
    for (std::size_t index = 0; index < result.privateResult.size(); ++index) {
        if (index != 0) {
            serialized.push_back(',');
        }
        const auto eventError = appendPrivateEvent(serialized, result.privateResult[index], exchangeRunId,
                                                   commandSequence, result.recipient);
        if (eventError != ApplicationEventJsonError::NONE) {
            return eventError;
        }
    }
    if (result.correlation.has_value()) {
        if (result.correlation->exchangeRunId != exchangeRunId ||
            result.correlation->commandSequence != *commandSequence) {
            return ApplicationEventJsonError::CORRELATION_MISMATCH;
        }
        if (result.correlation->clientId != result.recipient) {
            return ApplicationEventJsonError::RECIPIENT_MISMATCH;
        }
    }
    serialized.append("]}");
    output = std::move(serialized);
    return ApplicationEventJsonError::NONE;
}

} // namespace exchange::application_event
