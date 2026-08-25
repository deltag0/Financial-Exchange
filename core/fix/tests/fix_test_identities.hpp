#pragma once

#include "client_identity.hpp"

namespace exchange::core::fix::test {

inline const ClientIdentityResolver& clientIdentityResolver() {
    static const ClientIdentityResolver resolver({
        {FIX::SessionID("FIX.4.4", "S", "T"), domain::ClientId{101}},
        {FIX::SessionID("FIX.4.4", "SENDER", "TARGET"), domain::ClientId{102}},
    });
    return resolver;
}

} // namespace exchange::core::fix::test
