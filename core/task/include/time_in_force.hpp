#pragma once

namespace exchange::core::task {

enum class TimeInForce {
    // Active until end of day
    DAY = '0',
    // Good Till Cancel
    GTC = '1',
    // Immediate or Cancel
    IOC = '3',
    //  Fill or Kill
    FOK = '4',
    // Good Till Crossing
    GTX = '5',
    ATC = '7',
    // Good Till Date
    GTD = '6'
};

} // namespace exchange::core::task
