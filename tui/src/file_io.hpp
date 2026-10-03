#pragma once

#include <cerrno>
#include <cstring>
#include <cstdint>
#include <expected>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace chat::tui
{
namespace file_detail
{
struct descriptor
{
    int value;
    ~descriptor() { if (value >= 0) { ::close(value); } }
};
inline std::string failure(std::string_view action) { return std::string(action) + ": " + std::strerror(errno); }
}

// Open first and inspect the descriptor: checking a path before open allows a
// replaced FIFO to block the UI. O_NONBLOCK also makes that case safe.
inline std::expected<std::string, std::string> read_local_file(std::string const& path, std::size_t limit)
{
    if (path.empty() || path.find('\0') != std::string::npos) { return std::unexpected("请输入有效文件路径"); }
    file_detail::descriptor file{::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)};
    if (file.value < 0) { return std::unexpected(file_detail::failure("无法打开文件")); }
    struct stat info{};
    if (::fstat(file.value, &info) != 0) { return std::unexpected(file_detail::failure("无法读取文件信息")); }
    if (!S_ISREG(info.st_mode)) { return std::unexpected("只能读取普通文件"); }
    if (info.st_size < 0 || static_cast<std::uintmax_t>(info.st_size) > limit)
    { return std::unexpected("文件超过允许大小"); }
    std::string data;
    data.reserve(static_cast<std::size_t>(info.st_size));
    char buffer[16384];
    for (;;)
    {
        auto count = ::read(file.value, buffer, sizeof(buffer));
        if (count < 0)
        {
            if (errno == EINTR) { continue; }
            return std::unexpected(file_detail::failure("读取文件失败"));
        }
        if (count == 0) { return data; }
        if (static_cast<std::size_t>(count) > limit - data.size())
        { return std::unexpected("文件超过允许大小"); }
        data.append(buffer, static_cast<std::size_t>(count));
    }
}

inline std::expected<void, std::string> save_local_file(std::string const& path, std::string_view data)
{
    if (path.empty() || path.find('\0') != std::string::npos) { return std::unexpected("请输入有效保存路径"); }
    file_detail::descriptor file{::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
    if (file.value < 0) { return std::unexpected(file_detail::failure("无法创建文件（已有文件不会覆盖）")); }
    struct stat created{};
    if (::fstat(file.value, &created) != 0)
    {
        auto message = file_detail::failure("无法读取新文件信息");
        return std::unexpected(std::move(message));
    }
    auto failed = [&](std::string message) -> std::expected<void, std::string> {
        struct stat current{};
        if (::lstat(path.c_str(), &current) == 0 && current.st_ino == created.st_ino && current.st_dev == created.st_dev)
        { ::unlink(path.c_str()); }
        return std::unexpected(std::move(message));
    };
    std::size_t offset = 0;
    while (offset < data.size())
    {
        auto count = ::write(file.value, data.data() + offset, data.size() - offset);
        if (count < 0)
        {
            if (errno == EINTR) { continue; }
            return failed(file_detail::failure("写入文件失败"));
        }
        if (count == 0) { return failed("写入文件未完成"); }
        offset += static_cast<std::size_t>(count);
    }
    if (::fsync(file.value) != 0) { return failed(file_detail::failure("保存文件失败")); }
    auto const descriptor = std::exchange(file.value, -1);
    if (::close(descriptor) != 0) { return failed(file_detail::failure("关闭文件失败")); }
    return {};
}
}
