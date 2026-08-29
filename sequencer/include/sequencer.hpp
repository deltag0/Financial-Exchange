#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "../../core/admission/include/command_admission.hpp"
#include "../../core/task/include/task.hpp"
#include "sequence_message.hpp"

namespace exchange {
namespace sequencer {

#define ANY_TICKER ""
#define MAX_CLIENT_PORTS 65536

/*
globalSequenceNumber: sequence number for each ticker globally across all clients
topicSequenceNumbers: most recent sequence number for each ticker and client port combination
*/
struct topicData {
    uint64_t lastSenderPort;
    uint64_t sequenceNumber;
};

class Sequencer : public exchange::core::task::Task<sequenceMessage> {
public:
    struct InternalAdmissionBypassTag final {
        explicit InternalAdmissionBypassTag() = default;
    };
    inline static constexpr InternalAdmissionBypassTag INTERNAL_ADMISSION_BYPASS{};

    Sequencer(std::vector<core::SharedQueue<sequenceMessage>*> mq_shards,
              core::SharedQueue<sequenceMessage>* matchingEngineQueue,
              core::admission::CommandAdmissionIndex& admissionIndex)
        : Task{std::move(mq_shards)}, matchingEngineQueue(matchingEngineQueue), admissionIndex(&admissionIndex) {}

    Sequencer(std::vector<core::SharedQueue<sequenceMessage>*> mq_shards,
              core::SharedQueue<sequenceMessage>* matchingEngineQueue, InternalAdmissionBypassTag)
        : Task{std::move(mq_shards)}, matchingEngineQueue(matchingEngineQueue) {}

    void run() override;
    void send(sequenceMessage& message) override;
    domain::CommandSequence getNextGlobalSequenceNumber(const sequenceMessage& message);
    uint64_t getNextTopicSequenceNumber(const sequenceMessage& message);

    [[nodiscard]] bool hasPendingSequencedCommand() const noexcept {
        return pendingSequencedCommand.has_value();
    }

    [[nodiscard]] const std::optional<sequenceMessage>& pendingCommand() const noexcept {
        return pendingSequencedCommand;
    }

protected:
    bool processNext();

private:
    bool handoffPendingCommand();

private:
    // Global sequence number
    uint64_t globalSequenceNumber = 0;

    std::unordered_map<uint64_t, topicData> topicSequence;
    core::SharedQueue<sequenceMessage>* matchingEngineQueue = nullptr;
    core::admission::CommandAdmissionIndex* admissionIndex = nullptr;
    std::optional<sequenceMessage> pendingSequencedCommand;
};

} // namespace sequencer
} // namespace exchange
