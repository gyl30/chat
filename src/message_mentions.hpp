#ifndef CHAT_SRC_MESSAGE_MENTIONS_HPP
#define CHAT_SRC_MESSAGE_MENTIONS_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <boost/capy/io_task.hpp>
#include <chat/message.hpp>

class pg_connection;

// 调用方持有 conversation 锁及事务；修改正文和目标必须一起提交。
boost::capy::io_task<std::vector<chat::mention>> refresh_message_mentions(
    pg_connection& connection, std::int64_t conversation, std::int64_t message, std::string text);

#endif
