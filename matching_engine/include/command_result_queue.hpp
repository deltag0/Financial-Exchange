#pragma once

#include "command_result.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace exchange::matching_engine {

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
