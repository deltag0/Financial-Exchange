#pragma once

#include <cstdint>
#include <string_view>

namespace exchange::core::instrument {

struct InstrumentConfiguration {
    std::string_view symbol;
    std::uint64_t instrumentId;
    std::uint64_t configurationVersion;
    std::uint32_t priceScale;
    std::uint64_t tickSizeMantissa;
    std::uint64_t lotSize;
    std::uint64_t maxPriceTicks;
    std::uint64_t maxOrderQuantity;
    std::uint64_t maxPriceLevelAggregate;
};

inline constexpr InstrumentConfiguration SPY_V1{
    .symbol = "SPY",
    .instrumentId = 1,
    .configurationVersion = 1,
    .priceScale = 4,
    .tickSizeMantissa = 1,
    .lotSize = 1,
    .maxPriceTicks = 10'000'000'000ULL,
    .maxOrderQuantity = 100'000'000ULL,
    .maxPriceLevelAggregate = 1'000'000'000ULL,
};

inline constexpr const InstrumentConfiguration* findBySymbol(const std::string_view symbol) {
    return symbol == SPY_V1.symbol ? &SPY_V1 : nullptr;
}

inline constexpr const InstrumentConfiguration* findByIdentity(const std::uint64_t instrumentId,
                                                               const std::uint64_t configurationVersion) {
    return instrumentId == SPY_V1.instrumentId && configurationVersion == SPY_V1.configurationVersion ? &SPY_V1
                                                                                                      : nullptr;
}

} // namespace exchange::core::instrument
