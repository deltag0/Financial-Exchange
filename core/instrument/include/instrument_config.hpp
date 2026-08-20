#pragma once

#include "domain_types.hpp"

#include <cstdint>
#include <string_view>

namespace exchange::core::instrument {

struct InstrumentConfiguration {
    std::string_view symbol;
    domain::InstrumentId instrumentId;
    std::uint64_t configurationVersion;
    std::uint32_t priceScale;
    std::uint64_t tickSizeMantissa;
    domain::Quantity lotSize;
    domain::Price maxPrice;
    domain::Quantity maxOrderQuantity;
    domain::Quantity maxPriceLevelAggregate;
};

inline constexpr InstrumentConfiguration SPY_V1{
    .symbol = "SPY",
    .instrumentId = domain::InstrumentId{1},
    .configurationVersion = 1,
    .priceScale = 4,
    .tickSizeMantissa = 1,
    .lotSize = domain::Quantity{1},
    .maxPrice = domain::Price{10'000'000'000ULL},
    .maxOrderQuantity = domain::Quantity{100'000'000ULL},
    .maxPriceLevelAggregate = domain::Quantity{1'000'000'000ULL},
};

inline constexpr const InstrumentConfiguration* findBySymbol(const std::string_view symbol) {
    return symbol == SPY_V1.symbol ? &SPY_V1 : nullptr;
}

inline constexpr const InstrumentConfiguration* findByIdentity(const domain::InstrumentId instrumentId,
                                                               const std::uint64_t configurationVersion) {
    return instrumentId == SPY_V1.instrumentId && configurationVersion == SPY_V1.configurationVersion ? &SPY_V1
                                                                                                      : nullptr;
}

} // namespace exchange::core::instrument
