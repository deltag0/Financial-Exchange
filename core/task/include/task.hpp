#pragma once

#include "../../shared_queue/include/shared_queue.hpp"

#include <utility>
#include <vector>

namespace exchange::core::task {

enum class TimeInForce {
    DAY = '0',
    GTC = '1',
    IOC = '3',
    FOK = '4',
    GTX = '5',
    ATC = '7',
    GTD = '6'
};

template <typename T> class Task {
  public:
    using sequenceMessage = T;

    Task(const std::vector<core::SharedQueue<T> *> &mq_shards) : mq_shards(mq_shards) {}
    virtual ~Task() = default;
    virtual void run() = 0;
    virtual void send(sequenceMessage &message) = 0;

  protected:
    std::vector<core::SharedQueue<T> *> mq_shards;
};
} // namespace exchange::core::task