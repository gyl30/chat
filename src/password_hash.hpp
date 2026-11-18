#ifndef CHAT_SRC_PASSWORD_HASH_HPP
#define CHAT_SRC_PASSWORD_HASH_HPP

#include <expected>
#include <string>
#include <string_view>
#include <system_error>

// These synchronous operations belong on a worker, outside database transactions.
std::expected<std::string, std::error_code> hash_password(std::string_view password);
std::expected<bool, std::error_code> check_password(std::string_view password, std::string_view hash);

#endif
