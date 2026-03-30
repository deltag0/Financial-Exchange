#pragma once
#include <cstdint>

namespace exchange::sequencer {

/*
Order types accepted to be sent by contributors to the matching engine
*/
enum class orderType {
    BUY,
    SELL,
    CANCEL,
    CANCELREJ,
};

/*
Message format transmitted to the matching engine
*/
struct sequenceMessage {
    uint64_t id;
    uint64_t sequence_number;
    uint64_t port;
    uint64_t price;
    uint64_t quantity;
    char symbol[10];
    orderType type;
};

}