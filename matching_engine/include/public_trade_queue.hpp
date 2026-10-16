#pragma once

#include "public_trade_projection.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace exchange::market_data {

class BoundedPublicTradeQueue final {
public:
    explicit BoundedPublicTradeQueue(const std::size_t recordCapacity) : recordCapacity_(recordCapacity) {
        if (recordCapacity == 0) {
            throw std::invalid_argument("public-trade queue record capacity must be positive");
        }
    }

    bool tryPush(std::vector<PublicTrade>& batch) {
        if (batch.empty()) {
            throw std::invalid_argument("public-trade queue cannot accept an empty batch");
        }

        std::lock_guard lock(mutex_);
        if (batch.size() > recordCapacity_ - queuedRecordCount_) {
            return false;
        }
        const std::size_t acceptedRecords = batch.size();
        batches_.push_back(std::move(batch));
        queuedRecordCount_ += acceptedRecords;
        return true;
    }

    bool tryPop(std::vector<PublicTrade>& batch) {
        std::lock_guard lock(mutex_);
        if (batches_.empty()) {
            return false;
        }
        queuedRecordCount_ -= batches_.front().size();
        batch = std::move(batches_.front());
        batches_.pop_front();
        return true;
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard lock(mutex_);
        return batches_.empty();
    }

    [[nodiscard]] std::size_t queuedRecordCount() const {
        std::lock_guard lock(mutex_);
        return queuedRecordCount_;
    }

private:
    const std::size_t recordCapacity_;
    mutable std::mutex mutex_;
    std::deque<std::vector<PublicTrade>> batches_;
    std::size_t queuedRecordCount_{0};
};

} // namespace exchange::market_data
