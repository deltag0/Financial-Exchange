#pragma once

#include "../../core/domain/include/business_events.hpp"
#include "../../core/domain/include/command_result_fwd.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
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
            if (eventSequence != correlation_.commandSequence) {
                throw std::invalid_argument("command-result event sequence does not match batch correlation");
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

class BoundedCommandResultQueue final {
public:
    explicit BoundedCommandResultQueue(const std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("command-result queue capacity must be positive");
        }
    }

    bool tryPush(ImmutableCommandResultBatch batch) {
        if (batch == nullptr) {
            throw std::invalid_argument("command-result queue cannot accept an empty batch pointer");
        }

        std::lock_guard lock(mutex_);
        if (batches_.size() >= capacity_) {
            return false;
        }
        batches_.push_back(std::move(batch));
        return true;
    }

    bool tryPop(ImmutableCommandResultBatch& batch) {
        std::lock_guard lock(mutex_);
        if (batches_.empty()) {
            return false;
        }
        batch = std::move(batches_.front());
        batches_.pop_front();
        return true;
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard lock(mutex_);
        return batches_.empty();
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return batches_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<ImmutableCommandResultBatch> batches_;
};

} // namespace exchange::matching_engine
