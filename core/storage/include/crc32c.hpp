#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace exchange::storage {

class Crc32cAccumulator final {
public:
    void update(std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] std::uint32_t checksum() const noexcept;

private:
    std::uint32_t state_{0xFFFFFFFFU};
};

} // namespace exchange::storage
