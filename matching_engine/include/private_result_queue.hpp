#pragma once

#include "private_result_projection.hpp"

#include <cstddef>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace exchange::private_result {

class BoundedPrivateResultQueue final {
public:
    explicit BoundedPrivateResultQueue(const std::size_t eventCapacity) : eventCapacity_(eventCapacity) {
        if (eventCapacity == 0) {
            throw std::invalid_argument("private-result queue event capacity must be positive");
        }
    }

    bool tryPush(RecipientResults& batch) {
        if (batch.empty()) {
            throw std::invalid_argument("private-result queue cannot accept an empty batch");
        }

        std::size_t batchEventCount = 0;
        for (const auto& recipient : batch) {
            if (recipient.privateResult.size() > std::numeric_limits<std::size_t>::max() - batchEventCount) {
                throw std::length_error("private-result batch event count overflow");
            }
            batchEventCount += recipient.privateResult.size();
        }
        if (batchEventCount == 0) {
            throw std::invalid_argument("private-result queue cannot accept a batch without events");
        }

        std::lock_guard lock(mutex_);
        if (batchEventCount > eventCapacity_ - queuedEventCount_) {
            return false;
        }
        batches_.push_back(std::move(batch));
        queuedEventCount_ += batchEventCount;
        return true;
    }

    bool tryPop(RecipientResults& batch) {
        std::lock_guard lock(mutex_);
        if (batches_.empty()) {
            return false;
        }
        for (const auto& recipient : batches_.front()) {
            queuedEventCount_ -= recipient.privateResult.size();
        }
        batch = std::move(batches_.front());
        batches_.pop_front();
        return true;
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard lock(mutex_);
        return batches_.empty();
    }

    [[nodiscard]] std::size_t queuedEventCount() const {
        std::lock_guard lock(mutex_);
        return queuedEventCount_;
    }

private:
    const std::size_t eventCapacity_;
    mutable std::mutex mutex_;
    std::deque<RecipientResults> batches_;
    std::size_t queuedEventCount_{0};
};

} // namespace exchange::private_result
