#pragma once

#include "../../core/domain/include/business_events.hpp"
#include "../../core/domain/include/command_result_fwd.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

namespace exchange::matching_engine {

enum class ProcessingResult {
    APPLIED,
    BOOK_CAPACITY_EXCEEDED,
};

class CommandResultBatch final {
public:
    CommandResultBatch(domain::CommandResultCorrelation correlation, const ProcessingResult result,
                       std::vector<domain::BusinessEvent> events)
        : correlation_(std::move(correlation)), result_(result), events_(std::move(events)) {
        for (const domain::BusinessEvent& event : events_) {
            const domain::CommandSequence eventSequence =
                std::visit([](const auto& typedEvent) { return typedEvent.eventId.commandSequence; }, event);
            const domain::ExchangeRunId eventRunId =
                std::visit([](const auto& typedEvent) { return typedEvent.eventId.exchangeRunId; }, event);
            if (eventSequence != correlation_.commandSequence || eventRunId != correlation_.exchangeRunId) {
                throw std::invalid_argument("command-result event identity does not match batch correlation");
            }
        }
    }

    CommandResultBatch(const CommandResultBatch&) = default;
    CommandResultBatch(CommandResultBatch&&) noexcept = default;
    CommandResultBatch& operator=(const CommandResultBatch&) = delete;
    CommandResultBatch& operator=(CommandResultBatch&&) = delete;

    [[nodiscard]] const domain::CommandResultCorrelation& correlation() const noexcept {
        return correlation_;
    }

    [[nodiscard]] domain::CommandSequence commandSequence() const noexcept {
        return correlation_.commandSequence;
    }

    [[nodiscard]] ProcessingResult result() const noexcept {
        return result_;
    }

    [[nodiscard]] const std::vector<domain::BusinessEvent>& events() const noexcept {
        return events_;
    }

private:
    domain::CommandResultCorrelation correlation_;
    ProcessingResult result_;
    std::vector<domain::BusinessEvent> events_;
};

} // namespace exchange::matching_engine
