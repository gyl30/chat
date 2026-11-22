#ifndef CHAT_CLIENT_ATTACHMENT_FILES_HPP
#define CHAT_CLIENT_ATTACHMENT_FILES_HPP
#include <chat/client.hpp>
#include "sqlite_database.hpp"
#include <atomic>
#include <stop_token>

namespace chat::detail
{
struct inspected_file
{
    std::int64_t size, stamp;
    std::string checksum, media;
};
template <class T>
using file_result = std::expected<T, error>;
file_result<sqlite_database> open_transfer_journal(std::string const& directory, std::string const& endpoint);
file_result<std::string> installation_secret(sqlite_database& db);
file_result<std::pair<std::int64_t, std::int64_t>> file_snapshot(std::string const& path);
file_result<inspected_file> inspect_file(std::string const& path, std::stop_token stop);
file_result<std::string> put_part(std::string const& url,
                                  std::string const& path,
                                  inspected_file const& snapshot,
                                  std::int64_t offset,
                                  std::int64_t length,
                                  std::stop_token stop,
                                  client::progress_handler const& progress);
file_result<std::string> download_file(std::string const& url,
                                       std::int64_t size,
                                       std::string const& checksum,
                                       std::string const& target,
                                       std::atomic_bool const& stop,
                                       download_options const& options);
}    // namespace chat::detail
#endif
