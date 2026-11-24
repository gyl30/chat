#ifndef CHAT_SRC_CURL_HANDLES_HPP
#define CHAT_SRC_CURL_HANDLES_HPP

#include <curl/curl.h>
#include <cstdlib>
#include <memory>
#include <string>

namespace chat::detail
{
inline auto make_curl()
{
    // All transfer workers are owned by service/client instances and join before process exit.
    struct runtime
    {
        CURLcode status = curl_global_init(CURL_GLOBAL_DEFAULT);
        ~runtime()
        {
            if (status == CURLE_OK)
            {
                curl_global_cleanup();
            }
        }
    };
    static runtime const global;
    return std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>{global.status == CURLE_OK ? curl_easy_init() : nullptr, curl_easy_cleanup};
}

// Match OpenSSL's process-scoped trust override without changing system CAs.
inline void use_openssl_ca_file(CURL* curl)
{
    if (auto const* path = std::getenv("SSL_CERT_FILE"); path && *path)
    {
        curl_easy_setopt(curl, CURLOPT_CAINFO, path);
    }
}

using curl_headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

inline bool add_header(curl_headers& headers, std::string const& header)
{
    auto* next = curl_slist_append(headers.get(), header.c_str());
    if (!next)
    {
        return false;
    }
    headers.release();
    headers.reset(next);
    return true;
}
}    // namespace chat::detail
#endif
