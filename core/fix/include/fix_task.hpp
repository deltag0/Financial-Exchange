#pragma once

#include "../../admission/include/command_admission.hpp"
#include "../../bus/include/bus.hpp"
#include "../../shared_queue/include/shared_queue.hpp"
#include "../../task/include/task.hpp"
#include "fix_parser.hpp"

// Pre-include STL headers so the throw(...) macro hack doesn't break them
#include <cstddef>
#include <cstring>
#include <optional>

#if __cplusplus >= 201703L
// QuickFIX 1.15 uses dynamic exception specifications which were removed in
// C++17
#define throw(...)
#endif

#include <quickfix/Application.h>
#include <quickfix/Message.h>
#include <quickfix/Session.h>
#include <quickfix/SessionID.h>

#if __cplusplus >= 201703L
#undef throw
#endif

#define EXT_BURST_MESSAGES 64
#define INT_BURST_MESSAGES 32

namespace exchange::core::task {

class FixTask : public FIX::Application, public Task<sequencer::sequenceMessage> {
public:
    FixTask(core::SharedQueue<sequencer::sequenceMessage> &sequencingIngressQueue, Bus &multicastBus,
            const fix::ClientIdentityResolver &clientIdentityResolver,
            admission::CommandAdmissionIndex &commandAdmissionIndex, const std::size_t stagingQueueCapacity = 1000)
        : sequencingIngressQueue_(sequencingIngressQueue),
          stagingQueue_(stagingQueueCapacity),
          multicastBus(multicastBus),
          clientIdentityResolver(clientIdentityResolver),
          commandAdmissionIndex(commandAdmissionIndex) {
        multicastBus.registerCursor(cursor);
    }
    virtual ~FixTask() = default;

    [[nodiscard]] bool processNextStagedCommand();
    [[nodiscard]] bool stagingQueueEmpty() const {
        return stagingQueue_.empty();
    }
    [[nodiscard]] bool hasPendingStagedCommand() const noexcept {
        return pendingStagedCommand_.has_value();
    }
    [[nodiscard]] const std::optional<sequencer::sequenceMessage> &pendingStagedCommand() const noexcept {
        return pendingStagedCommand_;
    }

    void onCreate(const FIX::SessionID &sessionID) override;
    void onLogon(const FIX::SessionID &sessionID) override;
    void onLogout(const FIX::SessionID &sessionID) override;
    void toAdmin(FIX::Message &, const FIX::SessionID &) override;
    void toApp(FIX::Message &message, const FIX::SessionID &) noexcept override;
    void fromAdmin(const FIX::Message &message, const FIX::SessionID &) noexcept override;
    void fromApp(const FIX::Message &message, const FIX::SessionID &sessionID) noexcept override;

    void sendFixMessage(FIX::Message &message, const FIX::SessionID &sessionID);
    void run() override;
    void send(sequencer::sequenceMessage &message) override;

private:
    // The composition root owns sequencing ingress and outlives FixTask. QuickFIX session threads
    // are staging producers; this task's one worker is the staging consumer and ingress producer.
    core::SharedQueue<sequencer::sequenceMessage> &sequencingIngressQueue_;
    core::SharedQueue<sequencer::sequenceMessage> stagingQueue_;
    std::optional<sequencer::sequenceMessage> pendingStagedCommand_;
    Bus &multicastBus;
    // The exchange composition root owns both shared services and outlives FixTask.
    const fix::ClientIdentityResolver &clientIdentityResolver;
    admission::CommandAdmissionIndex &commandAdmissionIndex;
    std::atomic<Bus::cursor_type> cursor{0};
};

} // namespace exchange::core::task
