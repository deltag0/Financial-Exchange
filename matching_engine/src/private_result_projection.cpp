#include "private_result_projection.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace exchange::private_result {
namespace {

struct AffectedRecipients final {
    std::array<domain::ClientId, 2> clients{};
    std::size_t size{0};
};

struct RecipientPlan final {
    domain::ClientId clientId;
    std::size_t projectedEventCount;
};

domain::Side makerSide(const domain::Side takerSide) {
    switch (takerSide) {
        case domain::Side::BUY:
            return domain::Side::SELL;
        case domain::Side::SELL:
            return domain::Side::BUY;
    }
    throw std::logic_error("authoritative trade has invalid taker side");
}

std::size_t projectedEventIncrement(const domain::BusinessEvent& event, const domain::ClientId recipient) {
    return std::visit(
        [recipient](const auto& typedEvent) -> std::size_t {
            using Event = std::decay_t<decltype(typedEvent)>;
            if constexpr (std::is_same_v<Event, domain::Trade>) {
                return static_cast<std::size_t>(typedEvent.makerClientId == recipient) +
                       static_cast<std::size_t>(typedEvent.takerClientId == recipient);
            } else {
                return typedEvent.clientId == recipient ? 1U : 0U;
            }
        },
        event);
}

AffectedRecipients affectedRecipients(const domain::BusinessEvent& event) {
    return std::visit(
        [](const auto& typedEvent) {
            using Event = std::decay_t<decltype(typedEvent)>;
            if constexpr (std::is_same_v<Event, domain::Trade>) {
                AffectedRecipients recipients{
                    .clients = {typedEvent.makerClientId, typedEvent.takerClientId},
                    .size = 1,
                };
                if (typedEvent.takerClientId != typedEvent.makerClientId) {
                    recipients.size = 2;
                }
                return recipients;
            } else {
                return AffectedRecipients{
                    .clients = {typedEvent.clientId, domain::ClientId{}},
                    .size = 1,
                };
            }
        },
        event);
}

void appendProjectedEvent(PrivateResult& output, const domain::BusinessEvent& event, const domain::ClientId recipient) {
    std::visit(
        [&output, recipient](const auto& typedEvent) {
            using Event = std::decay_t<decltype(typedEvent)>;
            if constexpr (std::is_same_v<Event, domain::Trade>) {
                if (typedEvent.makerClientId == recipient) {
                    output.emplace_back(PrivateTrade{
                        .eventId = typedEvent.eventId,
                        .instrumentId = typedEvent.instrumentId,
                        .orderId = typedEvent.makerOrderId,
                        .side = makerSide(typedEvent.takerSide),
                        .role = TradeRole::MAKER,
                        .executionPrice = typedEvent.executionPrice,
                        .executionQuantity = typedEvent.executionQuantity,
                        .remainingQuantity = typedEvent.makerRemainingQuantity,
                    });
                }
                if (typedEvent.takerClientId == recipient) {
                    output.emplace_back(PrivateTrade{
                        .eventId = typedEvent.eventId,
                        .instrumentId = typedEvent.instrumentId,
                        .orderId = typedEvent.takerOrderId,
                        .side = typedEvent.takerSide,
                        .role = TradeRole::TAKER,
                        .executionPrice = typedEvent.executionPrice,
                        .executionQuantity = typedEvent.executionQuantity,
                        .remainingQuantity = typedEvent.takerRemainingQuantity,
                    });
                }
            } else if (typedEvent.clientId == recipient) {
                output.emplace_back(typedEvent);
            }
        },
        event);
}

RecipientResult makeRecipientResult(const matching_engine::CommandResultBatch& authoritativeResult,
                                    const domain::ClientId recipient, const std::size_t projectedEventCount) {
    PrivateResult privateResult;
    privateResult.reserve(projectedEventCount);
    return {
        .recipient = recipient,
        .correlation = recipient == authoritativeResult.correlation().clientId
                           ? std::optional{authoritativeResult.correlation()}
                           : std::nullopt,
        .privateResult = std::move(privateResult),
    };
}

} // namespace

std::optional<RecipientResult> projectPrivateResult(const matching_engine::CommandResultBatch& authoritativeResult,
                                                    const domain::ClientId recipient) {
    std::size_t projectedCount = 0;
    for (const auto& event : authoritativeResult.events()) {
        const std::size_t increment = projectedEventIncrement(event, recipient);
        if (increment > std::numeric_limits<std::size_t>::max() - projectedCount) {
            throw std::length_error("private result event count overflow");
        }
        projectedCount += increment;
    }

    if (projectedCount == 0) {
        return std::nullopt;
    }

    RecipientResult projected = makeRecipientResult(authoritativeResult, recipient, projectedCount);
    for (const auto& event : authoritativeResult.events()) {
        appendProjectedEvent(projected.privateResult, event, recipient);
    }
    return projected;
}

RecipientResults projectPrivateResults(const matching_engine::CommandResultBatch& authoritativeResult) {
    std::vector<RecipientPlan> plans;
    for (const auto& event : authoritativeResult.events()) {
        const AffectedRecipients recipients = affectedRecipients(event);
        for (std::size_t index = 0; index < recipients.size; ++index) {
            const domain::ClientId recipient = recipients.clients[index];
            const std::size_t eventCount = projectedEventIncrement(event, recipient);
            const auto existing = std::find_if(plans.begin(), plans.end(), [recipient](const RecipientPlan& plan) {
                return plan.clientId == recipient;
            });
            if (existing == plans.end()) {
                plans.push_back({.clientId = recipient, .projectedEventCount = eventCount});
            } else {
                if (eventCount > std::numeric_limits<std::size_t>::max() - existing->projectedEventCount) {
                    throw std::length_error("recipient private result event count overflow");
                }
                existing->projectedEventCount += eventCount;
            }
        }
    }

    RecipientResults projected;
    projected.reserve(plans.size());
    for (const auto& plan : plans) {
        projected.push_back(makeRecipientResult(authoritativeResult, plan.clientId, plan.projectedEventCount));
    }

    for (const auto& event : authoritativeResult.events()) {
        const AffectedRecipients recipients = affectedRecipients(event);
        for (std::size_t index = 0; index < recipients.size; ++index) {
            const domain::ClientId recipient = recipients.clients[index];
            const auto output = std::find_if(projected.begin(), projected.end(),
                                             [recipient](const auto& result) { return result.recipient == recipient; });
            if (output == projected.end()) {
                throw std::logic_error("recipient plan missing during private result projection");
            }
            appendProjectedEvent(output->privateResult, event, recipient);
        }
    }
    return projected;
}

} // namespace exchange::private_result
