#ifndef CHAT_SRC_S3_SIGNER_HPP
#define CHAT_SRC_S3_SIGNER_HPP

#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct s3_settings;

namespace s3
{
using parameters = std::vector<std::pair<std::string, std::string>>;
std::string path(std::string_view bucket, std::string_view key);
std::string query(parameters const& params);
std::string authority(s3_settings const& settings);
std::string origin(s3_settings const& settings);
std::string presign(s3_settings const& settings, std::string const& uri, std::string_view method, parameters params, int seconds, std::time_t now);
}    // namespace s3
#endif
