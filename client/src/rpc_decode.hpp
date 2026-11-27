#ifndef CHAT_CLIENT_SRC_RPC_DECODE_HPP
#define CHAT_CLIENT_SRC_RPC_DECODE_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <simdjson.h>

#include <chat/conversation.hpp>
#include <chat/error.hpp>
#include <chat/friendship.hpp>
#include <chat/message.hpp>
#include <chat/presence.hpp>
#include <chat/reaction.hpp>
#include <chat/user.hpp>

namespace chat::detail
{

bool parse_avatar_state(simdjson::dom::object object, avatar_state& value);
std::string string_field(simdjson::dom::object object, std::string_view key);
bool parse_user(simdjson::dom::object object, user& value);
std::optional<friendship_state> parse_friendship_state(std::optional<simdjson::dom::element> value);
bool parse_reaction_update(simdjson::dom::object object, reaction_update& value);
bool parse_message(simdjson::dom::object object, message& value);
std::expected<messages_result, error> parse_message_page(simdjson::dom::element response, std::int64_t conversation);
bool parse_receipt(simdjson::dom::object fields, message& value);
bool parse_presence(simdjson::dom::object object, presence& value);
std::expected<conversation, error> parse_conversation(simdjson::dom::element const& value);

}    // namespace chat::detail
#endif
