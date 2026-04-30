#include "../include/fix_parser.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <quickfix/Fields.h>
#include <quickfix/FixFields.h>

namespace exchange::core::fix {

// Global order counter
std::atomic<uint64_t> orderCounter{0};

bool isProcessableMessageType(const std::string &msgType) {
    return msgType == "D" || msgType == "F";
}

sequencer::sequenceMessage parseFixMessage(const FIX::Message &fixMessage,
                                           const FIX::SessionID &sessionID, size_t numShards) {
    try {
        FIX::MsgType msgType;
        fixMessage.getHeader().getField(msgType);

        sequencer::sequenceMessage seqMsg{};
        seqMsg.port = std::hash<std::string>{}(sessionID.toString());

        // Determine order type based on message type
        if (msgType.getValue() == "F") {
            seqMsg.type = sequencer::orderType::CANCEL;
        } else if (msgType.getValue() == "D") {
            FIX::Side side;
            if (fixMessage.isSetField(side)) {
                fixMessage.getField(side);
                seqMsg.type = (side.getValue() == FIX::Side_BUY) ? sequencer::orderType::BUY
                                                                 : sequencer::orderType::SELL;
            }
        }

        // Extract symbol
        FIX::Symbol symbol;
        if (fixMessage.isSetField(symbol)) {
            fixMessage.getField(symbol);
            std::strncpy(seqMsg.symbol, symbol.getValue().c_str(), sizeof(seqMsg.symbol) - 1);
            seqMsg.symbol[sizeof(seqMsg.symbol) - 1] = '\0';

            // Determine shard based on ticker hash
            std::string ticker{seqMsg.symbol};
            seqMsg.shard_id = std::hash<std::string>{}(ticker) % numShards;
        }

        // Extract order quantity
        FIX::OrderQty qty;
        if (fixMessage.isSetField(qty)) {
            fixMessage.getField(qty);
            seqMsg.quantity = static_cast<uint64_t>(qty.getValue());
        } else {
            seqMsg.quantity = 0;
        }

        // Extract price (in basis points, multiplied by 10000)
        FIX::Price price;
        if (fixMessage.isSetField(price)) {
            fixMessage.getField(price);
            seqMsg.price = static_cast<uint64_t>(price.getValue() * 10000);
        } else {
            seqMsg.price = 0;
        }

        // Extract order ID
        FIX::ClOrdID clOrdID;
        if (fixMessage.isSetField(clOrdID)) {
            fixMessage.getField(clOrdID);
            seqMsg.id = std::hash<std::string>{}(clOrdID.getValue());
        }

        FIX::TimeInForce tif;
        if (fixMessage.isSetField(tif)) {
            fixMessage.getField(tif);
            seqMsg.tif = static_cast<exchange::core::task::TimeInForce>(tif.getValue());
        }

        // Assign global order counter and timestamp
        seqMsg.order = orderCounter.fetch_add(1, std::memory_order_seq_cst);
        seqMsg.timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();

        return seqMsg;

    } catch (const FIX::FieldNotFound &e) {
        std::cerr << "[FixParser] Required field missing: " << e.field << std::endl;
        throw;
    } catch (const std::exception &e) {
        std::cerr << "[FixParser] Error parsing FIX message: " << e.what() << std::endl;
        throw;
    }
}

} // namespace exchange::core::fix
