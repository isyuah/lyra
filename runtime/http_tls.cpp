#include "http_transport.h"
#include "strings.h"
#include "tasks.h"

#include <cerrno>
#include <memory>
#include <new>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <string_view>

namespace {

namespace http = lyraRuntime::http;
namespace net = lyraRuntime::net;

struct Connection { net::Socket socket; SSL* ssl; int status{}; };

int receive(BIO* bio, char* bytes, int size) {
    BIO_clear_retry_flags(bio);
    auto& connection = *static_cast<Connection*>(BIO_get_data(bio));
    auto count = net::receive(connection.socket, bytes, size);
    connection.status = count < 0 ? net::error() : 0;
    if (count < 0 && net::retry(connection.status)) BIO_set_retry_read(bio);
    return count;
}

int send(BIO* bio, const char* bytes, int size) {
    BIO_clear_retry_flags(bio);
    auto& connection = *static_cast<Connection*>(BIO_get_data(bio));
    auto count = net::send(connection.socket, bytes, size);
    connection.status = count < 0 ? net::error() : 0;
    if (count < 0 && net::retry(connection.status)) BIO_set_retry_write(bio);
    return count;
}

const BIO_METHOD* socketBio() {
    static const auto* method = [] {
        auto* value = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "Lyra TCP");
        if (!value) return value;
        if (!BIO_meth_set_read(value, receive) || !BIO_meth_set_write(value, send) ||
            !BIO_meth_set_ctrl(value, [](BIO*, int command, long, void*) -> long { return command == BIO_CTRL_FLUSH; })) {
            BIO_meth_free(value);
            return static_cast<BIO_METHOD*>(nullptr);
        }
        return value;
    }();
    return method;
}

int open(void* context, net::Socket socket, void** result) {
    auto connection = std::unique_ptr<Connection>(new (std::nothrow) Connection{socket, nullptr});
    if (!connection) return ENOMEM;
    auto* ssl = SSL_new(static_cast<SSL_CTX*>(context));
    if (!ssl) return http::handshakeFailed;
    auto* bio = BIO_new(socketBio());
    if (!bio) { SSL_free(ssl); return ENOMEM; }
    connection->ssl = ssl;
    BIO_set_init(bio, 1);
    BIO_set_data(bio, connection.get());
    SSL_set_bio(ssl, bio, bio);
    SSL_set_accept_state(ssl);
    *result = connection.release();
    return 0;
}

int drive(Connection& connection, const auto& operation, int* count, std::int64_t deadline, bool handshaking = false) {
    for (;;) {
        if (lyraBlockingCancelled()) return ECANCELED;
        if (http::now() >= deadline) return ETIMEDOUT;
        ERR_clear_error();
        connection.status = 0;
        auto result = operation(connection.ssl);
        if (result > 0) { *count = result; return 0; }
        auto error = SSL_get_error(connection.ssl, result);
        if (!handshaking && error == SSL_ERROR_ZERO_RETURN) { *count = 0; return 0; }
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            auto ready = http::waitFor(connection.socket, error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline);
            if (!ready) continue;
            return ready;
        }
        if (handshaking) return http::handshakeFailed;
        return error == SSL_ERROR_SYSCALL && connection.status ? connection.status : ECONNRESET;
    }
}

int handshake(void* state, std::int64_t deadline) {
    int unused;
    return drive(*static_cast<Connection*>(state), SSL_accept, &unused, deadline, true);
}

int read(void* state, char* bytes, int size, int* count, std::int64_t deadline) {
    return drive(*static_cast<Connection*>(state), [&](auto* ssl) { return SSL_read(ssl, bytes, size); }, count, deadline);
}

int write(void* state, const char* bytes, int size, int* count, std::int64_t deadline) {
    return drive(*static_cast<Connection*>(state), [&](auto* ssl) { return SSL_write(ssl, bytes, size); }, count, deadline);
}

void shutdown(void* state, std::int64_t deadline) {
    auto& connection = *static_cast<Connection*>(state);
    while (!lyraBlockingCancelled() && http::now() < deadline) {
        ERR_clear_error();
        auto result = SSL_shutdown(connection.ssl);
        if (result >= 0 || SSL_get_error(connection.ssl, result) != SSL_ERROR_WANT_WRITE ||
            http::waitFor(connection.socket, POLLOUT, deadline)) return;
    }
}

void releaseConnection(void* state) {
    auto* connection = static_cast<Connection*>(state);
    SSL_free(connection->ssl);
    delete connection;
}

void releaseListener(void* state) { SSL_CTX_free(static_cast<SSL_CTX*>(state)); }

constexpr http::Transport transport{open, handshake, read, write, shutdown, releaseConnection, releaseListener};

bool trim(std::string_view& pem) {
    auto begin = pem.find_first_not_of(" \t\r\n");
    pem.remove_prefix(begin == pem.npos ? pem.size() : begin);
    return !pem.empty();
}

bool loadChain(SSL_CTX* context, std::string_view pem) {
    constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----", end = "-----END CERTIFICATE-----";
    bool leaf = true;
    while (trim(pem)) {
        if (!pem.starts_with(begin)) return false;
        auto marker = pem.find(end, begin.size());
        if (marker == pem.npos) return false;
        auto length = marker + end.size();
        auto input = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new_mem_buf(pem.data(), static_cast<int>(length)), BIO_free);
        if (!input) return false;
        auto cert = std::unique_ptr<X509, decltype(&X509_free)>(PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr), X509_free);
        if (!cert) return false;
        if (leaf) {
            if (SSL_CTX_use_certificate(context, cert.get()) != 1) return false;
            leaf = false;
        } else if (SSL_CTX_add1_chain_cert(context, cert.get()) != 1) return false;
        pem.remove_prefix(length);
    }
    return !leaf;
}

bool loadKey(SSL_CTX* context, std::string_view pem) {
    if (!trim(pem) || !pem.starts_with("-----BEGIN ")) return false;
    auto input = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    if (!input) return false;
    auto key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(PEM_read_bio_PrivateKey(input.get(), nullptr,
        [](char*, int, int, void*) { return 0; }, nullptr), EVP_PKEY_free);
    if (!key) return false;
    pem.remove_prefix(pem.size() - BIO_ctrl_pending(input.get()));
    return !trim(pem) && SSL_CTX_use_PrivateKey(context, key.get()) == 1 && SSL_CTX_check_private_key(context) == 1;
}

void* configurationError(std::string_view detail) {
    return lyraHttpListenFailure(http::tlsConfig, lyraStringFromBytes(detail.data(), detail.size()));
}

}

extern "C" void* lyraHttpTlsListen(const void* host, std::int64_t port, const void* chain, const void* key, std::int64_t handshakeTimeout) {
    constexpr std::size_t pemLimit = 4 * 1024 * 1024;
    auto chainSize = lyraStringLength(chain), keySize = lyraStringLength(key);
    if (chainSize == 0 || chainSize > pemLimit || keySize == 0 || keySize > pemLimit)
        return configurationError("certificate chain and private key must each be 1 byte to 4 MiB");
    if (handshakeTimeout < 1 || handshakeTimeout > 600000) return configurationError("handshake timeout must be in 1..600000 ms");
    if (OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr) != 1 || !socketBio()) return configurationError("cannot initialize TLS");
    auto context = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    if (!context || SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(context.get(), TLS1_3_VERSION) != 1) return configurationError("cannot configure TLS 1.2/1.3");
    SSL_CTX_set_options(context.get(), SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    if (!loadChain(context.get(), {lyraStringBytes(chain), chainSize})) return configurationError("invalid PEM certificate chain");
    if (!loadKey(context.get(), {lyraStringBytes(key), keySize})) return configurationError("invalid or mismatched PEM private key");
    return lyraHttpListenWithTransport(host, port, &transport, context.release(), handshakeTimeout);
}
