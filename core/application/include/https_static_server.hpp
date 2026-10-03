#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace boost::asio {
class io_context;
}

namespace exchange::application_event {

struct HttpsStaticServerConfiguration final {
    std::string listenAddress;
    std::uint16_t listenPort{};
    std::filesystem::path staticAssetRoot;
    std::filesystem::path certificateChainPath;
    std::filesystem::path privateKeyPath;
    std::size_t maximumConnections{};
    std::size_t maximumHttpHeaderBytes{};
    std::size_t maximumHttpBodyBytes{};
    std::size_t maximumStaticAssetBytes{};
    std::chrono::milliseconds tlsHandshakeTimeout{};
    std::chrono::milliseconds idleTimeout{};
};

enum class HttpsStaticServerStartFailure : std::uint8_t {
    NONE,
    ALREADY_STARTED,
    INVALID_CONFIGURATION,
    STATIC_ASSET_ROOT_INVALID,
    CERTIFICATE_CHAIN_LOAD_FAILED,
    PRIVATE_KEY_LOAD_FAILED,
    PRIVATE_KEY_WITHIN_STATIC_ROOT,
    TLS_CONFIGURATION_FAILED,
    ACCEPTOR_OPEN_FAILED,
    ACCEPTOR_BIND_FAILED,
    ACCEPTOR_LISTEN_FAILED,
    INTERNAL_FAILURE,
};

struct HttpsStaticServerStartResult final {
    HttpsStaticServerStartFailure failure{HttpsStaticServerStartFailure::NONE};
    int systemError{};
};

struct LocalHttpsEndpoint final {
    std::string address;
    std::uint16_t port{};
};

class HttpsStaticServer final {
public:
    HttpsStaticServer(boost::asio::io_context& ioContext, HttpsStaticServerConfiguration configuration);
    ~HttpsStaticServer();

    HttpsStaticServer(const HttpsStaticServer&) = delete;
    HttpsStaticServer& operator=(const HttpsStaticServer&) = delete;
    HttpsStaticServer(HttpsStaticServer&&) = delete;
    HttpsStaticServer& operator=(HttpsStaticServer&&) = delete;

    [[nodiscard]] HttpsStaticServerStartResult start() noexcept;
    void stop() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::optional<LocalHttpsEndpoint> localEndpoint() const;

private:
    class Implementation;
    std::shared_ptr<Implementation> implementation_;
};

} // namespace exchange::application_event
