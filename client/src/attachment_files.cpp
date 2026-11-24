#include "attachment_files.hpp"
#include <chat/attachment.hpp>
#include "curl_handles.hpp"
#include <openssl/evp.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <openssl/rand.h>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace chat::detail
{
namespace fs = std::filesystem;
namespace
{
auto failed(std::string text) { return std::unexpected(error{error_kind::file_transfer, 0, std::move(text)}); }
std::string hex(unsigned char const* bytes, std::size_t n)
{
    std::string out;
    constexpr std::string_view digits = "0123456789abcdef";
    for (std::size_t i = 0; i < n; i++)
    {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 15]);
    }
    return out;
}
struct digest
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    bool init() { return ctx && EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) == 1; }
    bool add(std::string_view s) { return EVP_DigestUpdate(ctx.get(), s.data(), s.size()) == 1; }
    std::string finish()
    {
        std::array<unsigned char, 32> hash;
        return EVP_DigestFinal_ex(ctx.get(), hash.data(), nullptr) == 1 ? hex(hash.data(), hash.size()) : std::string{};
    }
};

// Presigned grants are opaque URLs. Keep the path and query unchanged.
void configure_transfer(CURL* curl, std::string const& url)
{
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PATH_AS_IS, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    use_openssl_ca_file(curl);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
}
std::size_t discard_response(char*, std::size_t size, std::size_t count, void*) { return size * count; }
}    // namespace

file_result<std::pair<std::int64_t, std::int64_t>> file_snapshot(std::string const& path)
{
    std::error_code ec;
    auto f = fs::path(path);
    auto sz = fs::file_size(f, ec);
    if (ec)
    {
        return failed("文件不存在或无法读取");
    }
    if (sz > static_cast<std::uintmax_t>(max_attachment_size))
    {
        return failed("文件超过附件大小上限");
    }
    auto t = fs::last_write_time(f, ec);
    if (ec)
    {
        return failed("无法读取文件修改时间");
    }
    return std::pair{static_cast<std::int64_t>(sz), static_cast<std::int64_t>(t.time_since_epoch().count())};
}

file_result<sqlite_database> open_transfer_journal(std::string const& directory, std::string const& endpoint)
{
    fs::path base(directory);
    if (base.empty())
    {
        if (auto* x = std::getenv("XDG_DATA_HOME"); x && *x)
        {
            base = fs::path(x) / "chat";
        }
        else if (auto* x = std::getenv("HOME"); x && *x)
        {
            base = fs::path(x) / ".local/share/chat";
        }
        else
        {
            return failed("缺少应用数据目录");
        }
    }
    digest d;
    if (!d.init() || !d.add(endpoint))
    {
        return failed("无法生成服务标识");
    }
    auto key = d.finish();
    if (key.empty())
    {
        return failed("无法生成服务标识");
    }
    base = base / "transfers" / key;
    std::error_code ec;
    fs::create_directories(base, ec);
    if (!ec)
    {
        fs::permissions(base, fs::perms::owner_all, fs::perm_options::replace, ec);
    }
    if (ec)
    {
        return failed("无法创建私有上传目录");
    }
    auto const journal = base / "tasks.sqlite3";
    auto db = sqlite_database::open(journal.string());
    if (!db)
    {
        return failed("无法打开上传记录");
    }
#ifndef _WIN32
    fs::permissions(journal, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    if (ec)
    {
        return failed("无法保护本地任务凭证");
    }
#endif
    if (db->exec(R"SQL(CREATE TABLE IF NOT EXISTS login_credentials(username TEXT PRIMARY KEY,token TEXT NOT NULL);
    CREATE TABLE IF NOT EXISTS installation(id INTEGER PRIMARY KEY CHECK(id=1),secret TEXT NOT NULL);
    CREATE TABLE IF NOT EXISTS tasks(user INTEGER NOT NULL,client INTEGER NOT NULL,conversation INTEGER NOT NULL,path TEXT NOT NULL,
        size INTEGER NOT NULL,stamp INTEGER NOT NULL,reply INTEGER NOT NULL,checksum TEXT NOT NULL,part_size INTEGER NOT NULL DEFAULT 0,
        count INTEGER NOT NULL DEFAULT 0,number INTEGER NOT NULL DEFAULT 0,confirmed INTEGER NOT NULL DEFAULT 0,length INTEGER NOT NULL DEFAULT 0,
        touched INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER)), PRIMARY KEY(user,client));
    CREATE TABLE IF NOT EXISTS group_file_tasks(user INTEGER NOT NULL,client INTEGER NOT NULL,PRIMARY KEY(user,client),
        FOREIGN KEY(user,client) REFERENCES tasks(user,client) ON DELETE CASCADE);
    CREATE TABLE IF NOT EXISTS text_outbox(
        user INTEGER NOT NULL,client INTEGER NOT NULL,conversation INTEGER NOT NULL,body TEXT NOT NULL,
        reply INTEGER,created INTEGER NOT NULL,rejection INTEGER NOT NULL DEFAULT 0,markdown INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(user,client));
    CREATE TABLE IF NOT EXISTS compose_drafts(
        user INTEGER NOT NULL,conversation INTEGER NOT NULL,revision INTEGER NOT NULL,reply INTEGER NOT NULL,
        body TEXT NOT NULL,updated INTEGER NOT NULL,markdown INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(user,conversation));)SQL"))
    {
        return failed("无法初始化上传记录");
    }
    if (db->exec("DELETE FROM tasks WHERE touched<=CAST(strftime('%s','now') AS INTEGER)-?", upload_retention_seconds))
    {
        return failed("无法清理过期上传记录");
    }
    return std::move(*db);
}
file_result<std::string> installation_secret(sqlite_database& db)
{
    std::array<unsigned char, 32> secret;
    if (RAND_bytes(secret.data(), secret.size()) != 1)
    {
        return failed("无法生成安装凭证");
    }
    // A v4 installation UUID followed by 128 bits of private proof. The server stores only its digest.
    secret[6] = (secret[6] & 15) | 64;
    secret[8] = (secret[8] & 63) | 128;
    if (db.exec("INSERT OR IGNORE INTO installation VALUES(1,?)", hex(secret.data(), secret.size())))
    {
        return failed("无法保存设备标识");
    }
    auto q = db.prepare("SELECT secret FROM installation WHERE id=1");
    if (!q)
    {
        return failed("无法读取设备标识");
    }
    auto row = q->step();
    if (!row || !*row)
    {
        return failed("无法读取设备标识");
    }
    return std::string(q->column_text(0));
}
file_result<inspected_file> inspect_file(std::string const& path, std::stop_token stop)
{
    auto fp = file_snapshot(path);
    if (!fp)
    {
        return std::unexpected(fp.error());
    }
    std::ifstream input(fs::path(path), std::ios::binary);
    digest d;
    if (!input || !d.init())
    {
        return failed("无法读取附件");
    }
    std::array<char, 65536> buf;
    std::int64_t total = 0;
    std::string media = "application/octet-stream";
    while (input)
    {
        if (stop.stop_requested())
        {
            return failed("上传已暂停");
        }
        input.read(buf.data(), buf.size());
        auto n = input.gcount();
        std::string_view bytes(buf.data(), n);
        if (total == 0 && bytes.starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8)))
        {
            media = "image/png";
        }
        else if (total == 0 && bytes.starts_with(std::string_view("\xff\xd8\xff", 3)))
        {
            media = "image/jpeg";
        }
        total += n;
        if (!d.add(bytes))
        {
            return failed("无法校验附件");
        }
    }
    auto after = file_snapshot(path);
    if (!input.eof() || total != fp->first || !after || *after != *fp)
    {
        return failed("原文件已变更");
    }
    auto hash = d.finish();
    if (hash.empty())
    {
        return failed("无法校验附件");
    }
    return inspected_file{total, fp->second, std::move(hash), std::move(media)};
}
file_result<std::string> put_part(std::string const& url,
                                  std::string const& path,
                                  inspected_file const& snapshot,
                                  std::int64_t offset,
                                  std::int64_t length,
                                  std::stop_token stop,
                                  client::progress_handler const& progress)
{
    auto same = [&]
    {
        auto f = file_snapshot(path);
        return f && f->first == snapshot.size && f->second == snapshot.stamp;
    };
    if (!same())
    {
        return failed("原文件已变更或不存在");
    }
    if (stop.stop_requested())
    {
        return failed("上传已暂停");
    }
    std::ifstream input(fs::path(path), std::ios::binary);
    std::string bytes(length, '\0');
    if (!input.seekg(offset) || !input.read(bytes.data(), length) || !same())
    {
        return failed("读取附件失败或原文件已变更");
    }
    std::array<unsigned char, 16> hash;
    std::array<unsigned char, 25> base64;
    if (EVP_Digest(bytes.data(), bytes.size(), hash.data(), nullptr, EVP_md5(), nullptr) != 1)
    {
        return failed("无法校验附件分片");
    }
    EVP_EncodeBlock(base64.data(), hash.data(), hash.size());
    curl_headers headers{nullptr, curl_slist_free_all};
    auto curl = make_curl();
    if (!curl || !add_header(headers, "Content-MD5: " + std::string(reinterpret_cast<char const*>(base64.data()))) ||
        !add_header(headers, "Content-Type: application/octet-stream") || !add_header(headers, "Expect:"))
    {
        return failed("无法开始分片上传");
    }
    struct upload_transfer
    {
        std::string_view bytes;
        std::stop_token stop;
        client::progress_handler const& progress;
        std::int64_t offset, total;
        std::chrono::steady_clock::time_point last{};
    } transfer{bytes, stop, progress, offset, snapshot.size};
    configure_transfer(curl.get(), url);
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(length));
    curl_easy_setopt(curl.get(), CURLOPT_READDATA, &transfer);
    curl_easy_setopt(
        curl.get(),
        CURLOPT_READFUNCTION,
        +[](char* out, std::size_t size, std::size_t count, void* data) -> std::size_t
        {
            auto& t = *static_cast<upload_transfer*>(data);
            if (t.stop.stop_requested())
            {
                return CURL_READFUNC_ABORT;
            }
            auto const n = std::min(size * count, t.bytes.size());
            if (n)
            {
                t.bytes.copy(out, n);
                t.bytes.remove_prefix(n);
            }
            return n;
        });
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &discard_response);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(
        curl.get(),
        CURLOPT_XFERINFOFUNCTION,
        +[](void* data, curl_off_t, curl_off_t, curl_off_t total, curl_off_t sent)
        {
            auto& t = *static_cast<upload_transfer*>(data);
            if (t.stop.stop_requested())
            {
                return 1;
            }
            auto const now = std::chrono::steady_clock::now();
            if (t.progress && total && (now - t.last >= std::chrono::milliseconds(120) || sent == total))
            {
                t.progress(t.offset + sent, t.total);
                t.last = now;
            }
            return 0;
        });
    auto const rc = curl_easy_perform(curl.get());
    if (stop.stop_requested())
    {
        return failed("上传已暂停");
    }
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (rc != CURLE_OK || status != 200)
    {
        return failed("分片上传失败，任务已暂停");
    }
    curl_header* header = nullptr;
    if (curl_easy_header(curl.get(), "ETag", 0, CURLH_HEADER, -1, &header) != CURLHE_OK)
    {
        return failed("S3 未返回 ETag");
    }
    std::string etag = header->value;
    if (etag.size() > 1 && etag.front() == '"' && etag.back() == '"')
    {
        etag = etag.substr(1, etag.size() - 2);
    }
    if (etag.empty())
    {
        return failed("S3 未返回 ETag");
    }
    return etag;
}
namespace
{
void publish_new_file(fs::path const& temporary, fs::path const& target, std::error_code& error)
{
#if defined(__linux__) && defined(SYS_renameat2)
    if (syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE) == 0)
    {
        return;
    }
    auto const code = errno;
    if (code != ENOSYS && code != EINVAL && code != EOPNOTSUPP)
    {
        error = {code, std::generic_category()};
        return;
    }
#endif
    fs::create_hard_link(temporary, target, error);
    if (!error)
    {
        std::error_code cleanup;
        fs::remove(temporary, cleanup);
    }
}
}    // namespace

file_result<std::string> download_file(std::string const& url,
                                       std::int64_t size,
                                       std::string const& checksum,
                                       std::string const& target,
                                       std::stop_token stop,
                                       download_options const& options)
{
    auto stopped = [&] { return stop.stop_requested() || (options.control && options.control->cancelled()); };
    if (stopped())
    {
        return failed("下载已取消或连接已断开");
    }
    auto curl = make_curl();
    if (!curl)
    {
        return failed("无法开始下载");
    }
    // Create exclusively in the destination directory so the final rename is atomic.
    std::array<unsigned char, 16> random;
    if (RAND_bytes(random.data(), random.size()) != 1)
    {
        return failed("无法创建下载临时文件");
    }
    auto tmp = fs::path(target);
    tmp += (".part-" + hex(random.data(), random.size()));
    std::ofstream out(tmp, std::ios::binary | std::ios::noreplace);
    if (!out)
    {
        return failed("无法打开下载目标");
    }
    digest d;
    std::int64_t count = 0;
    bool valid = d.init();
    auto last_progress = std::chrono::steady_clock::now();
    if (options.progress)
    {
        options.progress(0, size);
    }
    struct download_transfer
    {
        decltype(stopped)& cancelled;
        decltype(options.progress) const& progress;
        digest& hash;
        std::ofstream& output;
        std::int64_t size, &count;
        std::chrono::steady_clock::time_point last;
        bool& valid;
    } transfer{stopped, options.progress, d, out, size, count, last_progress, valid};
    configure_transfer(curl.get(), url);
    curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(
        curl.get(),
        CURLOPT_XFERINFOFUNCTION,
        +[](void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return static_cast<download_transfer*>(data)->cancelled() ? 1 : 0; });
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(
        curl.get(),
        CURLOPT_WRITEFUNCTION,
        +[](char* data, std::size_t size, std::size_t count, void* state) -> std::size_t
        {
            auto& t = *static_cast<download_transfer*>(state);
            auto const length = size * count;
            t.valid = t.valid && !t.cancelled() && length <= static_cast<std::uint64_t>(t.size - t.count) && t.hash.add({data, length}) &&
                      static_cast<bool>(t.output.write(data, static_cast<std::streamsize>(length)));
            if (!t.valid)
            {
                return CURL_WRITEFUNC_ERROR;
            }
            t.count += static_cast<std::int64_t>(length);
            auto const now = std::chrono::steady_clock::now();
            if (t.progress && now - t.last >= std::chrono::milliseconds(100))
            {
                t.progress(t.count, t.size);
                t.last = now;
            }
            return length;
        });
    auto const rc = curl_easy_perform(curl.get());
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    out.close();
    std::error_code ec;
    if (rc == CURLE_OK && status >= 200 && status < 300 && !stopped() && valid && out && count == size && d.finish() == checksum &&
        (!options.control || options.control->begin_finalize()))
    {
        if (options.replace_existing)
        {
            fs::rename(tmp, fs::path(target), ec);
        }
        else
        {
            // Publish without overwriting a file created after the save dialog.
            publish_new_file(tmp, fs::path(target), ec);
        }
        if (!ec)
        {
            if (options.progress)
            {
                options.progress(size, size);
            }
            return target;
        }
        fs::remove(tmp, ec);
        return failed("无法写入下载目标（可能已存在文件）");
    }
    fs::remove(tmp, ec);
    return failed(stopped() ? "下载已取消或连接已断开" : "下载未完成或 SHA-256 校验失败");
}
}    // namespace chat::detail
