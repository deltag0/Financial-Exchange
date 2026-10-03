#pragma once
#include <optional>

#include "../../core/admission/include/command_admission.hpp"
#include "../../core/shared_queue/include/shared_queue.hpp"
#include "../../core/storage/include/run_journal_writer.hpp"
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
              core::SharedQueue<sequenceMessage>& matchingEngineQueue,
              core::admission::CommandAdmissionIndex& admissionIndex, storage::RunJournalWriterV1& journalWriter);

    Sequencer(core::SharedQueue<sequenceMessage>& sequencingIngressQueue,
              core::SharedQueue<sequenceMessage>& matchingEngineQueue, InternalAdmissionBypassTag,
              domain::CommandSequence lastAssignedSequence = {})
        : sequencingIngressQueue_(sequencingIngressQueue),
          matchingEngineQueue_(matchingEngineQueue),
          lastAssignedSequence_(lastAssignedSequence) {}

    void run() override;
    void send(sequenceMessage& message) override;
    // Nonblocking sole-consumer cycle; caller provides single-threaded access or quiescence.
    [[nodiscard]] bool advance();
    [[nodiscard]] bool hasPendingSequencedCommand() const noexcept {
        return pendingCommand_.has_value() && pendingReadyForMatching_;
    }

    // Inspect pending state and append failure only on the consumer thread or while externally quiesced.
    [[nodiscard]] const std::optional<sequenceMessage>& pendingCommand() const noexcept {
        return pendingCommand_;
    }

    [[nodiscard]] const std::optional<storage::RunJournalAppendResult>& journalAppendFailure() const noexcept {
        return journalAppendFailure_;
    }

protected:
    bool drainAvailable();
    bool processNext();

private:
    [[nodiscard]] domain::CommandSequence nextCommandSequence();
    [[nodiscard]] storage::RunJournalAppendResult appendCommand(const sequenceMessage& message) noexcept;
    bool handoffPendingCommand();

private:
    // Caller owns queues, admission and optional writer; all outlive this sole consumer.
    // The journal-backed caller guarantees exclusive append ownership; failures require lifecycle handling.
    core::SharedQueue<sequenceMessage>& sequencingIngressQueue_;
    core::SharedQueue<sequenceMessage>& matchingEngineQueue_;
    core::admission::CommandAdmissionIndex* admissionIndex_ = nullptr;
    storage::RunJournalWriterV1* journalWriter_ = nullptr;
    domain::CommandSequence lastAssignedSequence_{};
    std::optional<sequenceMessage> pendingCommand_;
    bool pendingReadyForMatching_{false};
    std::optional<storage::RunJournalAppendResult> journalAppendFailure_;
};

} // namespace sequencer
} // namespace exchange
