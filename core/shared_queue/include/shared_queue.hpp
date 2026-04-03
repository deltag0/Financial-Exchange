#pragma once

#include <boost/lockfree/queue.hpp>
#include <string>
#include <memory>
#include <iostream>

namespace exchange::core {

template <typename T>
class SharedQueue {
public:
    SharedQueue(size_t max_messages = 1000) : q_(max_messages) {}

    bool push(const T& data) {
        return q_.push(data);
    }

    bool pop(T& buffer) {
        return q_.pop(buffer);
    }

    bool empty() const {
        return q_.empty();
    }

private:
    boost::lockfree::queue<T, boost::lockfree::fixed_sized<true>> q_;
};

} // namespace exchange::core
