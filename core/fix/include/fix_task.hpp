#pragma once

#include "../../task/include/task.hpp"
#include "../../sequencer/include/sequencer.hpp"

// Pre-include STL headers so the throw(...) macro hack doesn't break them
#include <atomic>
#include <string>
#include <cstring>
#include <vector>
#include <memory>
#include <map>
#include <set>
#include <stdexcept>
#include <iostream>
#include <ostream>
#include <istream>
#include <sstream>
#include <queue>
#include <deque>
#include <list>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>

#if __cplusplus >= 201703L
// QuickFIX 1.15 uses dynamic exception specifications which were removed in C++17
#define throw(...)
#endif

#include <quickfix/Application.h>
#include <quickfix/Message.h>
#include <quickfix/Session.h>
#include <quickfix/SessionID.h>

#if __cplusplus >= 201703L
#undef throw
#endif

namespace exchange::core::task {

std::atomic<uint64_t> orderCounter{0};

class FixTask : public FIX::Application {
public:
    FixTask(std::vector<core::SharedQueue<sequencer::sequenceMessage>*> mq_shards) : mq_shards(mq_shards) {}
    virtual ~FixTask() = default;

    // FIX::Application overrides
    void onCreate(const FIX::SessionID& sessionID) override {
        std::cout << "[FixTask] Session created: " << sessionID << std::endl;
    }
    void onLogon(const FIX::SessionID& sessionID) override {
        std::cout << "[FixTask] Logon received: " << sessionID << std::endl;
    }
    void onLogout(const FIX::SessionID& sessionID) override {
        std::cout << "[FixTask] Logout received: " << sessionID << std::endl;
    }

    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}

    void toApp(FIX::Message& message, const FIX::SessionID&) noexcept override {
        std::cout << "[FixTask] -> Sending App Data: " << message.toString() << std::endl;
    }

    void fromAdmin(const FIX::Message& message, const FIX::SessionID&) noexcept override {
        std::cout << "[FixTask] <- Received Admin (Raw): " << message.toString().substr(0, 30) << "..." << std::endl;
    }

    void fromApp(const FIX::Message& message, const FIX::SessionID& sessionID) noexcept override {
        std::cout << "\n=======================================================\n";
        std::cout << "[FixTask] <- APPLICATION MESSAGE RECEIVED FROM " << sessionID.getTargetCompID().getValue()
                  << "\n";
        std::cout << message.toString() << "\n";
        std::cout << "=======================================================\n\n";

        createSequencerMessage(message, sessionID);
    }

    // Sends a FIX message using the QuickFIX session
    void sendFixMessage(FIX::Message& message, const FIX::SessionID& sessionID) {
        FIX::Session::sendToTarget(message, sessionID);
    }

    void createSequencerMessage(const FIX::Message& message, const FIX::SessionID& sessionID) {
        try {
            FIX::MsgType msgType;
            message.getHeader().getField(msgType);

            if (msgType.getValue() != "D" && msgType.getValue() != "F") {
                return;
            }

            sequencer::sequenceMessage seqMsg{};

            seqMsg.sequence_number = 0;
            seqMsg.port = std::hash<std::string>{}(sessionID.toString());

            if (msgType.getValue() == "F") {
                seqMsg.type = sequencer::orderType::CANCEL;
            } else {
                FIX::Side side;
                if (message.isSetField(side)) {
                    message.getField(side);
                    seqMsg.type =
                        (side.getValue() == FIX::Side_BUY) ? sequencer::orderType::BUY : sequencer::orderType::SELL;
                }
            }

            FIX::Symbol symbol;
            if (message.isSetField(symbol)) {
                message.getField(symbol);
                std::strncpy(seqMsg.symbol, symbol.getValue().c_str(), sizeof(seqMsg.symbol) - 1);
                seqMsg.symbol[sizeof(seqMsg.symbol) - 1] = '\0';

                std::string ticker{seqMsg.symbol};
                seqMsg.shard_id = std::hash<std::string>{}(ticker) % mq_shards.size();
            }

            FIX::OrderQty qty;
            if (message.isSetField(qty)) {
                message.getField(qty);
                seqMsg.quantity = static_cast<uint64_t>(qty.getValue());
            } else {
                seqMsg.quantity = 0;
            }

            FIX::Price price;
            if (message.isSetField(price)) {
                message.getField(price);
                seqMsg.price = static_cast<uint64_t>(price.getValue() * 10000);
            } else {
                seqMsg.price = 0;
            }

            FIX::ClOrdID clOrdID;
            if (message.isSetField(clOrdID)) {
                message.getField(clOrdID);
                seqMsg.id = std::hash<std::string>{}(clOrdID.getValue());
            }

            seqMsg.order = orderCounter.fetch_add(1, std::memory_order_seq_cst);
            seqMsg.timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();

            sendSequencerMessage(seqMsg);

        } catch (const FIX::FieldNotFound& e) {
            std::cerr << "[FixTask] Required field missing: " << e.field << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[FixTask] Error: " << e.what() << std::endl;
        }
    }

    void sendSequencerMessage(sequencer::sequenceMessage& message) {
        if (!mq_shards[message.shard_id]->push(message)) {
            std::cerr << "[FixTask] Failed to send message to sequencer shard " << (int)message.shard_id << std::endl;
        }
    }

private:
    std::vector<core::SharedQueue<sequencer::sequenceMessage>*> mq_shards;
};

} // namespace exchange::core::task
