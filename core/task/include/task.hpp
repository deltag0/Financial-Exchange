#pragma once

#include "time_in_force.hpp"

namespace exchange::core::task {

template <typename T>
class Task {
public:
    using sequenceMessage = T;

    virtual ~Task() = default;
    virtual void run() = 0;
    virtual void send(sequenceMessage &message) = 0;
};
} // namespace exchange::core::task
