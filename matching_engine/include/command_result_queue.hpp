#pragma once

#include "../../core/domain/include/business_events.hpp"

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
    CommandResultBatch(const domain::CommandSequence commandSequence, const ProcessingResult result,
                       std::vector<domain::BusinessEvent> events)
        : commandSequence_(commandSequence), result_(result), events_(std::move(events)) {}

    CommandResultBatch(const CommandResultBatch&) = default;
    CommandResultBatch(CommandResultBatch&&) noexcept = default;
    CommandResultBatch& operator=(const CommandResultBatch&) = delete;
    CommandResultBatch& operator=(CommandResultBatch&&) = delete;

    [[nodiscard]] domain::CommandSequence commandSequence() const noexcept {
        return commandSequence_;
    }

    [[nodiscard]] ProcessingResult result() const noexcept {
        return result_;
    }

    [[nodiscard]] const std::vector<domain::BusinessEvent>& events() const noexcept {
        return events_;
    }

private:
    domain::CommandSequence commandSequence_;
    ProcessingResult result_;
    std::vector<domain::BusinessEvent> events_;
};

using ImmutableCommandResultBatch = std::shared_ptr<const CommandResultBatch>;

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
