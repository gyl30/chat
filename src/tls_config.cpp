#include "tls_config.hpp"

#include <fstream>
#include <memory>
#include <utility>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace
{

constexpr std::size_t kMaxPemFileSize = 4 * 1024 * 1024;

std::expected<std::string, std::string> read_pem_file(std::string_view path, std::string_view description)
{
    std::ifstream input(std::string(path), std::ios::binary);
    if (!input) { return std::unexpected("Cannot read TLS " + std::string(description) + " file"); }
    std::string contents(kMaxPemFileSize + 1, '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    contents.resize(static_cast<std::size_t>(input.gcount()));
    if (input.bad() || contents.size() > kMaxPemFileSize)
    { return std::unexpected("TLS " + std::string(description) + " file is unreadable or exceeds 4 MiB"); }
    return contents;
}

std::string_view trim_pem(std::string_view text)
{
    auto const first = text.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string_view::npos) { return {}; }
    text.remove_prefix(first);
    text.remove_suffix(text.size() - text.find_last_not_of(" \t\r\n\f\v") - 1);
    return text;
}

int no_password(char*, int, int, void*) { return 0; }

using bio_ptr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using certificate_ptr = std::unique_ptr<X509, decltype(&X509_free)>;
using key_ptr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

} // namespace

std::expected<boost::corosio::tls_context, std::string>
make_server_tls_context(std::string_view certificate_chain_file, std::string_view private_key_file)
{
    auto certificate_bytes = read_pem_file(certificate_chain_file, "certificate chain");
    if (!certificate_bytes) { return std::unexpected(certificate_bytes.error()); }
    auto private_key_bytes = read_pem_file(private_key_file, "private key");
    if (!private_key_bytes) { return std::unexpected(private_key_bytes.error()); }

    // Parse every certificate and reject non-whitespace after or between the PEM blocks. Merely
    // loading a corosio context defers parsing until a handshake and would accept a broken setup.
    ERR_clear_error();
    constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----";
    constexpr std::string_view end = "-----END CERTIFICATE-----";
    auto remaining = trim_pem(*certificate_bytes);
    certificate_ptr leaf(nullptr, X509_free);
    while (!remaining.empty())
    {
        if (!remaining.starts_with(begin)) { return std::unexpected("TLS certificate chain is invalid PEM"); }
        auto const closing = remaining.find(end, begin.size());
        if (closing == std::string_view::npos) { return std::unexpected("TLS certificate chain is invalid PEM"); }
        auto const length = closing + end.size();
        auto block = std::string(remaining.substr(0, length)) + '\n';
        bio_ptr input(BIO_new_mem_buf(block.data(), static_cast<int>(block.size())), BIO_free);
        certificate_ptr certificate(input ? PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr) : nullptr, X509_free);
        if (!certificate) { return std::unexpected("TLS certificate chain is invalid PEM"); }
        if (!leaf) { leaf = std::move(certificate); }
        remaining = trim_pem(remaining.substr(length));
    }
    if (!leaf) { return std::unexpected("TLS certificate chain is empty"); }

    auto const key_text = trim_pem(*private_key_bytes);
    bool complete_key = false;
    for (std::string_view label : {"PRIVATE KEY", "RSA PRIVATE KEY", "EC PRIVATE KEY", "DSA PRIVATE KEY"})
    {
        auto const opening = "-----BEGIN " + std::string(label) + "-----";
        auto const closing = "-----END " + std::string(label) + "-----";
        if (key_text.starts_with(opening))
        {
            auto const at = key_text.find(closing, opening.size());
            complete_key = at != std::string_view::npos && at + closing.size() == key_text.size();
            break;
        }
    }
    if (!complete_key) { return std::unexpected("TLS private key is invalid PEM or encrypted"); }
    bio_ptr key_input(BIO_new_mem_buf(private_key_bytes->data(), static_cast<int>(private_key_bytes->size())), BIO_free);
    key_ptr key(key_input ? PEM_read_bio_PrivateKey(key_input.get(), nullptr, no_password, nullptr) : nullptr, EVP_PKEY_free);
    if (!key) { return std::unexpected("TLS private key is invalid PEM or encrypted"); }
    if (X509_check_private_key(leaf.get(), key.get()) != 1)
    { return std::unexpected("TLS private key does not match the first certificate"); }

    boost::corosio::tls_context context;
    if (auto ec = context.use_certificate_chain(*certificate_bytes))
    { return std::unexpected("Cannot load TLS certificate chain: " + ec.message()); }
    if (auto ec = context.use_private_key(*private_key_bytes, boost::corosio::tls_file_format::pem))
    { return std::unexpected("Cannot load TLS private key: " + ec.message()); }
    if (auto ec = context.set_min_protocol_version(boost::corosio::tls_version::tls_1_2))
    { return std::unexpected("Cannot configure TLS protocol: " + ec.message()); }
    ERR_clear_error();
    return context;
}
