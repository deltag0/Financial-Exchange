#pragma once

#include "application_event_json.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace exchange::core {
class ExchangeRunController;
}

namespace exchange::application_event {

// Safe for one valid controller-produced V1 public or participant-private handoff: at most 4,096 authoritative
// events, the private projection's two-event self-trade expansion, valid controller identities and event indexes,
// maximum-width encoded fields, and worst-case ClientCommandId escaping.
extern const std::size_t MIN_APPLICATION_GATEWAY_OUTBOUND_CAPACITY_V1;

struct ConnectionHandle final {
    std::uint64_t value{};

    auto operator<=>(const ConnectionHandle&) const = default;
};

enum class ConnectionRegistrationOutcome : std::uint8_t {
    REGISTERED,
    REPLACED,
    CONNECTION_LIMIT_REACHED,
    INVALID_BINDING,
    HANDLE_ALREADY_REGISTERED,
};

struct ConnectionRegistrationResult final {
    ConnectionRegistrationOutcome outcome{ConnectionRegistrationOutcome::INVALID_BINDING};
    std::optional<ConnectionHandle> replacedConnection{};
};

struct ConnectionInspection final {
    domain::ClientId clientId;
    domain::ExchangeRunId exchangeRunId;
    std::size_t queuedMessageCount;
    std::size_t queuedBytes;
};

using ImmutableApplicationMessage = std::shared_ptr<const std::string>;

enum class ApplicationEventInputFailureReason : std::uint8_t {
    SERIALIZATION_FAILED,
    RECIPIENT_INVARIANT,
    RUN_IDENTITY_INVARIANT,
    BYTE_ACCOUNTING_INVARIANT,
};

struct ApplicationEventInputFailure final {
    ApplicationEventInputFailureReason reason;
    std::optional<ApplicationEventJsonError> serializationError{};
};

struct ApplicationEventInputInspection final {
    std::optional<ApplicationEventInputFailure> failure{};
    std::size_t retainedValueCount{0};
};

struct ApplicationEventGatewayInspection final {
    std::size_t connectionCount{0};
    ApplicationEventInputInspection publicInput{};
    ApplicationEventInputInspection privateInput{};
};

enum class ApplicationEventAdvanceOutcome : std::uint8_t {
    IDLE,
    ROUTED,
    FAILED,
};

class ApplicationEventGateway final {
public:
    ApplicationEventGateway(core::ExchangeRunController& controller, std::size_t maximumConnections,
                            std::size_t perConnectionOutboundByteCapacity);

    ApplicationEventGateway(const ApplicationEventGateway&) = delete;
    ApplicationEventGateway& operator=(const ApplicationEventGateway&) = delete;
    ApplicationEventGateway(ApplicationEventGateway&&) = delete;
    ApplicationEventGateway& operator=(ApplicationEventGateway&&) = delete;

    [[nodiscard]] ConnectionRegistrationResult registerConnection(ConnectionHandle handle, domain::ClientId clientId,
                                                                  domain::ExchangeRunId exchangeRunId);
    [[nodiscard]] bool disconnect(ConnectionHandle handle) noexcept;
    [[nodiscard]] std::optional<ConnectionInspection> inspectConnection(ConnectionHandle handle) const noexcept;
    [[nodiscard]] bool tryPopOutbound(ConnectionHandle handle, ImmutableApplicationMessage& message);

    [[nodiscard]] ApplicationEventAdvanceOutcome advancePublic();
    [[nodiscard]] ApplicationEventAdvanceOutcome advancePrivate();
    [[nodiscard]] ApplicationEventGatewayInspection inspect() const noexcept;

private:
    struct ConnectionState final {
        ConnectionHandle handle;
        domain::ClientId clientId;
        domain::ExchangeRunId exchangeRunId;
        std::deque<ImmutableApplicationMessage> outbound;
        std::size_t queuedBytes{0};
    };

    [[nodiscard]] std::vector<ConnectionState>::iterator findConnection(ConnectionHandle handle) noexcept;
    [[nodiscard]] std::vector<ConnectionState>::const_iterator findConnection(ConnectionHandle handle) const noexcept;
    [[nodiscard]] ApplicationEventAdvanceOutcome failPublic(
        ApplicationEventInputFailureReason failure,
        std::optional<ApplicationEventJsonError> serializationError = std::nullopt) noexcept;
    [[nodiscard]] ApplicationEventAdvanceOutcome failPrivate(
        ApplicationEventInputFailureReason failure,
        std::optional<ApplicationEventJsonError> serializationError = std::nullopt) noexcept;

    core::ExchangeRunController& controller_;
    const std::size_t maximumConnections_;
    const std::size_t perConnectionOutboundByteCapacity_;
    std::vector<ConnectionState> connections_;
    std::optional<std::vector<market_data::PublicTrade>> pendingPublic_{};
    std::optional<private_result::RecipientResults> pendingPrivate_{};
    std::optional<ApplicationEventInputFailure> publicFailure_{};
    std::optional<ApplicationEventInputFailure> privateFailure_{};
};

} // namespace exchange::application_event
