#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace exchange::storage::detail {

inline void writeUint16LittleEndian(std::span<std::byte> bytes, const std::size_t offset,
                                    const std::uint16_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = std::byte{static_cast<std::uint8_t>(value >> (index * 8U))};
    }
}

inline void writeUint32LittleEndian(std::span<std::byte> bytes, const std::size_t offset,
                                    const std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = std::byte{static_cast<std::uint8_t>(value >> (index * 8U))};
    }
}

inline void writeUint64LittleEndian(std::span<std::byte> bytes, const std::size_t offset,
                                    const std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = std::byte{static_cast<std::uint8_t>(value >> (index * 8U))};
    }
}

inline std::uint16_t readUint16LittleEndian(const std::span<const std::byte> bytes, const std::size_t offset) noexcept {
    std::uint16_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8U);
    }
    return value;
}

inline std::uint32_t readUint32LittleEndian(const std::span<const std::byte> bytes, const std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8U);
    }
    return value;
}

inline std::uint64_t readUint64LittleEndian(const std::span<const std::byte> bytes, const std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8U);
    }
    return value;
}

} // namespace exchange::storage::detail
