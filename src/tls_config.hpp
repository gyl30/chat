#ifndef CHAT_SRC_TLS_CONFIG_HPP
#define CHAT_SRC_TLS_CONFIG_HPP

#include <expected>
#include <string>
#include <string_view>

#include <boost/corosio/tls_context.hpp>

// Validate and preload the same PEM bytes before the listener starts. The context is immutable
// after it has been passed to a server and its connection streams.
std::expected<boost::corosio::tls_context, std::string>
make_server_tls_context(std::string_view certificate_chain_file, std::string_view private_key_file);

#endif
