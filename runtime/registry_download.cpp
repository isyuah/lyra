#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <curl/curl.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace {

constexpr std::size_t maximum = 64 * 1024 * 1024;

std::size_t writeBody(char* bytes, std::size_t width, std::size_t count, void* opaque) {
    auto& remaining = *static_cast<std::size_t*>(opaque);
    if (!width || count > remaining / width) return 0;
    auto size = width * count;
    auto written = std::fwrite(bytes, 1, size, stdout);
    remaining -= written;
    return written;
}

int download(const char* url, const std::filesystem::path& caPath) {
    if (curl_global_sslset(CURLSSLBACKEND_OPENSSL, nullptr, nullptr) != CURLSSLSET_OK) return 1;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
    struct Global { ~Global() { curl_global_cleanup(); } } initialized;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(), curl_easy_cleanup);
    if (!easy) return 1;
    std::size_t remaining = maximum;
    auto status = CURLE_OK;
    auto set = [&](CURLoption option, auto value) {
        if (status == CURLE_OK) status = curl_easy_setopt(easy.get(), option, value);
    };
    set(CURLOPT_URL, url);
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_FAILONERROR, 1L);
    set(CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    set(CURLOPT_TIMEOUT_MS, 30000L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(maximum));
    set(CURLOPT_WRITEFUNCTION, writeBody);
    set(CURLOPT_WRITEDATA, &remaining);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    if (!caPath.empty()) {
        constexpr std::size_t caLimit = 4 * 1024 * 1024;
        std::string pem(caLimit + 1, '\0');
        std::ifstream input(caPath, std::ios::binary);
        if (!input) return 1;
        input.read(pem.data(), pem.size());
        auto size = static_cast<std::size_t>(input.gcount());
        if (input.bad() || !size || size > caLimit) return 1;
        curl_blob bundle{pem.data(), size, CURL_BLOB_COPY};
        set(CURLOPT_CAINFO, static_cast<const char*>(nullptr));
        set(CURLOPT_CAPATH, static_cast<const char*>(nullptr));
        set(CURLOPT_CAINFO_BLOB, &bundle);
    }
#ifdef _WIN32
    else {
        set(CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
        set(CURLOPT_PROXY_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
    }
#endif
    if (status == CURLE_OK) status = curl_easy_perform(easy.get());
    long response = 0;
    if (status != CURLE_OK || curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &response) != CURLE_OK || response < 200 || response >= 300)
        return 1;
    return std::fflush(stdout) == 0 ? 0 : 1;
}

}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || _setmode(_fileno(stdout), _O_BINARY) == -1) return 1;
    auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[1], -1, nullptr, 0, nullptr, nullptr);
    if (!size) return 1;
    std::string url(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[1], -1, url.data(), size, nullptr, nullptr)) return 1;
    auto ca = _wgetenv(L"LYRA_REGISTRY_CA_FILE");
    return download(url.c_str(), ca ? ca : L"");
}
#else
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    auto ca = std::getenv("LYRA_REGISTRY_CA_FILE");
    return download(argv[1], ca ? ca : "");
}
#endif
