#include "crc32c.hpp"

namespace exchange::storage {

namespace {

constexpr std::uint32_t REFLECTED_CASTAGNOLI_POLYNOMIAL = 0x82F63B78U;
constexpr std::uint32_t FINAL_XOR = 0xFFFFFFFFU;

} // namespace

void Crc32cAccumulator::update(const std::span<const std::byte> bytes) noexcept {
    for (const std::byte byte : bytes) {
        state_ ^= std::to_integer<std::uint8_t>(byte);
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            const std::uint32_t lowBitMask = 0U - (state_ & 1U);
            state_ = (state_ >> 1U) ^ (REFLECTED_CASTAGNOLI_POLYNOMIAL & lowBitMask);
        }
    }
}

std::uint32_t Crc32cAccumulator::checksum() const noexcept {
    return state_ ^ FINAL_XOR;
}

} // namespace exchange::storage
