#include "attachment_files.hpp"
#include <chat/attachment.hpp>
#include <miniocpp/http.h>
#include <miniocpp/utils.h>
#include <httplib.h>
#include <openssl/evp.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <array>
#include <chrono>
#include <memory>
#include <openssl/rand.h>

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
    auto db = sqlite_database::open((base / "tasks.sqlite3").string());
    if (!db)
    {
        return failed("无法打开上传记录");
    }
    if (db->exec(R"SQL(CREATE TABLE IF NOT EXISTS installation(id INTEGER PRIMARY KEY CHECK(id=1),secret TEXT NOT NULL);
    CREATE TABLE IF NOT EXISTS tasks(user INTEGER NOT NULL,client INTEGER NOT NULL,conversation INTEGER NOT NULL,path TEXT NOT NULL,
        size INTEGER NOT NULL,stamp INTEGER NOT NULL,reply INTEGER NOT NULL,checksum TEXT NOT NULL,part_size INTEGER NOT NULL DEFAULT 0,
        count INTEGER NOT NULL DEFAULT 0,number INTEGER NOT NULL DEFAULT 0,confirmed INTEGER NOT NULL DEFAULT 0,length INTEGER NOT NULL DEFAULT 0,
        touched INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER)), PRIMARY KEY(user,client));
    CREATE TABLE IF NOT EXISTS text_outbox(
        user INTEGER NOT NULL,client INTEGER NOT NULL,conversation INTEGER NOT NULL,body TEXT NOT NULL,
        reply INTEGER,created INTEGER NOT NULL,rejection INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(user,client));)SQL"))
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
    auto address = minio::http::Url::Parse(url);
    httplib::Client http((address.https ? "https://" : "http://") + address.HostHeaderValue());
    http.set_path_encode(false);
    http.set_connection_timeout(5, 0);
    http.set_write_timeout(60, 0);
    http.set_read_timeout(60, 0);
    httplib::Headers headers{{"Content-MD5", minio::utils::Md5sumHash(bytes)}};
    std::stop_callback interrupt(stop, [&] { http.stop(); });
    if (stop.stop_requested())
    {
        return failed("上传已暂停");
    }
    auto last = std::chrono::steady_clock::time_point{};
    auto result = http.Put("/" + address.path + "?" + address.query_string,
                           headers,
                           bytes,
                           "application/octet-stream",
                           [&](std::size_t bytes_sent, std::size_t part_bytes)
                           {
                               if (stop.stop_requested())
                               {
                                   return false;
                               }
                               auto now = std::chrono::steady_clock::now();
                               if (progress && (now - last >= std::chrono::milliseconds(120) || bytes_sent == part_bytes))
                               {
                                   progress(offset + static_cast<std::int64_t>(bytes_sent), snapshot.size);
                                   last = now;
                               }
                               return true;
                           });
    if (stop.stop_requested())
    {
        return failed("上传已暂停");
    }
    if (!result || result->status != 200)
    {
        return failed("分片上传失败，任务已暂停");
    }
    auto etag = result->get_header_value("ETag");
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
file_result<std::string> download_file(
    std::string const& url, std::int64_t size, std::string const& checksum, std::string const& target, std::atomic_bool const& stop)
{
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
    minio::http::Request req(minio::http::Method::kGet, minio::http::Url::Parse(url));
    req.connect_timeout_secs = 5;
    req.datafunc = [&](minio::http::DataFunctionArgs const& chunk)
    {
        auto const& bytes = chunk.datachunk;
        valid = valid && !stop && bytes.size() <= static_cast<std::uint64_t>(size - count) && d.add(bytes) &&
                static_cast<bool>(out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())));
        if (valid)
        {
            count += static_cast<std::int64_t>(bytes.size());
        }
        return valid;
    };
    auto res = req.Execute();
    out.close();
    std::error_code ec;
    if (res && !stop && valid && out && count == size && d.finish() == checksum)
    {
        fs::rename(tmp, fs::path(target), ec);
        if (!ec)
        {
            return target;
        }
    }
    fs::remove(tmp, ec);
    return failed("下载未完成或 SHA-256 校验失败");
}
}    // namespace chat::detail
