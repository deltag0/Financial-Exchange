#include "https_static_server.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <openssl/ssl.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace exchange::application_event {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

enum class AssetResolutionFailure : std::uint8_t {
    NONE,
    MALFORMED_TARGET,
    OUTSIDE_ROOT,
    NOT_REGULAR,
    FILESYSTEM_FAILURE,
};

struct AssetResolution final {
    AssetResolutionFailure failure{AssetResolutionFailure::NONE};
    std::filesystem::path path;
    std::uintmax_t size{};
};

bool isHexDigit(const char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

unsigned char hexValue(const char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned char>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned char>(value - 'a' + 10);
    }
    return static_cast<unsigned char>(value - 'A' + 10);
}

std::optional<std::string> decodeTargetPath(const std::string_view target) {
    const auto query = target.find('?');
    const std::string_view path = target.substr(0, query);
    if (path.empty() || path.front() != '/' || path.find('#') != std::string_view::npos) {
        return std::nullopt;
    }

    std::string decoded;
    decoded.reserve(path.size());
    for (std::size_t index = 0; index < path.size(); ++index) {
        unsigned char byte = static_cast<unsigned char>(path[index]);
        if (path[index] == '%') {
            if (index + 2 >= path.size() || !isHexDigit(path[index + 1]) || !isHexDigit(path[index + 2])) {
                return std::nullopt;
            }
            byte = static_cast<unsigned char>((hexValue(path[index + 1]) << 4) | hexValue(path[index + 2]));
            index += 2;
        }
        if (byte == 0 || byte == '\\' || byte < 0x20 || byte == 0x7f) {
            return std::nullopt;
        }
        decoded.push_back(static_cast<char>(byte));
    }

    std::size_t segmentStart = 1;
    while (segmentStart <= decoded.size()) {
        const std::size_t separator = decoded.find('/', segmentStart);
        const std::string_view segment{decoded.data() + segmentStart,
                                       (separator == std::string::npos ? decoded.size() : separator) - segmentStart};
        if (segment == "..") {
            return std::nullopt;
        }
        if (separator == std::string::npos) {
            break;
        }
        segmentStart = separator + 1;
    }
    return decoded;
}

bool isWithinRoot(const std::filesystem::path& root, const std::filesystem::path& candidate) noexcept {
    auto rootPart = root.begin();
    auto candidatePart = candidate.begin();
    for (; rootPart != root.end(); ++rootPart, ++candidatePart) {
        if (candidatePart == candidate.end() || *rootPart != *candidatePart) {
            return false;
        }
    }
    return true;
}

AssetResolution resolveAsset(const std::filesystem::path& root, const std::string_view target) {
    const auto decoded = decodeTargetPath(target);
    if (!decoded.has_value()) {
        return {.failure = AssetResolutionFailure::MALFORMED_TARGET};
    }

    std::filesystem::path relative = decoded->substr(1);
    if (relative.empty()) {
        relative = "index.html";
    }
    if (relative.is_absolute()) {
        return {.failure = AssetResolutionFailure::MALFORMED_TARGET};
    }

    std::error_code error;
    const auto candidate = std::filesystem::canonical(root / relative, error);
    if (error) {
        return {.failure = error == std::errc::no_such_file_or_directory ? AssetResolutionFailure::NOT_REGULAR
                                                                         : AssetResolutionFailure::FILESYSTEM_FAILURE};
    }
    if (!isWithinRoot(root, candidate)) {
        return {.failure = AssetResolutionFailure::OUTSIDE_ROOT};
    }
    if (!std::filesystem::is_regular_file(candidate, error)) {
        return {.failure = error ? AssetResolutionFailure::FILESYSTEM_FAILURE : AssetResolutionFailure::NOT_REGULAR};
    }
    const auto size = std::filesystem::file_size(candidate, error);
    if (error) {
        return {.failure = AssetResolutionFailure::FILESYSTEM_FAILURE};
    }
    return {.path = candidate, .size = size};
}

std::string_view mimeType(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension == ".html" || extension == ".htm") {
        return "text/html; charset=utf-8";
    }
    if (extension == ".css") {
        return "text/css; charset=utf-8";
    }
    if (extension == ".js" || extension == ".mjs") {
        return "text/javascript; charset=utf-8";
    }
    if (extension == ".json") {
        return "application/json";
    }
    if (extension == ".svg") {
        return "image/svg+xml";
    }
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".ico") {
        return "image/x-icon";
    }
    if (extension == ".wasm") {
        return "application/wasm";
    }
    if (extension == ".txt") {
        return "text/plain; charset=utf-8";
    }
    return "application/octet-stream";
}

int systemErrorValue(const boost::system::error_code& error) noexcept {
    return error.value();
}

class HttpsSession final : public std::enable_shared_from_this<HttpsSession> {
public:
    using Closed = std::function<void(HttpsSession*)>;

    HttpsSession(tcp::socket socket, ssl::context& tlsContext, const HttpsStaticServerConfiguration& configuration,
                 const std::filesystem::path& staticRoot, Closed closed)
        : stream_(std::move(socket), tlsContext),
          timer_(stream_.get_executor()),
          maximumHttpHeaderBytes_(configuration.maximumHttpHeaderBytes),
          maximumHttpBodyBytes_(configuration.maximumHttpBodyBytes),
          maximumStaticAssetBytes_(configuration.maximumStaticAssetBytes),
          tlsHandshakeTimeout_(configuration.tlsHandshakeTimeout),
          idleTimeout_(configuration.idleTimeout),
          staticRoot_(staticRoot),
          closed_(std::move(closed)) {}

    void start() {
        armTimer(tlsHandshakeTimeout_);
        stream_.async_handshake(
            ssl::stream_base::server,
            [self = shared_from_this()](const boost::system::error_code& error) { self->onHandshake(error); });
    }

    void stop(const bool notifyOwner = true) noexcept {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        boost::system::error_code ignored;
        cancelTimer();
        beast::get_lowest_layer(stream_).socket().cancel(ignored);
        beast::get_lowest_layer(stream_).socket().shutdown(tcp::socket::shutdown_both, ignored);
        beast::get_lowest_layer(stream_).socket().close(ignored);
        parser_.reset();
        response_.reset();
        if (notifyOwner && closed_) {
            auto closed = std::move(closed_);
            closed(this);
        } else {
            closed_ = {};
        }
    }

private:
    void armTimer(const std::chrono::milliseconds duration) {
        cancelTimer();
        timer_.expires_after(duration);
        timer_.async_wait([self = shared_from_this()](const boost::system::error_code& error) {
            if (!error) {
                self->stop();
            }
        });
    }

    void onHandshake(const boost::system::error_code& error) {
        if (stopped_) {
            return;
        }
        cancelTimer();
        if (error) {
            stop();
            return;
        }
        readRequest();
    }

    void readRequest() {
        if (stopped_) {
            return;
        }
        parser_.emplace();
        parser_->header_limit(static_cast<std::uint32_t>(maximumHttpHeaderBytes_));
        parser_->body_limit(maximumHttpBodyBytes_);
        armTimer(idleTimeout_);
        http::async_read(stream_, buffer_, *parser_,
                         [self = shared_from_this()](const boost::system::error_code& error, const std::size_t) {
                             self->onRead(error);
                         });
    }

    void onRead(const boost::system::error_code& error) {
        if (stopped_) {
            return;
        }
        cancelTimer();
        if (error == http::error::end_of_stream) {
            parser_.reset();
            shutdownTls();
            return;
        }
        if (error == http::error::header_limit) {
            parser_.reset();
            sendError(http::status::request_header_fields_too_large, "Request headers are too large\n");
            return;
        }
        if (error == http::error::body_limit) {
            parser_.reset();
            sendError(http::status::payload_too_large, "Request body is too large\n");
            return;
        }
        if (error) {
            parser_.reset();
            sendError(http::status::bad_request, "Malformed HTTP request\n");
            return;
        }

        auto request = parser_->release();
        parser_.reset();
        handleRequest(std::move(request));
    }

    void handleRequest(http::request<http::string_body> request) {
        const bool headOnly = request.method() == http::verb::head;
        if (websocket::is_upgrade(request)) {
            sendError(http::status::forbidden, "WebSocket authentication is unavailable\n", request.version(), false,
                      headOnly);
            return;
        }
        if (request.method() != http::verb::get && request.method() != http::verb::head) {
            sendError(http::status::method_not_allowed, "Only GET and HEAD are supported\n", request.version(),
                      request.keep_alive(), headOnly, true);
            return;
        }

        const auto asset =
            resolveAsset(staticRoot_, std::string_view{request.target().data(), request.target().size()});
        switch (asset.failure) {
            case AssetResolutionFailure::MALFORMED_TARGET:
                sendError(http::status::bad_request, "Invalid request target\n", request.version(),
                          request.keep_alive(), headOnly);
                return;
            case AssetResolutionFailure::OUTSIDE_ROOT:
                sendError(http::status::forbidden, "Asset is outside the configured root\n", request.version(),
                          request.keep_alive(), headOnly);
                return;
            case AssetResolutionFailure::NOT_REGULAR:
                sendError(http::status::not_found, "Asset not found\n", request.version(), request.keep_alive(),
                          headOnly);
                return;
            case AssetResolutionFailure::FILESYSTEM_FAILURE:
                sendError(http::status::internal_server_error, "Asset lookup failed\n", request.version(), false,
                          headOnly);
                return;
            case AssetResolutionFailure::NONE:
                break;
        }
        if (asset.size > maximumStaticAssetBytes_ ||
            asset.size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
            sendError(http::status::payload_too_large, "Asset exceeds the configured limit\n", request.version(),
                      request.keep_alive(), headOnly);
            return;
        }

        std::string body;
        if (!headOnly) {
            body.resize(static_cast<std::size_t>(asset.size));
            std::ifstream input(asset.path, std::ios::binary);
            if (!input || (asset.size != 0 && !input.read(body.data(), static_cast<std::streamsize>(body.size())))) {
                sendError(http::status::internal_server_error, "Asset read failed\n", request.version(), false, false);
                return;
            }
        }

        response_.emplace(http::status::ok, request.version());
        response_->set(http::field::server, "Financial-Exchange");
        response_->set(http::field::content_type, mimeType(asset.path));
        response_->keep_alive(request.keep_alive());
        response_->body() = std::move(body);
        response_->content_length(asset.size);
        writeResponse();
    }

    void sendError(const http::status status, const std::string_view body, const unsigned version = 11,
                   const bool keepAlive = false, const bool headOnly = false, const bool includeAllow = false) {
        response_.emplace(status, version);
        response_->set(http::field::server, "Financial-Exchange");
        response_->set(http::field::content_type, "text/plain; charset=utf-8");
        if (includeAllow) {
            response_->set(http::field::allow, "GET, HEAD");
        }
        response_->keep_alive(keepAlive);
        if (!headOnly) {
            response_->body() = body;
        }
        response_->content_length(body.size());
        writeResponse();
    }

    void writeResponse() {
        closeAfterWrite_ = !response_->keep_alive();
        armTimer(idleTimeout_);
        http::async_write(stream_, *response_,
                          [self = shared_from_this()](const boost::system::error_code& error, const std::size_t) {
                              self->onWrite(error);
                          });
    }

    void onWrite(const boost::system::error_code& error) {
        if (stopped_) {
            return;
        }
        cancelTimer();
        response_.reset();
        if (error) {
            stop();
            return;
        }
        if (closeAfterWrite_) {
            shutdownTls();
            return;
        }
        readRequest();
    }

    void shutdownTls() {
        if (stopped_) {
            return;
        }
        armTimer(idleTimeout_);
        stream_.async_shutdown([self = shared_from_this()](const boost::system::error_code&) { self->stop(); });
    }

    void cancelTimer() noexcept {
        try {
            static_cast<void>(timer_.cancel());
        } catch (...) {
        }
    }

    beast::ssl_stream<beast::tcp_stream> stream_;
    asio::steady_timer timer_;
    std::size_t maximumHttpHeaderBytes_;
    std::size_t maximumHttpBodyBytes_;
    std::size_t maximumStaticAssetBytes_;
    std::chrono::milliseconds tlsHandshakeTimeout_;
    std::chrono::milliseconds idleTimeout_;
    std::filesystem::path staticRoot_;
    Closed closed_;
    beast::flat_buffer buffer_;
    std::optional<http::request_parser<http::string_body>> parser_;
    std::optional<http::response<http::string_body>> response_;
    bool closeAfterWrite_{false};
    bool stopped_{false};
};

} // namespace

class HttpsStaticServer::Implementation final : public std::enable_shared_from_this<Implementation> {
public:
    Implementation(asio::io_context& ioContext, HttpsStaticServerConfiguration configuration)
        : configuration_(std::move(configuration)), tlsContext_(ssl::context::tls_server), acceptor_(ioContext) {}

    ~Implementation() {
        stop();
    }

    HttpsStaticServerStartResult start() noexcept {
        if (startedOnce_) {
            return {.failure = HttpsStaticServerStartFailure::ALREADY_STARTED};
        }
        startedOnce_ = true;
        try {
            boost::system::error_code error;
            const auto address = asio::ip::make_address(configuration_.listenAddress, error);
            if (error || configuration_.listenAddress.empty() || configuration_.staticAssetRoot.empty() ||
                configuration_.certificateChainPath.empty() || configuration_.privateKeyPath.empty() ||
                configuration_.maximumConnections == 0 || configuration_.maximumHttpHeaderBytes == 0 ||
                configuration_.maximumHttpHeaderBytes > std::numeric_limits<std::uint32_t>::max() ||
                configuration_.maximumHttpBodyBytes == 0 || configuration_.maximumStaticAssetBytes == 0 ||
                configuration_.maximumStaticAssetBytes >
                    static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()) ||
                configuration_.tlsHandshakeTimeout.count() <= 0 || configuration_.idleTimeout.count() <= 0) {
                return {.failure = HttpsStaticServerStartFailure::INVALID_CONFIGURATION,
                        .systemError = systemErrorValue(error)};
            }

            staticRoot_ = std::filesystem::canonical(configuration_.staticAssetRoot, error);
            if (error || !std::filesystem::is_directory(staticRoot_, error) || error) {
                return {.failure = HttpsStaticServerStartFailure::STATIC_ASSET_ROOT_INVALID,
                        .systemError = systemErrorValue(error)};
            }

            const auto privateKeyPath = std::filesystem::canonical(configuration_.privateKeyPath, error);
            if (error) {
                return {.failure = HttpsStaticServerStartFailure::PRIVATE_KEY_LOAD_FAILED,
                        .systemError = systemErrorValue(error)};
            }
            if (isWithinRoot(staticRoot_, privateKeyPath)) {
                return {.failure = HttpsStaticServerStartFailure::PRIVATE_KEY_WITHIN_STATIC_ROOT};
            }

            if (SSL_CTX_set_min_proto_version(tlsContext_.native_handle(), TLS1_2_VERSION) != 1) {
                return {.failure = HttpsStaticServerStartFailure::TLS_CONFIGURATION_FAILED};
            }
            SSL_CTX_set_options(tlsContext_.native_handle(), SSL_OP_NO_COMPRESSION);
            tlsContext_.use_certificate_chain_file(configuration_.certificateChainPath.string(), error);
            if (error) {
                return {.failure = HttpsStaticServerStartFailure::CERTIFICATE_CHAIN_LOAD_FAILED,
                        .systemError = systemErrorValue(error)};
            }
            tlsContext_.use_private_key_file(privateKeyPath.string(), ssl::context::file_format::pem, error);
            if (error) {
                return {.failure = HttpsStaticServerStartFailure::PRIVATE_KEY_LOAD_FAILED,
                        .systemError = systemErrorValue(error)};
            }
            if (SSL_CTX_check_private_key(tlsContext_.native_handle()) != 1) {
                return {.failure = HttpsStaticServerStartFailure::PRIVATE_KEY_LOAD_FAILED};
            }

            const tcp::endpoint endpoint{address, configuration_.listenPort};
            acceptor_.open(endpoint.protocol(), error);
            if (error) {
                return acceptorFailure(HttpsStaticServerStartFailure::ACCEPTOR_OPEN_FAILED, error);
            }
            acceptor_.set_option(asio::socket_base::reuse_address(true), error);
            if (error) {
                return acceptorFailure(HttpsStaticServerStartFailure::ACCEPTOR_OPEN_FAILED, error);
            }
            acceptor_.bind(endpoint, error);
            if (error) {
                return acceptorFailure(HttpsStaticServerStartFailure::ACCEPTOR_BIND_FAILED, error);
            }
            const std::size_t boundedBacklog = std::min(
                configuration_.maximumConnections, static_cast<std::size_t>(asio::socket_base::max_listen_connections));
            acceptor_.listen(static_cast<int>(boundedBacklog), error);
            if (error) {
                return acceptorFailure(HttpsStaticServerStartFailure::ACCEPTOR_LISTEN_FAILED, error);
            }
            const auto local = acceptor_.local_endpoint(error);
            if (error) {
                return acceptorFailure(HttpsStaticServerStartFailure::ACCEPTOR_LISTEN_FAILED, error);
            }
            localEndpoint_ = LocalHttpsEndpoint{.address = local.address().to_string(), .port = local.port()};
            ready_ = true;
            acceptNext();
            return {};
        } catch (...) {
            stop();
            return {.failure = HttpsStaticServerStartFailure::INTERNAL_FAILURE};
        }
    }

    void stop() noexcept {
        ready_ = false;
        localEndpoint_.reset();
        closeAcceptor();
        auto sessions = std::move(sessions_);
        sessions_.clear();
        for (auto& session : sessions) {
            session->stop(false);
        }
    }

    bool ready() const noexcept {
        return ready_;
    }

    std::optional<LocalHttpsEndpoint> localEndpoint() const {
        return localEndpoint_;
    }

private:
    HttpsStaticServerStartResult acceptorFailure(const HttpsStaticServerStartFailure failure,
                                                 const boost::system::error_code& error) noexcept {
        closeAcceptor();
        return {.failure = failure, .systemError = systemErrorValue(error)};
    }

    void closeAcceptor() noexcept {
        boost::system::error_code ignored;
        acceptor_.cancel(ignored);
        acceptor_.close(ignored);
    }

    void acceptNext() {
        if (!ready_ || sessions_.size() >= configuration_.maximumConnections || acceptPending_) {
            return;
        }
        acceptPending_ = true;
        acceptor_.async_accept([self = shared_from_this()](const boost::system::error_code& error, tcp::socket socket) {
            self->acceptPending_ = false;
            self->onAccept(error, std::move(socket));
        });
    }

    void onAccept(const boost::system::error_code& error, tcp::socket socket) {
        if (!ready_) {
            return;
        }
        if (error) {
            ready_ = false;
            localEndpoint_.reset();
            closeAcceptor();
            return;
        }

        std::weak_ptr<Implementation> weakSelf = shared_from_this();
        auto session = std::make_shared<HttpsSession>(std::move(socket), tlsContext_, configuration_, staticRoot_,
                                                      [weakSelf](HttpsSession* closedSession) {
                                                          if (const auto self = weakSelf.lock()) {
                                                              self->sessionClosed(closedSession);
                                                          }
                                                      });
        sessions_.push_back(session);
        session->start();
        acceptNext();
    }

    void sessionClosed(HttpsSession* closedSession) {
        std::erase_if(sessions_, [closedSession](const std::shared_ptr<HttpsSession>& session) {
            return session.get() == closedSession;
        });
        acceptNext();
    }

    HttpsStaticServerConfiguration configuration_;
    ssl::context tlsContext_;
    tcp::acceptor acceptor_;
    std::filesystem::path staticRoot_;
    std::optional<LocalHttpsEndpoint> localEndpoint_{};
    std::vector<std::shared_ptr<HttpsSession>> sessions_;
    bool startedOnce_{false};
    bool ready_{false};
    bool acceptPending_{false};
};

HttpsStaticServer::HttpsStaticServer(asio::io_context& ioContext, HttpsStaticServerConfiguration configuration)
    : implementation_(std::make_shared<Implementation>(ioContext, std::move(configuration))) {}

HttpsStaticServer::~HttpsStaticServer() {
    stop();
}

HttpsStaticServerStartResult HttpsStaticServer::start() noexcept {
    return implementation_->start();
}

void HttpsStaticServer::stop() noexcept {
    implementation_->stop();
}

bool HttpsStaticServer::ready() const noexcept {
    return implementation_->ready();
}

std::optional<LocalHttpsEndpoint> HttpsStaticServer::localEndpoint() const {
    return implementation_->localEndpoint();
}

} // namespace exchange::application_event
