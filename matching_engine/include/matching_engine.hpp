#pragma once

#include "command_result_queue.hpp"
#include "matching_state.hpp"

#include "../../bus/include/bus.hpp"
#include "../../core/shared_queue/include/shared_queue.hpp"
#include "../../core/task/include/task.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace exchange::matching_engine {

class MatchingEngine : public exchange::core::task::Task<sequencer::sequenceMessage> {
public:
    MatchingEngine(core::SharedQueue<sequencer::sequenceMessage>* sequencerQueue, core::Bus& multicastBus,
                   BoundedCommandResultQueue& commandResultQueue, MatchingState& matchingState);

    void run() override;
    void send(sequencer::sequenceMessage& message) override;
    // Nonblocking sole-consumer cycle; caller provides single-threaded access or quiescence.
    [[nodiscard]] bool advance();
    [[nodiscard]] bool hasPendingResult() const noexcept {
        return pendingCommandResult != nullptr;
    }
    [[nodiscard]] const std::optional<sequencer::sequenceMessage>& inProgressCommand() const noexcept {
        return inProgressCommand_;
    }
    [[nodiscard]] std::uint64_t multicastWriteFailures() const noexcept {
        return multicastWriteFailures_.load(std::memory_order_relaxed);
    }

protected:
    MatchingEngine(core::SharedQueue<sequencer::sequenceMessage>* sequencerQueue, core::Bus& multicastBus,
                   std::size_t ownedResultQueueCapacity, MatchingState& matchingState);

    bool drainQueue(core::SharedQueue<sequencer::sequenceMessage>& queue, const char* source, std::size_t index = 0);

    bool handoffPendingResult();

    core::SharedQueue<sequencer::sequenceMessage>& sequencerQueue;

    // Re-transmission bus for data to ports
    core::Bus& multicastBus;

    // Matching owns retry state; the event-stream boundary owns accepted immutable batches.
    std::unique_ptr<BoundedCommandResultQueue> ownedCommandResultQueue;
    BoundedCommandResultQueue& commandResultQueue;
    // Caller-owned state must outlive this worker and all worker invocations.
    MatchingState& matchingState_;
    ImmutableCommandResultBatch pendingCommandResult;
    // Retained if matching throws after dequeue; an uncertain mutation must never be retried here.
    std::optional<sequencer::sequenceMessage> inProgressCommand_{};
    std::atomic<std::uint64_t> multicastWriteFailures_{0};
};

} // namespace exchange::matching_engine
