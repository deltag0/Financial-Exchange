#pragma once
#include <optional>

#include "../../core/admission/include/command_admission.hpp"
#include "../../core/shared_queue/include/shared_queue.hpp"
#include "../../core/task/include/task.hpp"
#include "sequence_message.hpp"

namespace exchange {
namespace sequencer {

class Sequencer : public exchange::core::task::Task<sequenceMessage> {
public:
    struct InternalAdmissionBypassTag final {
        explicit InternalAdmissionBypassTag() = default;
    };
    inline static constexpr InternalAdmissionBypassTag INTERNAL_ADMISSION_BYPASS{};

    Sequencer(core::SharedQueue<sequenceMessage>& sequencingIngressQueue,
              core::SharedQueue<sequenceMessage>& matchingEngineQueue,
              core::admission::CommandAdmissionIndex& admissionIndex, domain::CommandSequence lastAssignedSequence = {})
        : sequencingIngressQueue_(sequencingIngressQueue),
          matchingEngineQueue_(matchingEngineQueue),
          admissionIndex_(&admissionIndex),
          lastAssignedSequence_(lastAssignedSequence) {}

    Sequencer(core::SharedQueue<sequenceMessage>& sequencingIngressQueue,
              core::SharedQueue<sequenceMessage>& matchingEngineQueue, InternalAdmissionBypassTag,
              domain::CommandSequence lastAssignedSequence = {})
        : sequencingIngressQueue_(sequencingIngressQueue),
          matchingEngineQueue_(matchingEngineQueue),
          lastAssignedSequence_(lastAssignedSequence) {}

    void run() override;
    void send(sequenceMessage& message) override;
    [[nodiscard]] bool hasPendingSequencedCommand() const noexcept {
        return pendingSequencedCommand_.has_value();
    }

    [[nodiscard]] const std::optional<sequenceMessage>& pendingCommand() const noexcept {
        return pendingSequencedCommand_;
    }

protected:
    bool drainAvailable();
    bool processNext();

private:
    [[nodiscard]] domain::CommandSequence nextCommandSequence();
    bool handoffPendingCommand();

private:
    // The composition root owns both queues and the admission index; all outlive this sole consumer.
    core::SharedQueue<sequenceMessage>& sequencingIngressQueue_;
    core::SharedQueue<sequenceMessage>& matchingEngineQueue_;
    core::admission::CommandAdmissionIndex* admissionIndex_ = nullptr;
    domain::CommandSequence lastAssignedSequence_;
    std::optional<sequenceMessage> pendingSequencedCommand_;
};

} // namespace sequencer
} // namespace exchange
