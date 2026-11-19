#ifndef CHAT_SRC_TIMESTAMP_HPP
#define CHAT_SRC_TIMESTAMP_HPP

#include <chrono>
#include <cstdint>

inline std::int64_t epoch_milliseconds(std::chrono::system_clock::time_point time = std::chrono::system_clock::now())
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

#endif
