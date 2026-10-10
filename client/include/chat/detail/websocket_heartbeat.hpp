#ifndef CHAT_DETAIL_WEBSOCKET_HEARTBEAT_HPP
#define CHAT_DETAIL_WEBSOCKET_HEARTBEAT_HPP

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <span>

namespace chat::detail
{

struct websocket_heartbeat_config
{
    std::chrono::steady_clock::duration interval = std::chrono::seconds(30);
    std::chrono::steady_clock::duration grace = std::chrono::seconds(10);
};

// One outstanding probe. Application traffic and unsolicited pongs do not extend its deadline.
class websocket_heartbeat
{
   public:
    using clock = std::chrono::steady_clock;
    using token = std::array<std::uint8_t, 8>;

    explicit websocket_heartbeat(websocket_heartbeat_config config = {}) : config_(config) {}

    void reset(clock::time_point now) noexcept
    {
        pending_ = false;
        next_deadline_ = now + config_.interval;
    }

    clock::time_point deadline() const noexcept { return next_deadline_; }
    clock::duration grace() const noexcept { return config_.grace; }
    bool pending() const noexcept { return pending_; }
    bool probe_due(clock::time_point now) const noexcept { return !pending_ && now >= next_deadline_; }
    bool expired(clock::time_point now) const noexcept { return pending_ && now >= next_deadline_; }

    token start_probe(clock::time_point now) noexcept
    {
        ++sequence_;
        for (std::size_t i = 0; i < token_.size(); ++i)
        {
            token_[i] = static_cast<std::uint8_t>(sequence_ >> ((token_.size() - 1 - i) * 8));
        }
        pending_ = true;
        next_deadline_ = now + config_.grace;
        return token_;
    }

    void pong(std::span<std::uint8_t const> payload, clock::time_point now) noexcept
    {
        if (pending_ && now < next_deadline_ && payload.size() == token_.size() && std::equal(payload.begin(), payload.end(), token_.begin()))
        {
            reset(now);
        }
    }

   private:
    websocket_heartbeat_config config_;
    clock::time_point next_deadline_{};
    std::uint64_t sequence_ = 0;
    token token_{};
    bool pending_ = false;
};

}

#endif
