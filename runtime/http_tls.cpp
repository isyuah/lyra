#include "http_transport.h"
#include "strings.h"

#include <algorithm>
#include <array>
#include <memory>
#include <new>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <string_view>

namespace {

namespace http = lyraRuntime::http;
using http::TlsProgress;
using http::TlsStep;

struct Connection { SSL* ssl; BIO* wire; };

void* open(void* context) {
    constexpr std::size_t bioLimit = 32 * 1024;
    auto connection = std::unique_ptr<Connection>(new (std::nothrow) Connection{});
    if (!connection) return nullptr;
    connection->ssl = SSL_new(static_cast<SSL_CTX*>(context));
    if (!connection->ssl) return nullptr;
    BIO* engine;
    if (BIO_new_bio_pair(&engine, bioLimit, &connection->wire, bioLimit) != 1) {
        SSL_free(connection->ssl);
        return nullptr;
    }
    SSL_set_bio(connection->ssl, engine, engine);
    SSL_set_accept_state(connection->ssl);
    SSL_set_mode(connection->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_RELEASE_BUFFERS);
    return connection.release();
}

std::size_t feed(void* state, const char* bytes, std::size_t size) {
    auto* wire = static_cast<Connection*>(state)->wire;
    auto available = std::min(size, BIO_ctrl_get_write_guarantee(wire));
    std::size_t consumed = 0;
    if (available) BIO_write_ex(wire, bytes, available, &consumed);
    return consumed;
}

std::size_t flush(void* state, char* bytes, std::size_t size) {
    auto* wire = static_cast<Connection*>(state)->wire;
    std::size_t count = 0;
    if (BIO_ctrl_pending(wire)) BIO_read_ex(wire, bytes, size, &count);
    return count;
}

void endInput(void* state) { BIO_shutdown_wr(static_cast<Connection*>(state)->wire); }

TlsStep progress(SSL* ssl, int result, std::size_t count = 0) {
    if (result > 0) return {TlsProgress::Done, count};
    switch (SSL_get_error(ssl, result)) {
        case SSL_ERROR_WANT_READ: return {TlsProgress::WantInput};
        case SSL_ERROR_WANT_WRITE: return {TlsProgress::WantOutput};
        case SSL_ERROR_ZERO_RETURN: return {TlsProgress::End};
        default: return {TlsProgress::Failed};
    }
}

TlsStep handshake(void* state) {
    auto* ssl = static_cast<Connection*>(state)->ssl;
    ERR_clear_error();
    return progress(ssl, SSL_accept(ssl));
}

TlsStep read(void* state, char* bytes, std::size_t size) {
    auto* ssl = static_cast<Connection*>(state)->ssl;
    std::size_t count = 0;
    ERR_clear_error();
    auto result = SSL_read_ex(ssl, bytes, size, &count);
    return progress(ssl, result, count);
}

TlsStep write(void* state, const char* bytes, std::size_t size) {
    auto* ssl = static_cast<Connection*>(state)->ssl;
    std::size_t count = 0;
    ERR_clear_error();
    auto result = SSL_write_ex(ssl, bytes, size, &count);
    return progress(ssl, result, count);
}

TlsStep shutdown(void* state) {
    auto* ssl = static_cast<Connection*>(state)->ssl;
    ERR_clear_error();
    auto result = SSL_shutdown(ssl);
    return result >= 0 ? TlsStep{TlsProgress::Done} : progress(ssl, result);
}

void releaseConnection(void* state) {
    auto* connection = static_cast<Connection*>(state);
    SSL_free(connection->ssl);
    BIO_free(connection->wire);
    delete connection;
}

void releaseListener(void* state) { SSL_CTX_free(static_cast<SSL_CTX*>(state)); }

bool http2(void* state) {
    const unsigned char* selected = nullptr;
    unsigned length = 0;
    SSL_get0_alpn_selected(static_cast<Connection*>(state)->ssl, &selected, &length);
    return length == 2 && selected[0] == 'h' && selected[1] == '2';
}

constexpr http::Transport transport{open, feed, flush, endInput, handshake, read, write, shutdown, releaseConnection, releaseListener, http2};

constexpr std::array<unsigned char, 12> protocols{2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

int negotiateProtocol(SSL*, const unsigned char** output, unsigned char* length, const unsigned char* input, unsigned size, void* first) {
    const auto* supported = static_cast<const unsigned char*>(first);
    auto remaining = static_cast<unsigned>(protocols.data() + protocols.size() - supported);
    unsigned char* selected = nullptr;
    if (SSL_select_next_proto(&selected, length, supported, remaining, input, size) != OPENSSL_NPN_NEGOTIATED)
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    *output = selected;
    return SSL_TLSEXT_ERR_OK;
}

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

extern "C" void* lyraHttpTlsListen(const void* host, std::int64_t port, const void* chain, const void* key, const http::ServerOptions* options) {
    constexpr std::size_t pemLimit = 4 * 1024 * 1024;
    auto chainSize = lyraStringLength(chain), keySize = lyraStringLength(key);
    if (chainSize == 0 || chainSize > pemLimit || keySize == 0 || keySize > pemLimit)
        return configurationError("certificate chain and private key must each be 1 byte to 4 MiB");
    if (OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr) != 1) return configurationError("cannot initialize TLS");
    auto context = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    if (!context || SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(context.get(), TLS1_3_VERSION) != 1) return configurationError("cannot configure TLS 1.2/1.3");
    SSL_CTX_set_options(context.get(), SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION | SSL_OP_CIPHER_SERVER_PREFERENCE);
    if (SSL_CTX_set_cipher_list(context.get(), "ECDHE+AESGCM:ECDHE+CHACHA20") != 1) return configurationError("cannot configure TLS ciphers");
    SSL_CTX_set_alpn_select_cb(context.get(), negotiateProtocol, const_cast<unsigned char*>(protocols.data() + (options->enableHttp2 ? 0 : 3)));
    if (!loadChain(context.get(), {lyraStringBytes(chain), chainSize})) return configurationError("invalid PEM certificate chain");
    if (!loadKey(context.get(), {lyraStringBytes(key), keySize})) return configurationError("invalid or mismatched PEM private key");
    return lyraHttpListenWithTransport(host, port, options, &transport, context.release());
}
