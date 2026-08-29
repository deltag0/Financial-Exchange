#pragma once

#include "../../admission/include/command_admission.hpp"
#include "../../bus/include/bus.hpp"
#include "../../shared_queue/include/shared_queue.hpp"
#include "../../task/include/task.hpp"
#include "fix_parser.hpp"

// Pre-include STL headers so the throw(...) macro hack doesn't break them
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

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
#define INTERNAL_QUEUES_COUNT 2

namespace exchange::core::task {

struct InternalQueues {
    std::unique_ptr<core::SharedQueue<sequencer::sequenceMessage>> fixMessageQueue;
};

class FixTask : public FIX::Application, public Task<sequencer::sequenceMessage> {
public:
    FixTask(const std::vector<core::SharedQueue<sequencer::sequenceMessage> *> &sequencerQueues, Bus &multicastBus,
            const fix::ClientIdentityResolver &clientIdentityResolver,
            admission::CommandAdmissionIndex &commandAdmissionIndex)
        : Task<sequencer::sequenceMessage>(sequencerQueues),
          multicastBus(multicastBus),
          clientIdentityResolver(clientIdentityResolver),
          commandAdmissionIndex(commandAdmissionIndex) {
        internalQueues.fixMessageQueue = std::make_unique<core::SharedQueue<sequencer::sequenceMessage>>(1000);

        multicastBus.registerCursor(cursor);
    }
    virtual ~FixTask() = default;

    core::SharedQueue<sequencer::sequenceMessage> *getFixMessageQueue() const {
        return internalQueues.fixMessageQueue.get();
    }

    [[nodiscard]] bool processNextNormalizedCommand();
    [[nodiscard]] bool hasPendingNormalizedCommand() const noexcept {
        return pendingNormalizedCommand.has_value();
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
    [[nodiscard]] bool trySendSequencerMessage(const sequencer::sequenceMessage &message);

    // Internal Queues to process bursts of messages from FIX sessions and internal business
    // messages
    InternalQueues internalQueues;
    Bus &multicastBus;
    // The exchange composition root owns both shared services and outlives FixTask.
    const fix::ClientIdentityResolver &clientIdentityResolver;
    admission::CommandAdmissionIndex &commandAdmissionIndex;
    std::optional<sequencer::sequenceMessage> pendingNormalizedCommand;
    std::atomic<Bus::cursor_type> cursor{0};
};

} // namespace exchange::core::task
