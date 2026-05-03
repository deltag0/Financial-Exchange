#pragma once

#include "../../../sequencer/include/sequencer.hpp"
#include "../../shared_queue/include/shared_queue.hpp"

// Pre-include STL headers so the throw(...) macro hack doesn't break them.
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#if __cplusplus >= 201703L
// QuickFIX 1.15 uses dynamic exception specifications which were removed in
// C++17.
#define throw(...)
#endif

#include <quickfix/Message.h>
#include <quickfix/SessionID.h>

#if __cplusplus >= 201703L
#undef throw
#endif

namespace exchange::core::fix {

class FixValidationError : public std::runtime_error {
  public:
    explicit FixValidationError(const std::string &message) : std::runtime_error(message) {}
};

/**
 * Parses a FIX message and converts it to a sequencer message
 * @param fixMessage The incoming FIX message
 * @param sessionID The FIX session ID
 * @param numShards The total number of message queue shards
 * @return A sequenceMessage ready to be queued, or empty if message type is not supported
 */
exchange::sequencer::sequenceMessage
parseFixMessage(const FIX::Message &fixMessage, const FIX::SessionID &sessionID, size_t numShards);

/**
 * Determines if a message is a type we should process (New Order or Cancel Order)
 * @param msgType The FIX message type
 * @return true if message type is 'D' (New Order) or 'F' (Cancel Order)
 */
bool isProcessableMessageType(const std::string &msgType);

} // namespace exchange::core::fix
