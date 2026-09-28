#include "https_static_server.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

namespace exchange::application_event {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

template <typename Type, void (*Release)(Type*)>
using OpenSslOwner = std::unique_ptr<Type, decltype(Release)>;

struct TestTlsMaterial final {
    std::filesystem::path certificate;
    std::filesystem::path privateKey;
};

TestTlsMaterial generateTlsMaterial(const std::filesystem::path& directory, const std::string_view name) {
    OpenSslOwner<EVP_PKEY_CTX, EVP_PKEY_CTX_free> keyContext{EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr),
                                                             EVP_PKEY_CTX_free};
    if (!keyContext || EVP_PKEY_keygen_init(keyContext.get()) != 1 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(keyContext.get(), 2048) != 1) {
        throw std::runtime_error("failed to initialize test key generation");
    }
    EVP_PKEY* rawKey = nullptr;
    if (EVP_PKEY_keygen(keyContext.get(), &rawKey) != 1) {
        throw std::runtime_error("failed to generate test key");
    }
    OpenSslOwner<EVP_PKEY, EVP_PKEY_free> key{rawKey, EVP_PKEY_free};
    OpenSslOwner<X509, X509_free> certificate{X509_new(), X509_free};
    if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
        X509_gmtime_adj(X509_get_notBefore(certificate.get()), 0) == nullptr ||
        X509_gmtime_adj(X509_get_notAfter(certificate.get()), 24 * 60 * 60) == nullptr ||
        X509_set_pubkey(certificate.get(), key.get()) != 1) {
        throw std::runtime_error("failed to initialize test certificate");
    }
    X509_NAME* subject = X509_get_subject_name(certificate.get());
    constexpr unsigned char COMMON_NAME[] = "localhost";
    if (subject == nullptr || X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, COMMON_NAME, -1, -1, 0) != 1 ||
        X509_set_issuer_name(certificate.get(), subject) != 1 ||
        X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) {
        throw std::runtime_error("failed to sign test certificate");
    }

    const TestTlsMaterial output{.certificate = directory / (std::string{name} + ".crt.pem"),
                                 .privateKey = directory / (std::string{name} + ".key.pem")};
    std::unique_ptr<FILE, decltype(&std::fclose)> certificateFile{std::fopen(output.certificate.c_str(), "wb"),
                                                                  &std::fclose};
    std::unique_ptr<FILE, decltype(&std::fclose)> keyFile{std::fopen(output.privateKey.c_str(), "wb"), &std::fclose};
    if (!certificateFile || !keyFile || PEM_write_X509(certificateFile.get(), certificate.get()) != 1 ||
        PEM_write_PrivateKey(keyFile.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        throw std::runtime_error("failed to write test TLS material");
    }
    return output;
}

class RunningHttpsServer final {
public:
    explicit RunningHttpsServer(HttpsStaticServerConfiguration configuration)
        : server_(ioContext_, std::move(configuration)) {}

    ~RunningHttpsServer() {
        stopAndJoin();
    }

    HttpsStaticServerStartResult start() {
        const auto result = server_.start();
        if (result.failure == HttpsStaticServerStartFailure::NONE) {
            thread_ = std::thread([this]() { ioContext_.run(); });
        }
        return result;
    }

    LocalHttpsEndpoint endpoint() const {
        const auto endpoint = server_.localEndpoint();
        if (!endpoint.has_value()) {
            throw std::logic_error("test server has no local endpoint");
        }
        return *endpoint;
    }

    template <typename Function>
    auto invoke(Function function) {
        using Result = std::invoke_result_t<Function, HttpsStaticServer&>;
        std::promise<Result> promise;
        auto future = promise.get_future();
        asio::post(ioContext_, [this, function = std::move(function), promise = std::move(promise)]() mutable {
            if constexpr (std::is_void_v<Result>) {
                function(server_);
                promise.set_value();
            } else {
                promise.set_value(function(server_));
            }
        });
        return future.get();
    }

    void stopAndJoin() {
        if (!thread_.joinable()) {
            server_.stop();
            return;
        }
        invoke([](HttpsStaticServer& server) { server.stop(); });
        thread_.join();
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool ioStopped() const noexcept {
        return ioContext_.stopped();
    }

private:
    asio::io_context ioContext_;
    HttpsStaticServer server_;
    std::thread thread_;
};

class TlsClient final {
public:
    explicit TlsClient(const LocalHttpsEndpoint& endpoint)
        : context_(ssl::context::tls_client), stream_(ioContext_, context_) {
        context_.set_verify_mode(ssl::verify_none);
        if (SSL_CTX_set_min_proto_version(context_.native_handle(), TLS1_2_VERSION) != 1) {
            throw std::runtime_error("failed to configure test TLS client");
        }
        beast::get_lowest_layer(stream_).connect(
            tcp::endpoint{asio::ip::make_address(endpoint.address), endpoint.port});
        stream_.handshake(ssl::stream_base::client);
    }

    ~TlsClient() {
        close();
    }

    http::response<http::string_body> request(const http::verb method, const std::string_view target,
                                              std::string body = {}, const bool keepAlive = false) {
        http::request<http::string_body> request{method, target, 11};
        request.set(http::field::host, "localhost");
        request.keep_alive(keepAlive);
        request.body() = std::move(body);
        if (!request.body().empty()) {
            request.prepare_payload();
        }
        http::write(stream_, request);
        return readResponse(method == http::verb::head);
    }

    void writeRaw(const std::string_view request) {
        asio::write(stream_, asio::buffer(request));
    }

    http::response<http::string_body> readResponse(const bool skipBody = false) {
        http::response_parser<http::string_body> parser;
        parser.skip(skipBody);
        http::read(stream_, buffer_, parser);
        return parser.release();
    }

    std::string protocolVersion() {
        return SSL_get_version(stream_.native_handle());
    }

    void close() noexcept {
        boost::system::error_code ignored;
        beast::get_lowest_layer(stream_).socket().shutdown(tcp::socket::shutdown_both, ignored);
        beast::get_lowest_layer(stream_).socket().close(ignored);
    }

private:
    asio::io_context ioContext_;
    ssl::context context_;
    beast::ssl_stream<beast::tcp_stream> stream_;
    beast::flat_buffer buffer_;
};

class HttpsStaticServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::array<char, 64> buffer{};
        constexpr char TEMPLATE[] = "/tmp/exchange-https-static-XXXXXX";
        std::copy(std::begin(TEMPLATE), std::end(TEMPLATE), buffer.begin());
        const char* created = ::mkdtemp(buffer.data());
        ASSERT_NE(created, nullptr);
        directory_ = created;
        assets_ = directory_ / "assets";
        ASSERT_TRUE(std::filesystem::create_directory(assets_));
        write(assets_ / "index.html", "<html>exchange</html>");
        write(assets_ / "app.js", "console.log('exchange');");
        write(assets_ / "opaque.data", "opaque");
        write(assets_ / "large.txt", std::string(33, 'x'));
        ASSERT_TRUE(std::filesystem::create_directory(assets_ / "directory"));
        write(directory_ / "outside.txt", "outside");
        std::error_code error;
        std::filesystem::create_symlink(directory_ / "outside.txt", assets_ / "escape.txt", error);
        ASSERT_FALSE(error);
        ASSERT_EQ(::mkfifo((assets_ / "pipe").c_str(), 0600), 0);
        tls_ = generateTlsMaterial(directory_, "server");
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
        EXPECT_FALSE(error);
    }

    static void write(const std::filesystem::path& path, const std::string_view contents) {
        std::ofstream output(path, std::ios::binary);
        ASSERT_TRUE(output);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        ASSERT_TRUE(output);
    }

    HttpsStaticServerConfiguration configuration() const {
        return {
            .listenAddress = "127.0.0.1",
            .listenPort = 0,
            .staticAssetRoot = assets_,
            .certificateChainPath = tls_.certificate,
            .privateKeyPath = tls_.privateKey,
            .maximumConnections = 4,
            .maximumHttpHeaderBytes = 512,
            .maximumHttpBodyBytes = 32,
            .maximumStaticAssetBytes = 32,
            .tlsHandshakeTimeout = 1s,
            .idleTimeout = 1s,
        };
    }

    static HttpsStaticServerStartResult startOnce(HttpsStaticServerConfiguration configuration, bool& ready,
                                                  std::optional<LocalHttpsEndpoint>& endpoint) {
        asio::io_context ioContext;
        HttpsStaticServer server(ioContext, std::move(configuration));
        const auto result = server.start();
        ready = server.ready();
        endpoint = server.localEndpoint();
        server.stop();
        ioContext.poll();
        return result;
    }

    std::filesystem::path directory_;
    std::filesystem::path assets_;
    TestTlsMaterial tls_;
};

TEST_F(HttpsStaticServerTest, RejectsInvalidOrZeroConfigurationBeforeBinding) {
    std::vector<HttpsStaticServerConfiguration> invalid;
    auto add = [this, &invalid](const auto mutation) {
        auto value = configuration();
        mutation(value);
        invalid.push_back(std::move(value));
    };
    add([](auto& value) { value.listenAddress.clear(); });
    add([](auto& value) { value.staticAssetRoot.clear(); });
    add([](auto& value) { value.certificateChainPath.clear(); });
    add([](auto& value) { value.privateKeyPath.clear(); });
    add([](auto& value) { value.maximumConnections = 0; });
    add([](auto& value) { value.maximumHttpHeaderBytes = 0; });
    add([](auto& value) { value.maximumHttpBodyBytes = 0; });
    add([](auto& value) { value.maximumStaticAssetBytes = 0; });
    add([](auto& value) { value.tlsHandshakeTimeout = 0ms; });
    add([](auto& value) { value.idleTimeout = 0ms; });

    for (auto& value : invalid) {
        bool ready = true;
        std::optional<LocalHttpsEndpoint> endpoint = LocalHttpsEndpoint{};
        const auto result = startOnce(std::move(value), ready, endpoint);
        EXPECT_EQ(result.failure, HttpsStaticServerStartFailure::INVALID_CONFIGURATION);
        EXPECT_FALSE(ready);
        EXPECT_FALSE(endpoint.has_value());
    }

    auto missingRoot = configuration();
    missingRoot.staticAssetRoot = directory_ / "missing-assets";
    bool ready = true;
    std::optional<LocalHttpsEndpoint> endpoint = LocalHttpsEndpoint{};
    EXPECT_EQ(startOnce(std::move(missingRoot), ready, endpoint).failure,
              HttpsStaticServerStartFailure::STATIC_ASSET_ROOT_INVALID);
    EXPECT_FALSE(ready);
    EXPECT_FALSE(endpoint.has_value());
}

TEST_F(HttpsStaticServerTest, RejectsMissingMalformedAndMismatchedTlsMaterialBeforeBinding) {
    auto expectFailure = [this](HttpsStaticServerConfiguration value, const HttpsStaticServerStartFailure expected) {
        bool ready = true;
        std::optional<LocalHttpsEndpoint> endpoint = LocalHttpsEndpoint{};
        EXPECT_EQ(startOnce(std::move(value), ready, endpoint).failure, expected);
        EXPECT_FALSE(ready);
        EXPECT_FALSE(endpoint.has_value());
    };

    auto missingCertificate = configuration();
    missingCertificate.certificateChainPath = directory_ / "missing.crt";
    expectFailure(std::move(missingCertificate), HttpsStaticServerStartFailure::CERTIFICATE_CHAIN_LOAD_FAILED);

    auto malformedCertificate = configuration();
    malformedCertificate.certificateChainPath = directory_ / "malformed.crt";
    write(malformedCertificate.certificateChainPath, "not a certificate");
    expectFailure(std::move(malformedCertificate), HttpsStaticServerStartFailure::CERTIFICATE_CHAIN_LOAD_FAILED);

    auto missingKey = configuration();
    missingKey.privateKeyPath = directory_ / "missing.key";
    expectFailure(std::move(missingKey), HttpsStaticServerStartFailure::PRIVATE_KEY_LOAD_FAILED);

    auto malformedKey = configuration();
    malformedKey.privateKeyPath = directory_ / "malformed.key";
    write(malformedKey.privateKeyPath, "not a key");
    expectFailure(std::move(malformedKey), HttpsStaticServerStartFailure::PRIVATE_KEY_LOAD_FAILED);

    const auto exposedKey = assets_ / "server.key.pem";
    ASSERT_TRUE(std::filesystem::copy_file(tls_.privateKey, exposedKey));
    auto keyWithinStaticRoot = configuration();
    keyWithinStaticRoot.privateKeyPath = exposedKey;
    expectFailure(std::move(keyWithinStaticRoot), HttpsStaticServerStartFailure::PRIVATE_KEY_WITHIN_STATIC_ROOT);

    const auto other = generateTlsMaterial(directory_, "other");
    auto mismatch = configuration();
    mismatch.privateKeyPath = other.privateKey;
    expectFailure(std::move(mismatch), HttpsStaticServerStartFailure::PRIVATE_KEY_LOAD_FAILED);
}

TEST_F(HttpsStaticServerTest, RefusesReadinessWhenBindFails) {
    RunningHttpsServer first(configuration());
    const auto firstStart = first.start();
    ASSERT_EQ(firstStart.failure, HttpsStaticServerStartFailure::NONE) << firstStart.systemError;
    auto secondConfiguration = configuration();
    secondConfiguration.listenPort = first.endpoint().port;
    bool ready = true;
    std::optional<LocalHttpsEndpoint> endpoint = LocalHttpsEndpoint{};
    EXPECT_EQ(startOnce(std::move(secondConfiguration), ready, endpoint).failure,
              HttpsStaticServerStartFailure::ACCEPTOR_BIND_FAILED);
    EXPECT_FALSE(ready);
    EXPECT_FALSE(endpoint.has_value());
}

TEST_F(HttpsStaticServerTest, ServesGetRootAndHeadOverTls12OrNewer) {
    RunningHttpsServer server(configuration());
    ASSERT_EQ(server.start().failure, HttpsStaticServerStartFailure::NONE);
    TlsClient client(server.endpoint());
    EXPECT_TRUE(client.protocolVersion() == "TLSv1.2" || client.protocolVersion() == "TLSv1.3");

    const auto get = client.request(http::verb::get, "/", {}, true);
    EXPECT_EQ(get.result(), http::status::ok);
    EXPECT_EQ(get[http::field::content_type], "text/html; charset=utf-8");
    EXPECT_EQ(get.body(), "<html>exchange</html>");

    const auto head = client.request(http::verb::head, "/", {}, false);
    EXPECT_EQ(head.result(), http::status::ok);
    EXPECT_TRUE(head.body().empty());
    EXPECT_EQ(head[http::field::content_length], "21");
}

TEST_F(HttpsStaticServerTest, IgnoresQueryAndUsesExplicitOrSafeDefaultMimeTypes) {
    RunningHttpsServer server(configuration());
    ASSERT_EQ(server.start().failure, HttpsStaticServerStartFailure::NONE);
    TlsClient client(server.endpoint());
    const auto javascript = client.request(http::verb::get, "/app.js?v=1", {}, true);
    EXPECT_EQ(javascript.result(), http::status::ok);
    EXPECT_EQ(javascript[http::field::content_type], "text/javascript; charset=utf-8");
    const auto opaque = client.request(http::verb::get, "/opaque.data");
    EXPECT_EQ(opaque.result(), http::status::ok);
    EXPECT_EQ(opaque[http::field::content_type], "application/octet-stream");
}

TEST_F(HttpsStaticServerTest, ReturnsBoundedErrorsForMissingOversizedAndUnsupportedRequests) {
    RunningHttpsServer server(configuration());
    ASSERT_EQ(server.start().failure, HttpsStaticServerStartFailure::NONE);
    TlsClient client(server.endpoint());
    const auto missing = client.request(http::verb::get, "/missing", {}, true);
    EXPECT_EQ(missing.result(), http::status::not_found);
    EXPECT_LT(missing.body().size(), 64U);
    const auto oversized = client.request(http::verb::get, "/large.txt", {}, true);
    EXPECT_EQ(oversized.result(), http::status::payload_too_large);
    EXPECT_LT(oversized.body().size(), 64U);
    const auto method = client.request(http::verb::post, "/", {}, false);
    EXPECT_EQ(method.result(), http::status::method_not_allowed);
    EXPECT_EQ(method[http::field::allow], "GET, HEAD");
}

TEST_F(HttpsStaticServerTest, EnforcesHeaderAndBodyLimitsBeforeServingAssets) {
    RunningHttpsServer server(configuration());
    ASSERT_EQ(server.start().failure, HttpsStaticServerStartFailure::NONE);
    TlsClient headerClient(server.endpoint());
    headerClient.writeRaw("GET / HTTP/1.1\r\nHost: localhost\r\nX-Large: " + std::string(600, 'x') + "\r\n\r\n");
    EXPECT_EQ(headerClient.readResponse().result(), http::status::request_header_fields_too_large);

    TlsClient bodyClient(server.endpoint());
    bodyClient.writeRaw("GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 33\r\nConnection: close\r\n\r\n" +
                        std::string(33, 'x'));
    EXPECT_EQ(bodyClient.readResponse().result(), http::status::payload_too_large);
}

TEST_F(HttpsStaticServerTest, PlainHttpOnTlsPortNeverReceivesAnHttpResponse) {
    RunningHttpsServer server(configuration());
    ASSERT_EQ(server.start().failure, HttpsStaticServerStartFailure::NONE);
    asio::io_context clientIo;
    beast::tcp_stream stream(clientIo);
    const auto endpoint = server.endpoint();
    stream.connect(tcp::endpoint{asio::ip::make_address(endpoint.address), endpoint.port});
    asio::write(stream.socket(), asio::buffer("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    stream.expires_after(1s);
    std::array<char, 64> response{};
    boost::system::error_code error;
    const std::size_t bytes = stream.read_some(asio::buffer(response), error);
    static_cast<void>(error);
    EXPECT_FALSE((std::string_view{response.data(), bytes}.starts_with("HTTP/")));
}

TEST_F(HttpsStaticServerTest, RejectsTraversalMalformedAndNonRegularAssetTargets) {
    RunningHttpsServer server(configuration());
    ASSERT_EQ(server.start().failure, HttpsStaticServerStartFailure::NONE);
    const std::array<std::pair<std::string_view, http::status>, 8> cases{{
        {"/../outside.txt", http::status::bad_request},
        {"/%2e%2e/outside.txt", http::status::bad_request},
        {"/..\\outside.txt", http::status::bad_request},
        {"/%00", http::status::bad_request},
        {"/%2", http::status::bad_request},
        {"/directory", http::status::not_found},
        {"/escape.txt", http::status::forbidden},
        {"/pipe", http::status::not_found},
    }};
    for (const auto& [target, expected] : cases) {
        TlsClient client(server.endpoint());
        EXPECT_EQ(client.request(http::verb::get, target).result(), expected) << target;
    }
}

TEST_F(HttpsStaticServerTest, RejectsUnauthenticatedWebSocketUpgrade) {
    RunningHttpsServer server(configuration());
    ASSERT_EQ(server.start().failure, HttpsStaticServerStartFailure::NONE);
    TlsClient client(server.endpoint());
    client.writeRaw(
        "GET /events HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    EXPECT_EQ(client.readResponse().result(), http::status::forbidden);
}

TEST_F(HttpsStaticServerTest, MaximumConnectionsDefersAcceptanceUntilAConnectionCloses) {
    auto value = configuration();
    value.maximumConnections = 1;
    value.idleTimeout = 2s;
    RunningHttpsServer server(std::move(value));
    ASSERT_EQ(server.start().failure, HttpsStaticServerStartFailure::NONE);
    auto first = std::make_unique<TlsClient>(server.endpoint());

    auto secondFuture =
        std::async(std::launch::async, [&server]() { return std::make_unique<TlsClient>(server.endpoint()); });
    EXPECT_EQ(secondFuture.wait_for(50ms), std::future_status::timeout);
    first->close();
    first.reset();
    ASSERT_EQ(secondFuture.wait_for(2s), std::future_status::ready);
    auto second = secondFuture.get();
    second->close();
}

TEST_F(HttpsStaticServerTest, HandshakeAndIdleTimeoutsReleaseSessionOwnership) {
    auto handshakeConfiguration = configuration();
    handshakeConfiguration.maximumConnections = 1;
    handshakeConfiguration.tlsHandshakeTimeout = 250ms;
    RunningHttpsServer handshakeServer(std::move(handshakeConfiguration));
    ASSERT_EQ(handshakeServer.start().failure, HttpsStaticServerStartFailure::NONE);
    asio::io_context rawIo;
    tcp::socket rawSocket(rawIo);
    const auto handshakeEndpoint = handshakeServer.endpoint();
    rawSocket.connect(tcp::endpoint{asio::ip::make_address(handshakeEndpoint.address), handshakeEndpoint.port});
    auto afterHandshakeTimeout = std::async(
        std::launch::async, [&handshakeServer]() { return std::make_unique<TlsClient>(handshakeServer.endpoint()); });
    EXPECT_EQ(afterHandshakeTimeout.wait_for(50ms), std::future_status::timeout);
    ASSERT_EQ(afterHandshakeTimeout.wait_for(2s), std::future_status::ready);
    auto handshakeSuccessor = afterHandshakeTimeout.get();
    handshakeSuccessor->close();

    auto idleConfiguration = configuration();
    idleConfiguration.maximumConnections = 1;
    idleConfiguration.idleTimeout = 250ms;
    RunningHttpsServer idleServer(std::move(idleConfiguration));
    ASSERT_EQ(idleServer.start().failure, HttpsStaticServerStartFailure::NONE);
    auto idleClient = std::make_unique<TlsClient>(idleServer.endpoint());
    auto afterIdleTimeout =
        std::async(std::launch::async, [&idleServer]() { return std::make_unique<TlsClient>(idleServer.endpoint()); });
    EXPECT_EQ(afterIdleTimeout.wait_for(50ms), std::future_status::timeout);
    ASSERT_EQ(afterIdleTimeout.wait_for(2s), std::future_status::ready);
    auto idleSuccessor = afterIdleTimeout.get();
    idleSuccessor->close();
    idleClient->close();
}

TEST_F(HttpsStaticServerTest, StopClosesAcceptorTimersAndEveryActiveSession) {
    RunningHttpsServer server(configuration());
    ASSERT_EQ(server.start().failure, HttpsStaticServerStartFailure::NONE);
    TlsClient client(server.endpoint());
    const auto stopped = server.invoke([](HttpsStaticServer& running) {
        running.stop();
        return std::pair{running.ready(), running.localEndpoint()};
    });
    EXPECT_FALSE(stopped.first);
    EXPECT_FALSE(stopped.second.has_value());
    EXPECT_ANY_THROW(static_cast<void>(client.request(http::verb::get, "/")));
    server.join();
    EXPECT_TRUE(server.ioStopped());
}

} // namespace
} // namespace exchange::application_event
