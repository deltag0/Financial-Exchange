#include "application_event_gateway.hpp"

#include "exchange_run_controller.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <variant>

namespace exchange::application_event {
namespace {

template <std::size_t Size>
consteval std::size_t literalBytes(const char (&)[Size]) noexcept {
    return Size - 1;
}

consteval std::size_t checkedBytesAdd(const std::size_t left, const std::size_t right) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        throw "Application gateway outbound capacity V1 overflow";
    }
    return left + right;
}

template <typename... Values>
consteval std::size_t checkedBytesSum(const Values... values) {
    std::size_t total = 0;
    ((total = checkedBytesAdd(total, static_cast<std::size_t>(values))), ...);
    return total;
}

consteval std::size_t checkedBytesMultiply(const std::size_t left, const std::size_t right) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        throw "Application gateway outbound capacity V1 overflow";
    }
    return left * right;
}

constexpr std::size_t MAX_AUTHORITATIVE_EVENTS_PER_COMMAND = 4'096;
constexpr std::size_t MAX_PRIVATE_EVENTS_PER_COMMAND = checkedBytesMultiply(2, MAX_AUTHORITATIVE_EVENTS_PER_COMMAND);
constexpr std::size_t QUOTED_UINT64_BYTES = 22;
constexpr std::size_t CONTROLLER_EVENT_INDEX_BYTES = 4;
constexpr std::size_t MAX_ESCAPED_CLIENT_COMMAND_ID_BYTES =
    checkedBytesSum(2, checkedBytesMultiply(6, domain::ClientCommandId::MAX_LENGTH));
constexpr std::size_t EVENT_ID_BYTES = checkedBytesSum(
    literalBytes("{\"exchangeRunId\":"), QUOTED_UINT64_BYTES, literalBytes(",\"commandSequence\":"),
    QUOTED_UINT64_BYTES, literalBytes(",\"eventIndex\":"), CONTROLLER_EVENT_INDEX_BYTES, literalBytes("}"));
constexpr std::size_t PRIVATE_TRADE_BYTES = checkedBytesSum(
    literalBytes("{\"type\":\"privateTrade\",\"eventId\":"), EVENT_ID_BYTES, literalBytes(",\"instrumentId\":"),
    QUOTED_UINT64_BYTES, literalBytes(",\"orderId\":"), QUOTED_UINT64_BYTES, literalBytes(",\"side\":"),
    literalBytes("\"SELL\""), literalBytes(",\"role\":"), literalBytes("\"MAKER\""),
    literalBytes(",\"executionPrice\":"), QUOTED_UINT64_BYTES, literalBytes(",\"executionQuantity\":"),
    QUOTED_UINT64_BYTES, literalBytes(",\"remainingQuantity\":"), QUOTED_UINT64_BYTES, literalBytes("}"));
constexpr std::size_t ORDER_RESTED_BYTES = checkedBytesSum(
    literalBytes("{\"type\":\"orderRested\",\"eventId\":"), EVENT_ID_BYTES, literalBytes(",\"orderId\":"),
    QUOTED_UINT64_BYTES, literalBytes(",\"clientId\":"), QUOTED_UINT64_BYTES, literalBytes(",\"instrumentId\":"),
    QUOTED_UINT64_BYTES, literalBytes(",\"side\":"), literalBytes("\"SELL\""), literalBytes(",\"price\":"),
    QUOTED_UINT64_BYTES, literalBytes(",\"remainingQuantity\":"), QUOTED_UINT64_BYTES, literalBytes("}"));
constexpr std::size_t ORDER_CANCELLED_BYTES = checkedBytesSum(
    literalBytes("{\"type\":\"orderCancelled\",\"eventId\":"), EVENT_ID_BYTES, literalBytes(",\"orderId\":"),
    QUOTED_UINT64_BYTES, literalBytes(",\"clientId\":"), QUOTED_UINT64_BYTES, literalBytes(",\"instrumentId\":"),
    QUOTED_UINT64_BYTES, literalBytes(",\"cancelledQuantity\":"), QUOTED_UINT64_BYTES, literalBytes(",\"reason\":"),
    literalBytes("\"CLIENT_REQUESTED\""), literalBytes("}"));
constexpr std::size_t COMMAND_REJECTED_BYTES = checkedBytesSum(
    literalBytes("{\"type\":\"commandRejected\",\"eventId\":"), EVENT_ID_BYTES, literalBytes(",\"commandType\":"),
    literalBytes("\"NEW_ORDER\""), literalBytes(",\"clientId\":"), QUOTED_UINT64_BYTES,
    literalBytes(",\"clientCommandId\":"), MAX_ESCAPED_CLIENT_COMMAND_ID_BYTES, literalBytes(",\"relevantOrderId\":"),
    QUOTED_UINT64_BYTES, literalBytes(",\"reason\":"), literalBytes("\"BOOK_CAPACITY_EXCEEDED\""), literalBytes("}"));
constexpr std::size_t TWO_PRIVATE_TRADES_BYTES = checkedBytesSum(checkedBytesMultiply(2, PRIVATE_TRADE_BYTES), 1);
static_assert(ORDER_RESTED_BYTES <= TWO_PRIVATE_TRADES_BYTES);
static_assert(ORDER_CANCELLED_BYTES <= TWO_PRIVATE_TRADES_BYTES);
static_assert(COMMAND_REJECTED_BYTES <= TWO_PRIVATE_TRADES_BYTES);
constexpr std::size_t PRIVATE_ENVELOPE_FIXED_BYTES = checkedBytesSum(
    literalBytes("{\"schemaVersion\":1,\"type\":\"privateResult\",\"exchangeRunId\":"), QUOTED_UINT64_BYTES,
    literalBytes(",\"recipientClientId\":"), QUOTED_UINT64_BYTES, literalBytes(",\"correlation\":{\"exchangeRunId\":"),
    QUOTED_UINT64_BYTES, literalBytes(",\"clientId\":"), QUOTED_UINT64_BYTES, literalBytes(",\"clientCommandId\":"),
    MAX_ESCAPED_CLIENT_COMMAND_ID_BYTES, literalBytes(",\"commandSequence\":"), QUOTED_UINT64_BYTES, literalBytes("}"),
    literalBytes(",\"events\":["), literalBytes("]}"));
constexpr std::size_t MAX_PRIVATE_HANDOFF_MESSAGE_BYTES = checkedBytesSum(
    PRIVATE_ENVELOPE_FIXED_BYTES, checkedBytesMultiply(MAX_PRIVATE_EVENTS_PER_COMMAND, PRIVATE_TRADE_BYTES),
    MAX_PRIVATE_EVENTS_PER_COMMAND - 1);
constexpr std::size_t PUBLIC_TRADE_BYTES = checkedBytesSum(
    literalBytes("{\"eventId\":"), EVENT_ID_BYTES, literalBytes(",\"instrumentId\":"), QUOTED_UINT64_BYTES,
    literalBytes(",\"executionPrice\":"), QUOTED_UINT64_BYTES, literalBytes(",\"executionQuantity\":"),
    QUOTED_UINT64_BYTES, literalBytes(",\"aggressorSide\":"), literalBytes("\"SELL\""), literalBytes("}"));
constexpr std::size_t PUBLIC_ENVELOPE_FIXED_BYTES =
    checkedBytesSum(literalBytes("{\"schemaVersion\":1,\"type\":\"publicTrades\",\"exchangeRunId\":"),
                    QUOTED_UINT64_BYTES, literalBytes(",\"trades\":["), literalBytes("]}"));
constexpr std::size_t MAX_PUBLIC_HANDOFF_MESSAGE_BYTES = checkedBytesSum(
    PUBLIC_ENVELOPE_FIXED_BYTES, checkedBytesMultiply(MAX_AUTHORITATIVE_EVENTS_PER_COMMAND, PUBLIC_TRADE_BYTES),
    MAX_AUTHORITATIVE_EVENTS_PER_COMMAND - 1);
constexpr std::size_t MIN_OUTBOUND_CAPACITY = MAX_PRIVATE_HANDOFF_MESSAGE_BYTES > MAX_PUBLIC_HANDOFF_MESSAGE_BYTES
                                                  ? MAX_PRIVATE_HANDOFF_MESSAGE_BYTES
                                                  : MAX_PUBLIC_HANDOFF_MESSAGE_BYTES;

enum class EnqueueCapacity : std::uint8_t {
    FITS,
    SLOW_CONSUMER,
    INVARIANT_FAILURE,
};

EnqueueCapacity enqueueCapacity(const std::size_t queuedBytes, const std::size_t messageBytes,
                                const std::size_t capacity) noexcept {
    if (queuedBytes > capacity) {
        return EnqueueCapacity::INVARIANT_FAILURE;
    }
    if (messageBytes > capacity - queuedBytes) {
        return EnqueueCapacity::SLOW_CONSUMER;
    }
    return EnqueueCapacity::FITS;
}

domain::EventId privateEventId(const private_result::PrivateEvent& event) {
    return std::visit([](const auto& typedEvent) { return typedEvent.eventId; }, event);
}

} // namespace

const std::size_t MIN_APPLICATION_GATEWAY_OUTBOUND_CAPACITY_V1 = MIN_OUTBOUND_CAPACITY;

ApplicationEventGateway::ApplicationEventGateway(core::ExchangeRunController& controller,
                                                 const std::size_t maximumConnections,
                                                 const std::size_t perConnectionOutboundByteCapacity)
    : controller_(controller),
      maximumConnections_(maximumConnections),
      perConnectionOutboundByteCapacity_(perConnectionOutboundByteCapacity) {
    if (maximumConnections == 0) {
        throw std::invalid_argument("application event gateway maximum connections must be positive");
    }
    if (perConnectionOutboundByteCapacity < MIN_APPLICATION_GATEWAY_OUTBOUND_CAPACITY_V1) {
        throw std::invalid_argument(
            "application event gateway outbound byte capacity cannot hold one maximum controller handoff");
    }
}

ConnectionRegistrationResult ApplicationEventGateway::registerConnection(const ConnectionHandle handle,
                                                                         const domain::ClientId clientId,
                                                                         const domain::ExchangeRunId exchangeRunId) {
    const auto* admission = controller_.admissionIndex();
    if (clientId.value() == 0 || exchangeRunId.value() == 0 || admission == nullptr ||
        admission->exchangeRunId() != exchangeRunId) {
        return {.outcome = ConnectionRegistrationOutcome::INVALID_BINDING};
    }
    if (findConnection(handle) != connections_.end()) {
        return {.outcome = ConnectionRegistrationOutcome::HANDLE_ALREADY_REGISTERED};
    }

    const auto replaced = std::find_if(
        connections_.begin(), connections_.end(), [clientId, exchangeRunId](const ConnectionState& connection) {
            return connection.clientId == clientId && connection.exchangeRunId == exchangeRunId;
        });
    if (replaced == connections_.end() && connections_.size() >= maximumConnections_) {
        return {.outcome = ConnectionRegistrationOutcome::CONNECTION_LIMIT_REACHED};
    }

    if (replaced != connections_.end()) {
        const auto replacedHandle = replaced->handle;
        *replaced = {.handle = handle, .clientId = clientId, .exchangeRunId = exchangeRunId};
        return {.outcome = ConnectionRegistrationOutcome::REPLACED, .replacedConnection = replacedHandle};
    }
    connections_.push_back({.handle = handle, .clientId = clientId, .exchangeRunId = exchangeRunId});
    return {.outcome = ConnectionRegistrationOutcome::REGISTERED};
}

bool ApplicationEventGateway::disconnect(const ConnectionHandle handle) noexcept {
    const auto connection = findConnection(handle);
    if (connection == connections_.end()) {
        return false;
    }
    connections_.erase(connection);
    return true;
}

std::optional<ConnectionInspection> ApplicationEventGateway::inspectConnection(
    const ConnectionHandle handle) const noexcept {
    const auto connection = findConnection(handle);
    if (connection == connections_.end()) {
        return std::nullopt;
    }
    return ConnectionInspection{.clientId = connection->clientId,
                                .exchangeRunId = connection->exchangeRunId,
                                .queuedMessageCount = connection->outbound.size(),
                                .queuedBytes = connection->queuedBytes};
}

bool ApplicationEventGateway::tryPopOutbound(const ConnectionHandle handle, ImmutableApplicationMessage& message) {
    const auto connection = findConnection(handle);
    if (connection == connections_.end() || connection->outbound.empty()) {
        return false;
    }
    const auto& next = connection->outbound.front();
    if (next == nullptr || next->size() > connection->queuedBytes) {
        throw std::logic_error("application event gateway outbound byte accounting invariant failed");
    }
    connection->queuedBytes -= next->size();
    message = std::move(connection->outbound.front());
    connection->outbound.pop_front();
    return true;
}

ApplicationEventAdvanceOutcome ApplicationEventGateway::advancePublic() {
    if (publicFailure_.has_value()) {
        return ApplicationEventAdvanceOutcome::FAILED;
    }
    if (!pendingPublic_.has_value()) {
        std::vector<market_data::PublicTrade> batch;
        if (!controller_.tryPopPublicTradeBatchV1(batch)) {
            return ApplicationEventAdvanceOutcome::IDLE;
        }
        pendingPublic_ = std::move(batch);
    }

    if (pendingPublic_->empty()) {
        return failPublic(ApplicationEventInputFailureReason::SERIALIZATION_FAILED,
                          ApplicationEventJsonError::EMPTY_EVENTS);
    }
    const domain::ExchangeRunId exchangeRunId = pendingPublic_->front().eventId.exchangeRunId;
    std::string serialized;
    const auto serialization = serializePublicTradesV1(exchangeRunId, *pendingPublic_, serialized);
    if (serialization != ApplicationEventJsonError::NONE) {
        return failPublic(ApplicationEventInputFailureReason::SERIALIZATION_FAILED, serialization);
    }
    const auto payload = std::make_shared<const std::string>(std::move(serialized));

    for (auto connection = connections_.begin(); connection != connections_.end();) {
        if (connection->exchangeRunId != exchangeRunId) {
            ++connection;
            continue;
        }
        switch (enqueueCapacity(connection->queuedBytes, payload->size(), perConnectionOutboundByteCapacity_)) {
            case EnqueueCapacity::FITS:
                connection->outbound.push_back(payload);
                connection->queuedBytes += payload->size();
                ++connection;
                break;
            case EnqueueCapacity::SLOW_CONSUMER:
                connection = connections_.erase(connection);
                break;
            case EnqueueCapacity::INVARIANT_FAILURE:
                return failPublic(ApplicationEventInputFailureReason::BYTE_ACCOUNTING_INVARIANT);
        }
    }
    pendingPublic_.reset();
    return ApplicationEventAdvanceOutcome::ROUTED;
}

ApplicationEventAdvanceOutcome ApplicationEventGateway::advancePrivate() {
    if (privateFailure_.has_value()) {
        return ApplicationEventAdvanceOutcome::FAILED;
    }
    if (!pendingPrivate_.has_value()) {
        private_result::RecipientResults batch;
        if (!controller_.tryPopPrivateResultBatchV1(batch)) {
            return ApplicationEventAdvanceOutcome::IDLE;
        }
        pendingPrivate_ = std::move(batch);
    }

    struct SerializedRecipient final {
        domain::ClientId recipient;
        domain::ExchangeRunId exchangeRunId;
        ImmutableApplicationMessage payload;
    };
    std::vector<SerializedRecipient> serializedRecipients;
    serializedRecipients.reserve(pendingPrivate_->size());
    std::unordered_set<std::uint64_t> recipients;
    recipients.reserve(pendingPrivate_->size());
    std::optional<domain::ExchangeRunId> batchRunId;
    std::optional<domain::CommandSequence> batchCommandSequence;

    if (pendingPrivate_->empty()) {
        return failPrivate(ApplicationEventInputFailureReason::RECIPIENT_INVARIANT);
    }

    for (const auto& recipient : *pendingPrivate_) {
        if (recipient.recipient.value() == 0 || recipient.privateResult.empty() ||
            !recipients.insert(recipient.recipient.value()).second) {
            return failPrivate(ApplicationEventInputFailureReason::RECIPIENT_INVARIANT);
        }
        const domain::EventId identity = privateEventId(recipient.privateResult.front());
        if (!batchRunId.has_value()) {
            batchRunId = identity.exchangeRunId;
            batchCommandSequence = identity.commandSequence;
        } else if (identity.exchangeRunId != *batchRunId || identity.commandSequence != *batchCommandSequence) {
            return failPrivate(ApplicationEventInputFailureReason::RUN_IDENTITY_INVARIANT);
        }

        std::string serialized;
        const auto serialization = serializePrivateResultV1(identity.exchangeRunId, recipient, serialized);
        if (serialization != ApplicationEventJsonError::NONE) {
            return failPrivate(ApplicationEventInputFailureReason::SERIALIZATION_FAILED, serialization);
        }
        serializedRecipients.push_back({.recipient = recipient.recipient,
                                        .exchangeRunId = identity.exchangeRunId,
                                        .payload = std::make_shared<const std::string>(std::move(serialized))});
    }

    for (const auto& serialized : serializedRecipients) {
        auto connection =
            std::find_if(connections_.begin(), connections_.end(), [&serialized](const ConnectionState& candidate) {
                return candidate.clientId == serialized.recipient &&
                       candidate.exchangeRunId == serialized.exchangeRunId;
            });
        if (connection == connections_.end()) {
            continue;
        }
        switch (
            enqueueCapacity(connection->queuedBytes, serialized.payload->size(), perConnectionOutboundByteCapacity_)) {
            case EnqueueCapacity::FITS:
                connection->outbound.push_back(serialized.payload);
                connection->queuedBytes += serialized.payload->size();
                break;
            case EnqueueCapacity::SLOW_CONSUMER:
                connections_.erase(connection);
                break;
            case EnqueueCapacity::INVARIANT_FAILURE:
                return failPrivate(ApplicationEventInputFailureReason::BYTE_ACCOUNTING_INVARIANT);
        }
    }
    pendingPrivate_.reset();
    return ApplicationEventAdvanceOutcome::ROUTED;
}

ApplicationEventGatewayInspection ApplicationEventGateway::inspect() const noexcept {
    const auto inputInspection = [](const std::optional<ApplicationEventInputFailure>& failure,
                                    const std::size_t retainedValueCount) {
        return ApplicationEventInputInspection{.failure = failure, .retainedValueCount = retainedValueCount};
    };
    return {
        .connectionCount = connections_.size(),
        .publicInput = inputInspection(publicFailure_, pendingPublic_.has_value() ? pendingPublic_->size() : 0),
        .privateInput = inputInspection(privateFailure_, pendingPrivate_.has_value() ? pendingPrivate_->size() : 0)};
}

std::vector<ApplicationEventGateway::ConnectionState>::iterator ApplicationEventGateway::findConnection(
    const ConnectionHandle handle) noexcept {
    return std::find_if(connections_.begin(), connections_.end(),
                        [handle](const ConnectionState& connection) { return connection.handle == handle; });
}

std::vector<ApplicationEventGateway::ConnectionState>::const_iterator ApplicationEventGateway::findConnection(
    const ConnectionHandle handle) const noexcept {
    return std::find_if(connections_.begin(), connections_.end(),
                        [handle](const ConnectionState& connection) { return connection.handle == handle; });
}

ApplicationEventAdvanceOutcome ApplicationEventGateway::failPublic(
    const ApplicationEventInputFailureReason failure,
    const std::optional<ApplicationEventJsonError> serializationError) noexcept {
    publicFailure_ = ApplicationEventInputFailure{.reason = failure, .serializationError = serializationError};
    return ApplicationEventAdvanceOutcome::FAILED;
}

ApplicationEventAdvanceOutcome ApplicationEventGateway::failPrivate(
    const ApplicationEventInputFailureReason failure,
    const std::optional<ApplicationEventJsonError> serializationError) noexcept {
    privateFailure_ = ApplicationEventInputFailure{.reason = failure, .serializationError = serializationError};
    return ApplicationEventAdvanceOutcome::FAILED;
}

} // namespace exchange::application_event
