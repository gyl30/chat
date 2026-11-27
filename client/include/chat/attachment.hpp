#ifndef CHAT_CLIENT_INCLUDE_CHAT_ATTACHMENT_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_ATTACHMENT_HPP

#include <atomic>
#include <cstddef>
#include <stop_token>
#include <functional>
#include <memory>
#include <cstdint>
#include <string>

namespace chat
{

inline constexpr std::int64_t max_attachment_size = INT64_C(4) * 1024 * 1024 * 1024;
inline constexpr std::int64_t attachment_part_size = INT64_C(16) * 1024 * 1024;
inline constexpr std::int64_t upload_retention_seconds = INT64_C(15) * 24 * 3600;
inline constexpr std::int64_t max_image_preview_size = INT64_C(10) * 1024 * 1024;

class download_control
{
   public:
    bool cancel() noexcept
    {
        auto expected = phase::running;
        if (!phase_.compare_exchange_strong(expected, phase::cancelled))
        {
            return false;
        }
        stop_.request_stop();
        return true;
    }
    bool cancelled() const noexcept { return phase_.load() == phase::cancelled; }
    std::stop_token token() const noexcept { return stop_.get_token(); }
    bool begin_finalize() noexcept
    {
        auto expected = phase::running;
        return phase_.compare_exchange_strong(expected, phase::finalizing);
    }

   private:
    enum class phase
    {
        running,
        cancelled,
        finalizing
    };
    std::atomic<phase> phase_{phase::running};
    std::stop_source stop_;
};

struct download_options
{
    std::shared_ptr<download_control> control;
    std::function<void(std::int64_t, std::int64_t)> progress;
    bool replace_existing = true;
};

struct attachment_info
{
    std::string filename;
    std::string media_type;
    std::int64_t size = 0;
    std::string object_key;
};

}    // namespace chat

#endif
