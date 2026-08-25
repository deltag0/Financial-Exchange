#pragma once

#include "domain_types.hpp"

#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#if __cplusplus >= 201703L
#define throw(...)
#endif

#include <quickfix/SessionID.h>

#if __cplusplus >= 201703L
#undef throw
#endif

namespace FIX {
class SessionSettings;
}

namespace exchange::core::fix {

inline constexpr const char* FIX_CLIENT_ID_SETTING = "ExchangeClientId";

class ClientIdentityResolver {
public:
    using Mapping = std::pair<FIX::SessionID, domain::ClientId>;

    explicit ClientIdentityResolver(std::initializer_list<Mapping> mappings);
    explicit ClientIdentityResolver(const FIX::SessionSettings& settings);

    [[nodiscard]] std::optional<domain::ClientId> resolve(const FIX::SessionID& sessionId) const noexcept;

private:
    void addMapping(const FIX::SessionID& sessionId, domain::ClientId clientId);

    // The resolver owns an immutable copy of every configured exact FIX identity mapping.
    std::map<FIX::SessionID, domain::ClientId> mappings_;
};

} // namespace exchange::core::fix
