#include "client_identity.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#if __cplusplus >= 201703L
#define throw(...)
#endif

#include <quickfix/Dictionary.h>
#include <quickfix/SessionSettings.h>

#if __cplusplus >= 201703L
#undef throw
#endif

namespace exchange::core::fix {
namespace {

domain::ClientId parseConfiguredClientId(const std::string_view text, const FIX::SessionID& sessionId) {
    if (text.empty()) {
        throw std::invalid_argument("empty " + std::string(FIX_CLIENT_ID_SETTING) + " for FIX session " +
                                    sessionId.toString());
    }

    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            throw std::invalid_argument(std::string(FIX_CLIENT_ID_SETTING) +
                                        " must be an unsigned decimal for FIX session " + sessionId.toString());
        }

        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (value > std::numeric_limits<std::uint64_t>::max() / 10 ||
            (value == std::numeric_limits<std::uint64_t>::max() / 10 &&
             digit > std::numeric_limits<std::uint64_t>::max() % 10)) {
            throw std::invalid_argument(std::string(FIX_CLIENT_ID_SETTING) + " overflows uint64 for FIX session " +
                                        sessionId.toString());
        }
        value = value * 10 + digit;
    }

    if (value == 0) {
        throw std::invalid_argument(std::string(FIX_CLIENT_ID_SETTING) + " must be nonzero for FIX session " +
                                    sessionId.toString());
    }
    return domain::ClientId{value};
}

} // namespace

ClientIdentityResolver::ClientIdentityResolver(const std::initializer_list<Mapping> mappings) {
    for (const auto& [sessionId, clientId] : mappings) {
        addMapping(sessionId, clientId);
    }
    if (mappings_.empty()) {
        throw std::invalid_argument("FIX client identity configuration must not be empty");
    }
}

ClientIdentityResolver::ClientIdentityResolver(const FIX::SessionSettings& settings) {
    for (const FIX::SessionID& sessionId : settings.getSessions()) {
        const FIX::Dictionary& session = settings.get(sessionId);
        if (!session.has(FIX_CLIENT_ID_SETTING)) {
            throw std::invalid_argument("missing " + std::string(FIX_CLIENT_ID_SETTING) + " for FIX session " +
                                        sessionId.toString());
        }
        addMapping(sessionId, parseConfiguredClientId(session.getString(FIX_CLIENT_ID_SETTING), sessionId));
    }
    if (mappings_.empty()) {
        throw std::invalid_argument("FIX client identity configuration must not be empty");
    }
}

std::optional<domain::ClientId> ClientIdentityResolver::resolve(const FIX::SessionID& sessionId) const noexcept {
    const auto mapping = mappings_.find(sessionId);
    if (mapping == mappings_.end()) {
        return std::nullopt;
    }
    return mapping->second;
}

void ClientIdentityResolver::addMapping(const FIX::SessionID& sessionId, const domain::ClientId clientId) {
    if (clientId.value() == 0) {
        throw std::invalid_argument("configured FIX ClientId must be nonzero");
    }
    if (!mappings_.emplace(sessionId, clientId).second) {
        throw std::invalid_argument("duplicate configured FIX session identity: " + sessionId.toString());
    }
}

} // namespace exchange::core::fix
