#pragma once

#include "../../shared_queue/include/shared_queue.hpp"

#include <vector>

namespace exchange::core::task {

template <typename T>
class Task {
public:
    Task(std::vector<core::SharedQueue<T>*> mq_shards) : mq_shards(mq_shards) {}
    virtual ~Task() = default;

    virtual void run() = 0;

    virtual void send() = 0;

protected:
    std::vector<core::SharedQueue<T>*> mq_shards;
};

} // namespace exchange::core::task