#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace exchange::domain {

template <typename Tag, typename Representation>
class StrongUnsigned final {
    static_assert(std::is_unsigned_v<Representation>);

public:
    using Underlying = Representation;

    constexpr StrongUnsigned() = default;
    explicit constexpr StrongUnsigned(const Representation value) : value_(value) {}

    [[nodiscard]] constexpr Representation value() const noexcept {
        return value_;
    }

    constexpr auto operator<=>(const StrongUnsigned&) const = default;

private:
    Representation value_{};
};

struct CommandSequenceTag;
struct OrderIdTag;
struct TargetOrderIdTag;
struct ClientIdTag;
struct InstrumentIdTag;
struct EventIndexTag;
struct PriceTag;
struct QuantityTag;

using CommandSequence = StrongUnsigned<CommandSequenceTag, std::uint64_t>;
using OrderId = StrongUnsigned<OrderIdTag, std::uint64_t>;
using TargetOrderId = StrongUnsigned<TargetOrderIdTag, std::uint64_t>;
using ClientId = StrongUnsigned<ClientIdTag, std::uint64_t>;
using InstrumentId = StrongUnsigned<InstrumentIdTag, std::uint64_t>;
using EventIndex = StrongUnsigned<EventIndexTag, std::uint32_t>;
using Price = StrongUnsigned<PriceTag, std::uint64_t>;
using Quantity = StrongUnsigned<QuantityTag, std::uint64_t>;

[[nodiscard]] constexpr OrderId orderIdFrom(const CommandSequence commandSequence) noexcept {
    return OrderId{commandSequence.value()};
}

[[nodiscard]] constexpr OrderId orderIdFrom(const TargetOrderId targetOrderId) noexcept {
    return OrderId{targetOrderId.value()};
}

class ClientCommandId final {
public:
    static constexpr std::size_t MAX_LENGTH = 64;

    explicit ClientCommandId(const std::string_view value) : size_(static_cast<std::uint8_t>(value.size())) {
        if (value.empty() || value.size() > MAX_LENGTH) {
            throw std::invalid_argument("ClientCommandId must contain between 1 and 64 ASCII bytes");
        }
        for (const char byte : value) {
            if (static_cast<unsigned char>(byte) > 0x7f) {
                throw std::invalid_argument("ClientCommandId must contain only ASCII bytes");
            }
        }
        for (std::size_t index = 0; index < value.size(); ++index) {
            bytes_[index] = value[index];
        }
    }

    [[nodiscard]] std::string_view value() const noexcept {
        return {bytes_.data(), size_};
    }

    bool operator==(const ClientCommandId&) const = default;

private:
    std::array<char, MAX_LENGTH> bytes_{};
    std::uint8_t size_;
};

struct CommandResultCorrelation final {
    ClientId clientId;
    ClientCommandId clientCommandId;
    CommandSequence commandSequence;

    bool operator==(const CommandResultCorrelation&) const = default;
};

static_assert(std::is_trivially_copyable_v<CommandSequence>);
static_assert(std::is_trivially_copyable_v<TargetOrderId>);
static_assert(std::is_trivially_copyable_v<ClientCommandId>);
static_assert(!std::is_convertible_v<Price, Quantity>);
static_assert(!std::is_convertible_v<Quantity, Price>);

} // namespace exchange::domain
