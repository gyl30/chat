#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <iostream>
#include <future>
#include <chat/detail/base64.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>
#include <boost/http/config.hpp>
#include <boost/http/field.hpp>
#include <boost/http/request_parser.hpp>
#include <boost/http/response.hpp>
#include <boost/http/serializer.hpp>
#include <boost/http/status.hpp>
#include <boost/http/version.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/tcp_server.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/json.hpp>

#include <chat/client.hpp>

#include "websocket.hpp"

namespace
{

using namespace std::chrono_literals;

std::string_view as_string_view(boost::core::string_view value) { return {value.data(), value.size()}; }

class client_test_worker final : public boost::corosio::tcp_server::worker_base
{
   public:
    client_test_worker(boost::corosio::io_context& io_context,
                       boost::http::shared_parser_config parser_config,
                       boost::http::shared_serializer_config serializer_config)
        : io_context_(io_context), socket_(io_context), parser_(std::move(parser_config)), serializer_(std::move(serializer_config))
    {
        serializer_.set_message(response_);
    }

    boost::corosio::tcp_socket& socket() override { return socket_; }

    void run(boost::corosio::tcp_server::launcher launch) override { launch(io_context_.get_executor(), run_session()); }

   private:
    boost::capy::io_task<> send_upgrade_response(std::string_view accept)
    {
        response_.clear();
        response_.set_start_line(boost::http::status::switching_protocols, boost::http::version::http_1_1);
        response_.set(boost::http::field::upgrade, "websocket");
        response_.set(boost::http::field::connection, "Upgrade");
        response_.set(boost::http::field::sec_websocket_accept, accept);

        serializer_.reset();
        serializer_.start();
        while (!serializer_.is_done())
        {
            auto prepared = serializer_.prepare();
            if (prepared.has_error())
            {
                co_return std::error_code(prepared.error());
            }
            if (boost::capy::buffer_empty(*prepared))
            {
                serializer_.consume(0);
                continue;
            }

            auto [ec, written] = co_await boost::capy::write(socket_, *prepared);
            serializer_.consume(written);
            if (ec)
            {
                co_return ec;
            }
        }
        co_return {};
    }

    boost::capy::io_task<> send_text(websocket_connection& connection, boost::json::object object)
    {
        auto text = boost::json::serialize(object);
        co_return co_await connection.send_text(text);
    }

    boost::capy::io_task<bool> handle_request(websocket_connection& connection, std::string_view payload)
    {
        boost::system::error_code ec;
        auto value = boost::json::parse(payload, ec);
        if (ec || !value.is_object())
        {
            co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
        }

        auto const& request = value.as_object();
        auto const* id = request.if_contains("id");
        auto const* method = request.if_contains("method");
        auto const* params = request.if_contains("params");
        if (!id || !method || !method->is_string() || !params || !params->is_object())
        {
            co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
        }

        boost::json::object response;
        response.emplace("jsonrpc", "2.0");
        response.emplace("id", *id);

        auto const operation = std::string_view(method->as_string());
        if (operation == "begin_avatar_upload" || operation == "upload_avatar_chunk" ||
            operation == "finish_avatar_upload" || operation == "cancel_avatar_upload" ||
            operation == "get_avatar" || operation == "clear_avatar")
        {
            if (operation == "get_avatar" && params->at("user").as_int64() == 92)
            { co_return boost::capy::io_result<bool>{std::error_code{}, true}; }
            boost::json::object result;
            if (operation == "begin_avatar_upload")
            {
                avatar_data_.clear();
                result = {{"upload", 7}};
            }
            else if (operation == "upload_avatar_chunk")
            {
                auto bytes = chat::detail::decode_base64(std::string_view(params->at("data").as_string()));
                if (!bytes || params->at("offset").as_int64() != static_cast<std::int64_t>(avatar_data_.size()))
                { co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false}; }
                avatar_data_ += *bytes;
                result = {{"offset", static_cast<std::int64_t>(avatar_data_.size() + (avatar_data_ == "bad-offset" ? 1 : 0))}};
            }
            else if (operation == "finish_avatar_upload") { result = {{"avatar_revision", avatar_data_ == "bad-finish" ? 0 : 1}, {"has_avatar", true}}; }
            else if (operation == "cancel_avatar_upload") { result = {{"cancelled", true}}; }
            else if (operation == "clear_avatar") { result = {{"avatar_revision", 2}, {"has_avatar", false}}; }
            else
            {
                auto user = params->at("user").as_int64();
                auto offset = params->at("offset").as_int64();
                auto bytes = std::string_view(avatar_data_).substr(offset, chat::attachment_chunk_size);
                result = {{"user", user}, {"avatar_revision", 1}, {"has_avatar", true}, {"offset", offset},
                    {"size", static_cast<std::int64_t>(avatar_data_.size())}, {"media_type", "image/png"},
                    {"data", chat::detail::encode_base64(bytes)},
                    {"has_more", offset + static_cast<std::int64_t>(bytes.size()) < static_cast<std::int64_t>(avatar_data_.size())}};
                if (user == 99) { result["data"] = "!invalid!"; }
                if (user == 98) { result["offset"] = offset + 1; }
                if (user == 97) { result["avatar_revision"] = 2; }
                if (user == 96) { result["avatar_revision"] = 0; }
                if (user == 95) { result["size"] = 1048577; }
                if (user == 94) { result["media_type"] = "application/octet-stream"; }
                if (user == 93) { result["has_more"] = !result.at("has_more").as_bool(); }
            }
            response.emplace("result", std::move(result));
            auto [sent] = co_await send_text(connection, std::move(response));
            if (sent) { co_return boost::capy::io_result<bool>{sent, false}; }
            if (operation == "finish_avatar_upload")
            {
                boost::json::object notice{{"jsonrpc", "2.0"}, {"method", "avatar"},
                    {"params", boost::json::object{{"user", 1}, {"avatar_revision", 1}, {"has_avatar", true}}}};
                auto [notified] = co_await send_text(connection, std::move(notice));
                if (notified) { co_return boost::capy::io_result<bool>{notified, false}; }
                boost::json::object invalid{{"jsonrpc", "2.0"}, {"method", "avatar"},
                    {"params", boost::json::object{{"user", 1}, {"avatar_revision", -1}, {"has_avatar", false}}}};
                auto [invalid_ec] = co_await send_text(connection, std::move(invalid));
                co_return boost::capy::io_result<bool>{invalid_ec, !invalid_ec};
            }
            co_return boost::capy::io_result<bool>{std::error_code{}, true};
        }

        if (operation == "open_direct_conversation")
        {
            auto const user = params->at("user").as_int64();
            boost::json::object result{{"conversation", 2}, {"can_send", true}};
            if (user == 98) { result.erase("can_send"); }
            if (user == 99) { result["can_send"] = "true"; }
            if (user == 97) { result["can_send"] = false; }
            if (user == 96) { result["conversation"] = 0; }
            response.emplace("result", std::move(result));
            auto [sent] = co_await send_text(connection, std::move(response));
            co_return boost::capy::io_result<bool>{sent, !sent};
        }
        if (operation == "set_conversation_muted" || operation == "set_conversation_pinned")
        {
            auto const conversation = params->at("conversation").as_int64();
            auto const key = operation == "set_conversation_pinned" ? "pinned" : "muted";
            auto const value = params->at(key).as_bool();
            boost::json::object result{{key, value}};
            if (conversation == 98) { result.erase(key); }
            if (conversation == 99) { result[key] = "true"; }
            if (conversation == 97) { result[key] = !value; }
            response.emplace("result", std::move(result));
            auto [sent] = co_await send_text(connection, std::move(response));
            co_return boost::capy::io_result<bool>{sent, !sent};
        }

        if (operation == "get_group_invite" || operation == "create_group_invite" || operation == "revoke_group_invite")
        {
            if (params->as_object().size() != 1 || !params->at("conversation").is_int64())
            { co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false}; }
            auto const conversation = params->at("conversation").as_int64();
            boost::json::object result{{"token", operation == "revoke_group_invite" || conversation == 1 ? boost::json::value(nullptr) : boost::json::value(std::string(64, 'a'))}};
            if (conversation == 98) { result.erase("token"); }
            if (conversation == 99) { result["token"] = true; }
            if (conversation == 97) { result["token"] = std::string(64, 'z'); }
            if (conversation == 96) { result["token"] = std::string(63, 'a'); }
            if (conversation == 95) { result["token"] = operation == "revoke_group_invite" ? boost::json::value(std::string(64, 'a')) : boost::json::value(nullptr); }
            response.emplace("result", std::move(result));
            auto [sent] = co_await send_text(connection, std::move(response));
            co_return boost::capy::io_result<bool>{sent, !sent};
        }
        if (operation == "join_group")
        {
            if (params->as_object().size() != 1 || !params->at("token").is_string())
            { co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false}; }
            auto const token = params->at("token").as_string();
            boost::json::object result{{"conversation", 2}, {"title", "linked group"}, {"member_count", 3}, {"state", token.front() == 'b' ? "member" : "joined"}};
            if (token.front() == '0') { result["state"] = "pending"; }
            if (token.front() == 'c') { result.erase("conversation"); }
            if (token.front() == 'd') { result["conversation"] = 0; }
            if (token.front() == 'e') { result["title"] = ""; }
            if (token.front() == 'f') { result["member_count"] = 0; }
            if (token.front() == 'g') { result["state"] = "unknown"; }
            if (token.front() == 'h') { result["member_count"] = "3"; }
            if (token.front() == 'i') { result["title"] = std::string("a\0b", 3); }
            if (token.front() == 'j') { result["title"] = std::string(257, 'a'); }
            response.emplace("result", std::move(result));
            auto [sent] = co_await send_text(connection, std::move(response));
            co_return boost::capy::io_result<bool>{sent, !sent};
        }

        if (operation == "get_group_join_requests")
        {
            auto const conversation = params->at("conversation").as_int64();
            boost::json::object applicant{{"id", 3}, {"username", "requester"}, {"avatar_revision", 0}, {"has_avatar", false}, {"created_at", 1000}};
            boost::json::object result{{"requests", boost::json::array{applicant}}, {"next", nullptr}};
            if (conversation == 99) { result.erase("requests"); }
            if (conversation == 98) { result["requests"] = true; }
            if (conversation == 97) { result.erase("next"); }
            if (conversation == 96) { result["next"] = 3; }
            if (conversation == 95) { result["requests"].as_array().push_back(applicant); }
            if (conversation == 94) { result["requests"].as_array()[0].as_object().erase("avatar_revision"); }
            if (conversation == 93) { result["requests"].as_array()[0].as_object()["created_at"] = 0; }
            if (conversation == 92) { result["requests"].as_array()[0].as_object()["id"] = 0; }
            response.emplace("result", std::move(result));
            auto [sent] = co_await send_text(connection, std::move(response));
            if (sent) { co_return boost::capy::io_result<bool>{sent, false}; }
            if (conversation == 2)
            {
                for (auto const* state : {"pending", "accepted", "rejected"})
                {
                    auto [notification_ec] = co_await send_text(connection, {{"jsonrpc", "2.0"}, {"method", "join_request"},
                        {"params", boost::json::object{{"conversation", 2}, {"user", 3}, {"state", state}}}});
                    if (notification_ec) { co_return boost::capy::io_result<bool>{notification_ec, false}; }
                }
            }
            else if (conversation >= 80 && conversation <= 84)
            {
                boost::json::object notification{{"conversation", 2}, {"user", 3}, {"state", "pending"}};
                if (conversation == 80) { notification["conversation"] = 0; }
                if (conversation == 81) { notification["user"] = 0; }
                if (conversation == 82) { notification["state"] = "unknown"; }
                if (conversation == 83) { notification.erase("state"); }
                if (conversation == 84) { notification["user"] = "3"; }
                auto [notification_ec] = co_await send_text(connection, {{"jsonrpc", "2.0"}, {"method", "join_request"}, {"params", std::move(notification)}});
                if (notification_ec) { co_return boost::capy::io_result<bool>{notification_ec, false}; }
            }
            co_return boost::capy::io_result<bool>{std::error_code{}, true};
        }
        if (operation == "pin_group_message" || operation == "unpin_group_message" || operation == "set_group_announcement" ||
            operation == "set_group_join_approval" || operation == "respond_group_join_request")
        {
            auto const conversation = params->at("conversation").as_int64();
            boost::json::object result{{"changed", true}};
            if (operation == "pin_group_message" && params->at("message").as_int64() != 10)
            { co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false}; }
            if (operation == "set_group_announcement")
            {
                if (params->as_object().size() != 2 || !params->at("text").is_string())
                { co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false}; }
                result["changed"] = !params->at("text").as_string().empty();
            }
            if (operation == "set_group_join_approval" && (params->as_object().size() != 2 || !params->at("required").is_bool()))
            { co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false}; }
            if (operation == "respond_group_join_request" && (params->as_object().size() != 3 || params->at("user").as_int64() != 3 || !params->at("accept").is_bool()))
            { co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false}; }
            if (conversation == 98) { result.erase("changed"); }
            if (conversation == 99) { result["changed"] = "true"; }
            response.emplace("result", std::move(result));
            auto [sent] = co_await send_text(connection, std::move(response));
            co_return boost::capy::io_result<bool>{sent, !sent};
        }

        if (method->as_string() == "get_conversations")
        {
            boost::json::array conversations;
            auto const* before = params->as_object().if_contains("before");
            if (!before || before->at("id").as_int64() >= 80)
            {
                boost::json::object last;
                last.emplace("conversation", 2);
                last.emplace("username", "bob");
                last.emplace("avatar_revision", 0);
                last.emplace("mentions", boost::json::array{});
                last.emplace("reaction_revision", 0);
                last.emplace("reactions", boost::json::array{});
                last.emplace("has_avatar", false);
                last.emplace("id", 12);
                last.emplace("from", 2);
                last.emplace("timestamp", 1700000000000LL);
                last.emplace("text", "hello");

                boost::json::object conversation;
                conversation.emplace("id", 2);
                conversation.emplace("kind", "direct");
                conversation.emplace("can_send", true);
                conversation.emplace("member_count", 2);
                conversation.emplace("user", 2);
                conversation.emplace("username", "bob");
                conversation.emplace("avatar_revision", 0);
                conversation.emplace("has_avatar", false);
                conversation.emplace("last", std::move(last));
                conversation.emplace("unread", 3);
                conversation.emplace("muted", true);
                conversation.emplace("pinned", true);
                conversation.emplace("pinned_message", nullptr);
                conversation.emplace("announcement", "");
                conversation.emplace("join_approval", false);
                if (before && before->at("id").as_int64() >= 80 && before->at("id").as_int64() <= 85)
                {
                    auto const probe = before->at("id").as_int64();
                    conversation["kind"] = "group";
                    conversation["user"] = nullptr;
                    conversation["pinned_message"] = boost::json::object{{"id", 10}, {"from", 2}, {"username", "bob"},
                        {"text", "pinned"}, {"deleted", false}, {"edited_at", nullptr}};
                    if (probe == 81) { conversation.erase("pinned_message"); }
                    if (probe == 82) { conversation["pinned_message"] = true; }
                    if (probe == 83) { conversation["pinned_message"].as_object()["deleted"] = true; }
                    if (probe == 84) { conversation["pinned_message"].as_object()["id"] = 0; }
                    if (probe == 85) { conversation["kind"] = "direct"; conversation["user"] = 2; }
                }
                if (before && before->at("id").as_int64() == 94) { conversation.erase("pinned"); }
                if (before && before->at("id").as_int64() == 104) { conversation.erase("can_send"); }
                if (before && before->at("id").as_int64() == 105) { conversation["can_send"] = "true"; }
                if (before && before->at("id").as_int64() == 106) { conversation["can_send"] = false; }
                if (before && before->at("id").as_int64() == 107)
                { conversation["kind"] = "group"; conversation["user"] = nullptr; conversation["can_send"] = false; }
                if (before && before->at("id").as_int64() >= 86 && before->at("id").as_int64() <= 92)
                {
                    auto const probe = before->at("id").as_int64();
                    conversation["kind"] = "group";
                    conversation["user"] = nullptr;
                    conversation["announcement"] = "公告\n<plain>";
                    if (probe == 87) { conversation.erase("announcement"); }
                    if (probe == 88) { conversation["announcement"] = nullptr; }
                    if (probe == 89) { conversation["announcement"] = true; }
                    if (probe == 90) { conversation["announcement"] = std::string(4097, 'x'); }
                    if (probe == 91) { conversation["kind"] = "direct"; conversation["user"] = 2; }
                    if (probe == 92) { conversation["announcement"] = std::string("a\0b", 3); }
                }
                if (before && before->at("id").as_int64() == 95) { conversation["pinned"] = "false"; }
                if (before && before->at("id").as_int64() == 98) { conversation.erase("muted"); }
                if (before && before->at("id").as_int64() == 99) { conversation["muted"] = "false"; }
                if (before && before->at("id").as_int64() >= 100 && before->at("id").as_int64() <= 103)
                {
                    auto const probe = before->at("id").as_int64();
                    conversation["kind"] = "group";
                    conversation["user"] = nullptr;
                    conversation["join_approval"] = true;
                    if (probe == 101) { conversation.erase("join_approval"); }
                    if (probe == 102) { conversation["join_approval"] = "true"; }
                    if (probe == 103) { conversation["kind"] = "direct"; conversation["user"] = 2; }
                }
                conversations.push_back(std::move(conversation));
            }
            else if (before->is_object() && before->as_object().at("id").as_int64() == 2)
            {
                if (!before->at("pinned").as_bool())
                {
                    co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
                }
                boost::json::object last;
                last.emplace("conversation", 3);
                last.emplace("username", "alice");
                last.emplace("avatar_revision", 0);
                last.emplace("mentions", boost::json::array{});
                last.emplace("reaction_revision", 0);
                last.emplace("reactions", boost::json::array{});
                last.emplace("has_avatar", false);
                last.emplace("id", 6);
                last.emplace("from", 1);
                last.emplace("timestamp", 1699990000000LL);
                last.emplace("text", "older");

                boost::json::object conversation;
                conversation.emplace("id", 3);
                conversation.emplace("kind", "direct");
                conversation.emplace("member_count", 2);
                conversation.emplace("user", 3);
                conversation.emplace("username", "carol");
                conversation.emplace("can_send", false);
                conversation.emplace("avatar_revision", 0);
                conversation.emplace("has_avatar", false);
                conversation.emplace("last", std::move(last));
                conversation.emplace("unread", 0);
                conversation.emplace("muted", false);
                conversation.emplace("pinned", false);
                conversation.emplace("pinned_message", nullptr);
                conversation.emplace("announcement", "");
                conversation.emplace("join_approval", false);
                conversations.push_back(std::move(conversation));
            }
            else
            {
                co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
            }

            boost::json::object result;
            result.emplace("next", nullptr);
            if (!before || before->at("id").as_int64() == 96 || before->at("id").as_int64() == 97)
            {
                boost::json::object next{{"activity", 1700000000000LL}, {"id", 2}, {"pinned", true}};
                if (before && before->at("id").as_int64() == 96) { next.erase("pinned"); }
                if (before && before->at("id").as_int64() == 97) { next["pinned"] = 1; }
                result["next"] = std::move(next);
            }
            result.emplace("conversations", std::move(conversations));
            response.emplace("result", std::move(result));

            auto [send_ec] = co_await send_text(connection, std::move(response));
            co_return boost::capy::io_result<bool>{send_ec, !send_ec};
        }

        if (method->as_string() == "get_presence")
        {
            boost::json::object user;
            user.emplace("user", 2);
            user.emplace("online", true);
            user.emplace("last_seen", 1699999999000LL);
            boost::json::array users;
            users.push_back(std::move(user));
            boost::json::object result;
            result.emplace("users", std::move(users));
            response.emplace("result", std::move(result));

            auto [response_ec] = co_await send_text(connection, std::move(response));
            if (response_ec)
            {
                co_return boost::capy::io_result<bool>{response_ec, false};
            }

            boost::json::object notification_params;
            notification_params.emplace("user", 3);
            notification_params.emplace("online", false);
            notification_params.emplace("last_seen", 1700000001000LL);
            boost::json::object notification;
            notification.emplace("jsonrpc", "2.0");
            notification.emplace("method", "presence");
            notification.emplace("params", std::move(notification_params));
            auto [notification_ec] = co_await send_text(connection, std::move(notification));
            co_return boost::capy::io_result<bool>{notification_ec, !notification_ec};
        }

        if (method->as_string() == "set_message_reaction")
        {
            auto const message = params->as_object().at("message").as_int64();
            auto const emoji = params->as_object().at("emoji").as_string();
            boost::json::array reactions;
            if (!emoji.empty())
            {
                reactions.push_back(boost::json::object{{"emoji", emoji}, {"users", boost::json::array{1}}});
            }
            if (message == 99 && !reactions.empty())
            {
                reactions[0].as_object().at("users").as_array().push_back(1);
            }
            boost::json::object result{{"conversation", 2}, {"message", message}, {"reaction_revision", 2},
                                       {"reactions", std::move(reactions)}};
            response.emplace("result", result);
            auto [send_ec] = co_await send_text(connection, std::move(response));
            if (send_ec) { co_return boost::capy::io_result<bool>{send_ec, false}; }
            auto [notice_ec] = co_await send_text(connection, boost::json::object{
                {"jsonrpc", "2.0"}, {"method", "reaction"}, {"params", std::move(result)}});
            co_return boost::capy::io_result<bool>{notice_ec, !notice_ec};
        }

        if (method->as_string() == "get_messages")
        {
            auto const* user = params->as_object().if_contains("conversation");
            auto const* before = params->as_object().if_contains("before");
            if (!user || !user->is_int64() || user->as_int64() != 2)
            {
                co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
            }

            boost::json::array messages;
            if (!before || before->as_int64() >= 90)
            {
                boost::json::object first;
                first.emplace("conversation", 2);
                first.emplace("username", "bob");
                first.emplace("avatar_revision", 0);
                first.emplace("mentions", boost::json::array{});
                first.emplace("reaction_revision", 1);
                first.emplace("reactions", boost::json::array{boost::json::object{{"emoji", "👍"}, {"users", boost::json::array{1, 2}}}});
                first.emplace("has_avatar", false);
                first.emplace("id", 10);
                first.emplace("from", 2);
                first.emplace("timestamp", 1700000000000);
                first.emplace("text", "first @alice");
                first["mentions"] = boost::json::array{boost::json::object{{"user", 1}, {"username", "alice"}}};
                if (before)
                {
                    auto const probe = before->as_int64();
                    if (probe == 90) { first.erase("mentions"); }
                    if (probe == 91) { first["mentions"] = false; }
                    if (probe == 92) { first["mentions"].as_array()[0].as_object()["user"] = 0; }
                    if (probe == 93) { first["mentions"].as_array().push_back(first["mentions"].as_array().front()); }
                    if (probe == 94) { first["mentions"].as_array()[0].as_object()["username"] = 1; }
                    if (probe == 95) { first["deleted"] = true; first["reactions"] = boost::json::array{}; }
                }
                messages.push_back(std::move(first));

                boost::json::object second;
                second.emplace("conversation", 2);
                second.emplace("username", "alice");
                second.emplace("avatar_revision", 0);
                second.emplace("mentions", boost::json::array{});
                second.emplace("reaction_revision", 0);
                second.emplace("reactions", boost::json::array{});
                second.emplace("has_avatar", false);
                second.emplace("id", 12);
                second.emplace("from", 1);
                second.emplace("timestamp", 1700000060000);
                second.emplace("text", "second");
                messages.push_back(std::move(second));
            }
            else if (before->is_int64() && before->as_int64() == 10)
            {
                boost::json::object older;
                older.emplace("conversation", 2);
                older.emplace("username", "bob");
                older.emplace("avatar_revision", 0);
                older.emplace("mentions", boost::json::array{});
                older.emplace("reaction_revision", 0);
                older.emplace("reactions", boost::json::array{});
                older.emplace("has_avatar", false);
                older.emplace("id", 4);
                older.emplace("from", 2);
                older.emplace("timestamp", 1699999940000);
                older.emplace("text", "older");
                messages.push_back(std::move(older));
            }
            else
            {
                co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
            }

            boost::json::object result;
            result.emplace("messages", std::move(messages));
            result.emplace("read_positions", boost::json::array{boost::json::object{{"user", 2}, {"message", 12}}});
            result.emplace("has_more", false);
            response.emplace("result", std::move(result));

            auto [send_ec] = co_await send_text(connection, std::move(response));
            co_return boost::capy::io_result<bool>{send_ec, !send_ec};
        }

        if (method->as_string() == "get_contacts")
        {
            boost::json::object user;
            user.emplace("id", 3);
            user.emplace("username", "carol");
            user.emplace("avatar_revision", 0);
            user.emplace("has_avatar", false);
            boost::json::array users;
            users.push_back(std::move(user));
            boost::json::object result;
            result.emplace("users", std::move(users));
            response.emplace("result", std::move(result));

            auto [send_ec] = co_await send_text(connection, std::move(response));
            co_return boost::capy::io_result<bool>{send_ec, !send_ec};
        }

        if (method->as_string() == "search_users")
        {
            auto const* query = params->as_object().if_contains("query");
            if (!query || !query->is_string() || query->as_string() != "bo")
            {
                co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
            }

            boost::json::object user;
            user.emplace("id", 2);
            user.emplace("username", "bob");
            user.emplace("avatar_revision", 0);
            user.emplace("has_avatar", false);
            boost::json::array users;
            users.push_back(std::move(user));
            boost::json::object result;
            result.emplace("users", std::move(users));
            response.emplace("result", std::move(result));

            auto [send_ec] = co_await send_text(connection, std::move(response));
            co_return boost::capy::io_result<bool>{send_ec, !send_ec};
        }

        if (method->as_string() == "add_contact")
        {
            auto const* user_id = params->as_object().if_contains("user");
            if (!user_id || !user_id->is_int64() || user_id->as_int64() != 2)
            {
                co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
            }

            boost::json::object user;
            user.emplace("id", 2);
            user.emplace("username", "bob");
            user.emplace("avatar_revision", 0);
            user.emplace("has_avatar", false);
            boost::json::object result;
            result.emplace("user", std::move(user));
            response.emplace("result", std::move(result));

            auto [send_ec] = co_await send_text(connection, std::move(response));
            co_return boost::capy::io_result<bool>{send_ec, !send_ec};
        }

        if (method->as_string() == "send_message")
        {
            auto const* user = params->as_object().if_contains("conversation");
            auto const* text = params->as_object().if_contains("text");
            if (!user || !user->is_int64() || user->as_int64() != 2 || !text || !text->is_string() || text->as_string() != "outgoing")
            {
                co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
            }

            boost::json::object result;
            result.emplace("message", 20);
            result.emplace("timestamp", 1700000120000);
            result.emplace("realtime", true);
            result.emplace("mentions", boost::json::array{});
            response.emplace("result", std::move(result));
            auto [response_ec] = co_await send_text(connection, std::move(response));
            if (response_ec)
            {
                co_return boost::capy::io_result<bool>{response_ec, false};
            }

            boost::json::object notification_params;
            notification_params.emplace("conversation", 2);
            notification_params.emplace("username", "bob");
            notification_params.emplace("avatar_revision", 0);
                notification_params.emplace("mentions", boost::json::array{});
                notification_params.emplace("reaction_revision", 0);
                notification_params.emplace("reactions", boost::json::array{});
            notification_params.emplace("has_avatar", false);
            notification_params.emplace("id", 21);
            notification_params.emplace("from", 2);
            notification_params.emplace("timestamp", 1700000180000);
            notification_params.emplace("text", "incoming @alice");
            notification_params["mentions"] = boost::json::array{boost::json::object{{"user", 1}, {"username", "alice"}}};
            boost::json::object notification;
            notification.emplace("jsonrpc", "2.0");
            notification.emplace("method", "message");
            notification.emplace("params", std::move(notification_params));
            auto [notification_ec] = co_await send_text(connection, std::move(notification));
            co_return boost::capy::io_result<bool>{notification_ec, !notification_ec};
        }

        if (method->as_string() == "mark_read")
        {
            auto const* user = params->as_object().if_contains("conversation");
            auto const* message = params->as_object().if_contains("message");
            if (!user || !user->is_int64() || user->as_int64() != 2 || !message || !message->is_int64() || message->as_int64() != 21)
            {
                co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
            }

            boost::json::object result;
            result.emplace("message", 21);
            response.emplace("result", std::move(result));
            auto [send_ec] = co_await send_text(connection, std::move(response));
            if (send_ec)
            {
                co_return boost::capy::io_result<bool>{send_ec, false};
            }

            boost::json::object notification_params;
            notification_params.emplace("user", 2);
            notification_params.emplace("conversation", 2);
            notification_params.emplace("message", 20);
            boost::json::object notification;
            notification.emplace("jsonrpc", "2.0");
            notification.emplace("method", "read");
            notification.emplace("params", std::move(notification_params));
            auto [notification_ec] = co_await send_text(connection, std::move(notification));
            co_return boost::capy::io_result<bool>{notification_ec, !notification_ec};
        }

        if (method->as_string() == "register")
        {
            auto const* username = params->as_object().if_contains("username");
            auto const* password = params->as_object().if_contains("password");
            if (!username || !username->is_string() || username->as_string() != "new_user" || !password ||
                !password->is_string() || password->as_string() != "new_secret")
            {
                co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
            }

            boost::json::object result;
            result.emplace("user", 4);
            response.emplace("result", std::move(result));
            auto [send_ec] = co_await send_text(connection, std::move(response));
            co_return boost::capy::io_result<bool>{send_ec, !send_ec};
        }

        if (method->as_string() != "authenticate")
        {
            co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
        }

        auto const* username = params->as_object().if_contains("username");
        auto const* password = params->as_object().if_contains("password");
        if (!username || !username->is_string() || !password || !password->is_string())
        {
            co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
        }

        auto const name = std::string_view(username->as_string().data(), username->as_string().size());
        auto const secret = std::string_view(password->as_string().data(), password->as_string().size());
        if (name == "close")
        {
            socket_.close();
            co_return boost::capy::io_result<bool>{std::error_code{}, false};
        }

        if (name == "protocol")
        {
            auto [invalid_ec] = co_await connection.send_text("invalid");
            if (invalid_ec)
            {
                co_return boost::capy::io_result<bool>{invalid_ec, false};
            }
        }

        if (name == "rpc")
        {
            boost::json::object error;
            error.emplace("code", -32003);
            error.emplace("message", "Already authenticated");
            response.emplace("error", std::move(error));
        }
        else
        {
            boost::json::object result;
            result.emplace("authenticated", name == "alice" && secret == "secret");
            result.emplace("avatar_revision", 0);
            result.emplace("has_avatar", false);
            result.emplace("user", name == "alice" && secret == "secret" ? 1 : 0);
            response.emplace("result", std::move(result));
        }

        auto [send_ec] = co_await send_text(connection, std::move(response));
        co_return boost::capy::io_result<bool>{send_ec, !send_ec};
    }

    boost::capy::task<> run_session()
    {
        parser_.reset();
        parser_.start();

        auto [read_ec] = co_await parser_.read_header(socket_);
        if (read_ec || !parser_.is_complete() || parser_.has_buffered_data())
        {
            socket_.close();
            co_return;
        }

        std::string accept;
        if (as_string_view(parser_.get().target()) != "/ws" || !websocket_upgrade_accept(parser_.get(), accept))
        {
            socket_.close();
            co_return;
        }

        auto [write_ec] = co_await send_upgrade_response(accept);
        if (write_ec)
        {
            socket_.close();
            co_return;
        }

        websocket_connection connection(socket_);
        for (;;)
        {
            auto receive_result = co_await connection.receive();
            auto& [ec, message] = receive_result;
            if (ec || message.message_type == websocket_message::type::close)
            {
                break;
            }

            auto [handle_ec, keep_open] = co_await handle_request(connection, message.payload);
            if (handle_ec || !keep_open)
            {
                break;
            }
        }

        socket_.close();
    }

    std::string avatar_data_;
    boost::corosio::io_context& io_context_;
    boost::corosio::tcp_socket socket_;
    boost::http::request_parser parser_;
    boost::http::response response_;
    boost::http::serializer serializer_;
};

std::vector<std::unique_ptr<boost::corosio::tcp_server::worker_base>> make_workers(
    boost::corosio::io_context& io_context,
    boost::http::shared_parser_config const& parser_config,
    boost::http::shared_serializer_config const& serializer_config)
{
    std::vector<std::unique_ptr<boost::corosio::tcp_server::worker_base>> workers;
    workers.push_back(std::make_unique<client_test_worker>(io_context, parser_config, serializer_config));
    return workers;
}

struct test_state
{
    template<class Predicate>
    bool wait(Predicate predicate)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, 5s, predicate);
    }

    std::mutex mutex;
    std::condition_variable condition;
    int connected = 0;
    int disconnected = 0;
    std::vector<chat::error> errors;
    std::vector<chat::message> messages;
    std::vector<chat::reaction_update> reactions;
    std::vector<chat::group_join_request_event> join_requests;
    std::vector<std::pair<std::int64_t, std::int64_t>> reads;
    std::vector<chat::presence> presences;
    std::vector<std::pair<std::int64_t, chat::avatar_state>> avatars;
};

boost::capy::task<> stop_server(boost::corosio::tcp_server& server)
{
    server.stop();
    co_return;
}

struct server_guard
{
    boost::corosio::io_context& io_context;
    boost::corosio::tcp_server& server;
    std::thread& thread;

    ~server_guard()
    {
        boost::capy::run_async(io_context.get_executor())(stop_server(server));
        thread.join();
        server.join();
    }
};

}    // namespace

int main()
{
    boost::corosio::io_context server_io_context;
    auto parser_config = boost::http::make_parser_config(boost::http::parser_config{true});
    auto serializer_config = boost::http::make_serializer_config(boost::http::serializer_config{});

    boost::corosio::tcp_server server(server_io_context, server_io_context.get_executor());
    server.set_workers(make_workers(server_io_context, parser_config, serializer_config));
    if (auto ec = server.bind(boost::corosio::endpoint(boost::corosio::ipv4_address::loopback(), 0)))
    {
        std::cerr << "FAIL client test server bind: " << ec.message() << '\n';
        return 1;
    }
    auto const port = server.local_endpoint().port();
    server.start();

    std::thread server_thread([&server_io_context] { server_io_context.run(); });
    server_guard guard{server_io_context, server, server_thread};

    test_state state;
    chat::client client;
    client.set_connected_handler([&state] {
        std::lock_guard lock(state.mutex);
        ++state.connected;
        state.condition.notify_all();
    });
    client.set_disconnected_handler([&state] {
        std::lock_guard lock(state.mutex);
        ++state.disconnected;
        state.condition.notify_all();
    });
    client.set_error_handler([&state](chat::error const& error) {
        std::lock_guard lock(state.mutex);
        state.errors.push_back(error);
        state.condition.notify_all();
    });
    client.set_message_handler([&state](chat::message message) {
        std::lock_guard lock(state.mutex);
        state.messages.push_back(std::move(message));
        state.condition.notify_all();
    });
    client.set_reaction_handler([&state](chat::reaction_update update) {
        std::lock_guard lock(state.mutex);
        state.reactions.push_back(std::move(update));
        state.condition.notify_all();
    });
    client.set_group_join_request_handler([&state](chat::group_join_request_event value) {
        std::lock_guard lock(state.mutex);
        state.join_requests.push_back(value);
        state.condition.notify_all();
    });
    client.set_read_handler(
        [&state](std::int64_t conversation, std::int64_t user, std::int64_t message)
        {
        std::lock_guard lock(state.mutex);
            if (conversation == 2)
            {
        state.reads.emplace_back(user, message);
            }
        state.condition.notify_all();
    });
    client.set_presence_handler([&state](chat::presence value) {
        std::lock_guard lock(state.mutex);
        state.presences.push_back(std::move(value));
        state.condition.notify_all();
    });

    client.set_avatar_handler([&](std::int64_t user, chat::avatar_state avatar) {
        std::lock_guard lock(state.mutex);
        state.avatars.emplace_back(user, avatar);
        state.condition.notify_all();
    });

    auto url = std::string("ws://127.0.0.1:") + std::to_string(port) + "/ws";
    client.connect(url);
    if (!state.wait([&state] { return state.connected == 1; }))
    {
        std::cerr << "FAIL client connection\n";
        return 1;
    }
    std::cout << "PASS client connection\n";

    bool registered_called = false;
    std::int64_t registered_user = 0;
    client.register_user("new_user", "new_secret", [&](std::expected<std::int64_t, chat::error> result) {
        std::lock_guard lock(state.mutex);
        registered_called = true;
        if (result)
        {
            registered_user = *result;
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return registered_called; }) || registered_user != 4)
    {
        std::cerr << "FAIL client register response\n";
        return 1;
    }
    std::cout << "PASS client register response\n";

    bool authenticated_called = false;
    bool authenticated = false;
    client.authenticate("alice", "secret",
                        [&](std::expected<chat::authentication_result, chat::error> result)
                        {
        std::lock_guard lock(state.mutex);
        authenticated_called = true;
                            authenticated = result && result->authenticated;
        state.condition.notify_all();
    });
    if (!state.wait([&] { return authenticated_called; }) || !authenticated)
    {
        std::cerr << "FAIL client authenticate response\n";
        return 1;
    }
    std::cout << "PASS client authenticate response\n";

    bool presence_called = false;
    std::vector<chat::presence> presence;
    client.get_presence([&](std::expected<std::vector<chat::presence>, chat::error> result) {
        std::lock_guard lock(state.mutex);
        presence_called = true;
        if (result)
        {
            presence = std::move(*result);
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return presence_called && !state.presences.empty(); }) || presence.size() != 1 ||
        presence[0].user != 2 || !presence[0].online || presence[0].last_seen != 1699999999000LL ||
        state.presences.back().user != 3 || state.presences.back().online ||
        state.presences.back().last_seen != 1700000001000LL)
    {
        std::cerr << "FAIL client presence\n";
        return 1;
    }
    std::cout << "PASS client presence\n";

    bool contacts_called = false;
    std::vector<chat::user> contacts;
    client.get_contacts([&](std::expected<std::vector<chat::user>, chat::error> result) {
        std::lock_guard lock(state.mutex);
        contacts_called = true;
        if (result)
        {
            contacts = std::move(*result);
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return contacts_called; }) || contacts.size() != 1 || contacts[0].id != 3 || contacts[0].username != "carol")
    {
        std::cerr << "FAIL client contacts\n";
        return 1;
    }
    std::cout << "PASS client contacts\n";

    bool users_called = false;
    std::vector<chat::user> users;
    client.search_users("bo", [&](std::expected<std::vector<chat::user>, chat::error> result) {
        std::lock_guard lock(state.mutex);
        users_called = true;
        if (result)
        {
            users = std::move(*result);
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return users_called; }) || users.size() != 1 || users[0].id != 2 || users[0].username != "bob")
    {
        std::cerr << "FAIL client user search\n";
        return 1;
    }
    std::cout << "PASS client user search\n";

    bool contact_added_called = false;
    chat::user added_contact;
    client.add_contact(2, [&](std::expected<chat::user, chat::error> result) {
        std::lock_guard lock(state.mutex);
        contact_added_called = true;
        if (result)
        {
            added_contact = std::move(*result);
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return contact_added_called; }) || added_contact.id != 2 || added_contact.username != "bob")
    {
        std::cerr << "FAIL client add contact\n";
        return 1;
    }
    std::cout << "PASS client add contact\n";

    bool conversations_called = false;
    std::vector<chat::conversation> conversations;
    client.get_conversations({},
                             [&](std::expected<chat::conversations_result, chat::error> result)
                             {
        std::lock_guard lock(state.mutex);
        conversations_called = true;
        if (result)
        {
                                     conversations = std::move(result->conversations);
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return conversations_called; }) || conversations.size() != 1 || conversations[0].user != 2 ||
        conversations[0].username != "bob" || conversations[0].last.id != 12 || conversations[0].last.from != 2 ||
        conversations[0].last.timestamp != 1700000000000LL || conversations[0].last.text != "hello" ||
        conversations[0].unread != 3 || !conversations[0].muted || !conversations[0].pinned || !conversations[0].can_send)
    {
        std::cerr << "FAIL client conversations\n";
        return 1;
    }
    std::cout << "PASS client conversations\n";

    bool conversation_cursor_called = false;
    std::vector<chat::conversation> older_conversations;
    client.get_conversations(chat::conversation_cursor{1700000000000LL, 2, true},
                             [&](std::expected<chat::conversations_result, chat::error> result)
                             {
        std::lock_guard lock(state.mutex);
        conversation_cursor_called = true;
        if (result)
        {
                                     older_conversations = std::move(result->conversations);
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return conversation_cursor_called; }) || older_conversations.size() != 1 ||
        older_conversations[0].user != 3 || older_conversations[0].last.id != 6 || older_conversations[0].muted ||
        older_conversations[0].pinned || older_conversations[0].can_send)
    {
        std::cerr << "FAIL client conversation cursor\n";
        return 1;
    }
    std::cout << "PASS client conversation cursor\n";

    for (auto user : {2, 96, 97, 98, 99})
    {
        auto promise = std::make_shared<std::promise<std::expected<chat::direct_conversation_result, chat::error>>>();
        auto future = promise->get_future();
        client.open_direct_conversation(user, [promise](auto result) { promise->set_value(std::move(result)); });
        if (future.wait_for(5s) != std::future_status::ready) { return 1; }
        auto result = future.get();
        if (user == 2 ? (!result || result->conversation != 2 || !result->can_send)
                      : (result.has_value() || result.error().kind != chat::error_kind::protocol)) { return 1; }
    }
    for (auto probe : {104, 105, 106, 107})
    {
        auto promise = std::make_shared<std::promise<std::expected<chat::conversations_result, chat::error>>>();
        auto future = promise->get_future();
        client.get_conversations(chat::conversation_cursor{1700000000000LL, probe, false},
            [promise](auto result) { promise->set_value(std::move(result)); });
        if (future.wait_for(5s) != std::future_status::ready) { return 1; }
        auto result = future.get();
        if (probe == 106 ? (!result || result->conversations.front().can_send)
                         : (result.has_value() || result.error().kind != chat::error_kind::protocol)) { return 1; }
    }
    std::cout << "PASS authoritative direct capability and strict snapshot validation\n";

    bool messages_called = false;
    chat::messages_result messages_result;
    client.get_messages(2, {}, [&](std::expected<chat::messages_result, chat::error> result) {
        std::lock_guard lock(state.mutex);
        messages_called = true;
        if (result)
        {
            messages_result = std::move(*result);
        }
        state.condition.notify_all();
    });
    auto const& messages = messages_result.messages;
    if (!state.wait([&] { return messages_called; }) ||
        (messages_result.read_positions.size() != 1 || messages_result.read_positions[0].message != 12) ||
        messages.size() != 2 || messages[0].id != 10 || messages[0].from != 2 ||
        messages[0].timestamp != 1700000000000 || messages[0].text != "first @alice" ||
        messages[0].mentions.size() != 1 || messages[0].mentions.front().user != 1 ||
        messages[0].mentions.front().username != "alice" ||
        messages[0].reaction_revision != 1 || messages[0].reactions.size() != 1 ||
        messages[0].reactions[0].emoji != "👍" || messages[0].reactions[0].users != std::vector<std::int64_t>{1, 2} || messages[1].id != 12 ||
        messages[1].from != 1 || messages[1].timestamp != 1700000060000 || messages[1].text != "second")
    {
        std::cerr << "FAIL client messages\n";
        return 1;
    }
    std::cout << "PASS client messages\n";

    bool older_messages_called = false;
    chat::messages_result older_messages_result;
    client.get_messages(2, 10, [&](std::expected<chat::messages_result, chat::error> result) {
        std::lock_guard lock(state.mutex);
        older_messages_called = true;
        if (result)
        {
            older_messages_result = std::move(*result);
        }
        state.condition.notify_all();
    });
    auto const& older_messages = older_messages_result.messages;
    if (!state.wait([&] { return older_messages_called; }) ||
        (older_messages_result.read_positions.size() != 1 || older_messages_result.read_positions[0].message != 12) ||
        older_messages.size() != 1 || older_messages[0].id != 4 || older_messages[0].from != 2 ||
        older_messages[0].timestamp != 1699999940000 || older_messages[0].text != "older")
    {
        std::cerr << "FAIL client message cursor\n";
        return 1;
    }
    std::cout << "PASS client message cursor\n";

    auto avatar_call = []<class T>(auto operation) -> std::expected<T, chat::error> {
        std::promise<std::expected<T, chat::error>> promise;
        auto future = promise.get_future();
        operation([&](auto result) { promise.set_value(std::move(result)); });
        if (future.wait_for(5s) != std::future_status::ready) { std::abort(); }
        return future.get();
    };
    for (auto probe : {90, 91, 92, 93, 94, 95})
    {
        auto result = avatar_call.operator()<chat::messages_result>([&](auto h) { client.get_messages(2, probe, h); });
        if (result || result.error().kind != chat::error_kind::protocol) { return 1; }
    }
    std::cout << "PASS client persistent mention metadata and malformed payload validation\n";
    for (auto muted : {true, false})
    {
        auto result = avatar_call.operator()<bool>([&](auto h) { client.set_conversation_muted(2, muted, h); });
        if (!result || *result != muted) { std::cerr << "FAIL client mute/unmute\n"; return 1; }
    }
    for (auto id : {97, 98, 99})
    {
        auto result = avatar_call.operator()<bool>([&](auto h) { client.set_conversation_muted(id, true, h); });
        if (result || result.error().kind != chat::error_kind::protocol) { return 1; }
    }
    for (auto id : {98, 99})
    {
        auto result = avatar_call.operator()<chat::conversations_result>([&](auto h) {
            client.get_conversations(chat::conversation_cursor{1, id}, h);
        });
        if (result || result.error().kind != chat::error_kind::protocol) { return 1; }
    }
    std::cout << "PASS client mute metadata, explicit setting and malformed protocol validation\n";
    for (auto pinned : {true, false})
    {
        auto result = avatar_call.operator()<bool>([&](auto h) { client.set_conversation_pinned(2, pinned, h); });
        if (!result || *result != pinned) { std::cerr << "FAIL client pin/unpin\n"; return 1; }
    }
    for (auto id : {97, 98, 99})
    {
        auto result = avatar_call.operator()<bool>([&](auto h) { client.set_conversation_pinned(id, true, h); });
        if (result || result.error().kind != chat::error_kind::protocol) { return 1; }
    }
    for (auto id : {94, 95, 96, 97})
    {
        auto result = avatar_call.operator()<chat::conversations_result>([&](auto h) {
            client.get_conversations(chat::conversation_cursor{1, id}, h);
        });
        if (result || result.error().kind != chat::error_kind::protocol) { return 1; }
    }
    auto pinned_page = avatar_call.operator()<chat::conversations_result>([&](auto h) { client.get_conversations({}, h); });
    if (!pinned_page || !pinned_page->next || !pinned_page->next->pinned || pinned_page->next->id != 2 ||
        pinned_page->next->activity != 1700000000000LL) { return 1; }
    std::cout << "PASS client pin metadata, setting and three-field cursor validation\n";
    auto group_pin = avatar_call.operator()<chat::conversations_result>([&](auto h) {
        client.get_conversations(chat::conversation_cursor{1, 80, false}, h);
    });
    if (!group_pin || group_pin->conversations.size() != 1 || !group_pin->conversations.front().pinned_message ||
        group_pin->conversations.front().pinned_message->id != 10 || group_pin->conversations.front().pinned_message->text != "pinned") { return 1; }
    for (auto id : {81, 82, 83, 84, 85})
    {
        auto invalid = avatar_call.operator()<chat::conversations_result>([&](auto h) {
            client.get_conversations(chat::conversation_cursor{1, id, false}, h);
        });
        if (invalid || invalid.error().kind != chat::error_kind::protocol) { return 1; }
    }
    for (auto clear : {false, true})
    {
        auto changed = avatar_call.operator()<bool>([&](auto h) {
            if (clear) { client.unpin_group_message(2, h); } else { client.pin_group_message(2, 10, h); }
        });
        if (!changed || !*changed) { return 1; }
        for (auto id : {98, 99})
        {
            auto invalid = avatar_call.operator()<bool>([&](auto h) {
                if (clear) { client.unpin_group_message(id, h); } else { client.pin_group_message(id, 10, h); }
            });
            if (invalid || invalid.error().kind != chat::error_kind::protocol) { return 1; }
        }
    }
    std::cout << "PASS client group pinned summary, explicit actions and protocol validation\n";
    auto announcement = avatar_call.operator()<chat::conversations_result>([&](auto h) {
        client.get_conversations(chat::conversation_cursor{1, 86, false}, h);
    });
    if (!announcement || announcement->conversations.size() != 1 ||
        announcement->conversations.front().announcement != "公告\n<plain>" || !conversations.front().announcement.empty()) { return 1; }
    for (auto id : {87, 88, 89, 90, 91, 92})
    {
        auto invalid = avatar_call.operator()<chat::conversations_result>([&](auto h) {
            client.get_conversations(chat::conversation_cursor{1, id, false}, h);
        });
        if (invalid || invalid.error().kind != chat::error_kind::protocol) { return 1; }
    }
    for (std::string text : {"公告\n<plain>", ""})
    {
        auto changed = avatar_call.operator()<bool>([&](auto h) { client.set_group_announcement(2, text, h); });
        if (!changed || *changed != !text.empty()) { return 1; }
        for (auto id : {98, 99})
        {
            auto invalid = avatar_call.operator()<bool>([&](auto h) { client.set_group_announcement(id, text, h); });
            if (invalid || invalid.error().kind != chat::error_kind::protocol) { return 1; }
        }
    }
    std::cout << "PASS client group announcement metadata, set/clear and malformed protocol\n";
    auto invite_call = [&](int operation, int id) {
        return avatar_call.operator()<std::optional<std::string>>([&](auto h) {
            if (operation == 0) { client.get_group_invite(id, h); }
            else if (operation == 1) { client.create_group_invite(id, h); }
            else { client.revoke_group_invite(id, h); }
        });
    };
    if (!invite_call(0, 1) || *invite_call(0, 1)) { return 1; }
    for (int operation = 0; operation < 3; ++operation)
    {
        auto token = invite_call(operation, 2);
        if (!token || (operation == 2 ? token->has_value() : *token != std::optional<std::string>(std::string(64, 'a')))) { return 1; }
        for (int id : {98, 99, 97, 96})
        {
            auto invalid = invite_call(operation, id);
            if (invalid || invalid.error().kind != chat::error_kind::protocol) { return 1; }
        }
        if (operation != 0)
        {
            auto invalid = invite_call(operation, 95);
            if (invalid || invalid.error().kind != chat::error_kind::protocol) { return 1; }
        }
    }
    for (char token : {'0', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j'})
    {
        auto joined = avatar_call.operator()<chat::group_join_result>([&](auto h) { client.join_group(std::string(64, token), h); });
        if (token <= 'b')
        {
            if (!joined || joined->conversation != 2 || joined->title != "linked group" || joined->member_count != 3 ||
                joined->state != (token == '0' ? chat::group_join_state::pending : token == 'a' ? chat::group_join_state::joined : chat::group_join_state::member)) { return 1; }
        }
        else if (joined || joined.error().kind != chat::error_kind::protocol) { return 1; }
    }
    std::cout << "PASS client invite view/create/revoke, join states and strict protocol validation\n";
    auto request_list = avatar_call.operator()<chat::group_join_requests_result>([&](auto h) { client.get_group_join_requests(2, {}, h); });
    if (!request_list || request_list->requests.size() != 1 || request_list->next || request_list->requests.front().applicant.id != 3 ||
        request_list->requests.front().created_at != 1000 || !state.wait([&] { return state.join_requests.size() == 3; })) { return 1; }
    for (int i = 0; i < 3; ++i)
    {
        if (state.join_requests[i].conversation != 2 || state.join_requests[i].user != 3 ||
            state.join_requests[i].state != static_cast<chat::group_join_request_state>(i)) { return 1; }
    }
    for (auto id : {99, 98, 97, 96, 95, 94, 93, 92})
    {
        auto invalid = avatar_call.operator()<chat::group_join_requests_result>([&](auto h) { client.get_group_join_requests(id, {}, h); });
        if (invalid || invalid.error().kind != chat::error_kind::protocol) { return 1; }
    }
    for (auto choice : {false, true})
    {
        for (auto id : {2, 98, 99})
        {
            auto configured = avatar_call.operator()<bool>([&](auto h) { client.set_group_join_approval(id, choice, h); });
            auto decided = avatar_call.operator()<bool>([&](auto h) { client.respond_group_join_request(id, 3, choice, h); });
            if (id == 2 ? !configured || !*configured || !decided || !*decided :
                configured || configured.error().kind != chat::error_kind::protocol || decided || decided.error().kind != chat::error_kind::protocol) { return 1; }
        }
    }
    for (auto id : {100, 101, 102, 103})
    {
        auto metadata = avatar_call.operator()<chat::conversations_result>([&](auto h) { client.get_conversations(chat::conversation_cursor{1, id, false}, h); });
        if (id == 100 ? !metadata || !metadata->conversations.front().join_approval : metadata || metadata.error().kind != chat::error_kind::protocol) { return 1; }
    }
    auto avatar_bytes = std::string(40000, 'x');
    auto uploaded = avatar_call.operator()<chat::avatar_state>([&](auto h) { client.set_avatar(avatar_bytes, h); });
    auto downloaded = avatar_call.operator()<chat::avatar>([&](auto h) { client.get_avatar(1, 1, h); });
    if (!uploaded || !uploaded->present || uploaded->revision != 1 || !downloaded || downloaded->data != avatar_bytes ||
        !state.wait([&] { return state.avatars.size() == 1; }) || state.avatars.front().first != 1)
    { std::cerr << "FAIL client avatar upload/download/notification\n"; return 1; }
    for (auto user : {99, 98, 97, 96, 95, 94, 93})
    {
        auto rejected_avatar = avatar_call.operator()<chat::avatar>([&](auto h) { client.get_avatar(user, 1, h); });
        if (rejected_avatar || rejected_avatar.error().kind != chat::error_kind::protocol)
        { std::cerr << "FAIL client malformed avatar response\n"; return 1; }
    }
    if (!state.wait([&] { return state.errors.size() == 1; }) || state.errors.front().kind != chat::error_kind::protocol)
    { std::cerr << "FAIL client malformed avatar notification\n"; return 1; }
    std::size_t join_errors;
    { std::lock_guard lock(state.mutex); join_errors = state.errors.size(); }
    for (auto id : {80, 81, 82, 83, 84})
    {
        auto listed = avatar_call.operator()<chat::group_join_requests_result>([&](auto h) { client.get_group_join_requests(id, {}, h); });
        if (!listed) { return 1; }
    }
    if (!state.wait([&] { return state.errors.size() == join_errors + 5; }) || state.join_requests.size() != 3) { return 1; }
    std::cout << "PASS client join approval, request list, metadata, decisions and malformed notifications\n";
    for (auto const* payload : {"bad-offset", "bad-finish"})
    {
        auto rejected_upload = avatar_call.operator()<chat::avatar_state>([&](auto h) { client.set_avatar(payload, h); });
        if (rejected_upload || rejected_upload.error().kind != chat::error_kind::protocol) { return 1; }
    }
    auto oversized_avatar = avatar_call.operator()<chat::avatar_state>([&](auto h) { client.set_avatar(std::string(chat::max_avatar_size + 1, 'x'), h); });
    if (oversized_avatar || oversized_avatar.error().kind != chat::error_kind::protocol) { return 1; }
    auto cleared_avatar = avatar_call.operator()<chat::avatar_state>([&](auto h) { client.clear_avatar(h); });
    if (!cleared_avatar || cleared_avatar->present || cleared_avatar->revision != 2)
    { std::cerr << "FAIL client invalid avatar upload or clear\n"; return 1; }
    std::cout << "PASS client multi-chunk avatar transport and malformed protocol validation\n";

    auto reaction_result = avatar_call.operator()<chat::reaction_update>([&](auto handler) {
        client.set_message_reaction(2, 10, "👍", handler);
    });
    if (!reaction_result || reaction_result->revision != 2 || reaction_result->reactions.size() != 1 ||
        reaction_result->reactions.front().users != std::vector<std::int64_t>{1} ||
        !state.wait([&] { return state.reactions.size() == 1; })) { return 1; }
    auto cleared_reaction = avatar_call.operator()<chat::reaction_update>([&](auto handler) {
        client.set_message_reaction(2, 10, "", handler);
    });
    if (!cleared_reaction || !cleared_reaction->reactions.empty() ||
        !state.wait([&] { return state.reactions.size() == 2 && state.reactions.back().reactions.empty(); })) { return 1; }
    std::size_t protocol_errors;
    { std::lock_guard lock(state.mutex); protocol_errors = state.errors.size(); }
    auto invalid_reaction = avatar_call.operator()<chat::reaction_update>([&](auto handler) {
        client.set_message_reaction(2, 99, "👍", handler);
    });
    if (invalid_reaction || invalid_reaction.error().kind != chat::error_kind::protocol ||
        !state.wait([&] { return state.errors.size() > protocol_errors; })) { return 1; }
    { std::lock_guard lock(state.mutex); if (state.reactions.size() != 2) { return 1; } }
    std::cout << "PASS client reaction metadata, RPC, clear, notification and duplicate-user rejection\n";

    bool send_called = false;
    chat::send_message_result send_result;
    client.send_message(2, "outgoing", [&](std::expected<chat::send_message_result, chat::error> result) {
        std::lock_guard lock(state.mutex);
        send_called = true;
        if (result)
        {
            send_result = *result;
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return send_called && !state.messages.empty(); }) || send_result.message_id != 20 ||
        send_result.timestamp != 1700000120000 || !send_result.realtime || state.messages.back().id != 21 ||
        state.messages.back().from != 2 || state.messages.back().timestamp != 1700000180000 ||
        state.messages.back().text != "incoming @alice" || state.messages.back().mentions.size() != 1 ||
        state.messages.back().mentions.front().user != 1)
    {
        std::cerr << "FAIL client send and notification\n";
        return 1;
    }
    std::cout << "PASS client send and notification\n";

    bool mark_read_called = false;
    std::int64_t read_message = 0;
    client.mark_read(2, 21, [&](std::expected<std::int64_t, chat::error> result) {
        std::lock_guard lock(state.mutex);
        mark_read_called = true;
        if (result)
        {
            read_message = *result;
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return mark_read_called && !state.reads.empty(); }) || read_message != 21 ||
        state.reads.back().first != 2 || state.reads.back().second != 20)
    {
        std::cerr << "FAIL client mark read and read notification\n";
        return 1;
    }
    std::cout << "PASS client mark read and read notification\n";

    bool rejected_called = false;
    bool rejected = false;
    client.authenticate("alice", "wrong",
                        [&](std::expected<chat::authentication_result, chat::error> result)
                        {
        std::lock_guard lock(state.mutex);
        rejected_called = true;
                            rejected = result && !result->authenticated;
        state.condition.notify_all();
    });
    if (!state.wait([&] { return rejected_called; }) || !rejected)
    {
        std::cerr << "FAIL client authenticate rejection\n";
        return 1;
    }
    std::cout << "PASS client authenticate rejection\n";

    bool rpc_error_called = false;
    chat::error rpc_error;
    client.authenticate("rpc", "secret",
                        [&](std::expected<chat::authentication_result, chat::error> result)
                        {
        std::lock_guard lock(state.mutex);
        if (!result)
        {
            rpc_error_called = true;
            rpc_error = result.error();
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return rpc_error_called; }) || rpc_error.kind != chat::error_kind::rpc || rpc_error.code != -32003 ||
        rpc_error.message != "Already authenticated")
    {
        std::cerr << "FAIL client rpc error\n";
        return 1;
    }
    std::cout << "PASS client rpc error\n";

    bool protocol_response_called = false;
    client.authenticate("protocol", "secret",
                        [&](std::expected<chat::authentication_result, chat::error> result)
                        {
        std::lock_guard lock(state.mutex);
                            protocol_response_called = result && !result->authenticated;
        state.condition.notify_all();
    });
    if (!state.wait([&] { return !state.errors.empty() && protocol_response_called; }) || state.errors.back().kind != chat::error_kind::protocol)
    {
        std::cerr << "FAIL client protocol error\n";
        return 1;
    }
    std::cout << "PASS client protocol error\n";

    client.close();
    if (!state.wait([&state] { return state.disconnected == 1; }))
    {
        std::cerr << "FAIL client close\n";
        return 1;
    }
    std::cout << "PASS client close\n";

    client.connect(url);
    if (!state.wait([&state] { return state.connected == 2; }))
    {
        std::cerr << "FAIL client reconnect\n";
        return 1;
    }
    std::cout << "PASS client reconnect\n";

    bool close_error_called = false;
    client.authenticate("close", "secret",
                        [&](std::expected<chat::authentication_result, chat::error> result)
                        {
        std::lock_guard lock(state.mutex);
        close_error_called = !result && result.error().kind == chat::error_kind::transport;
        state.condition.notify_all();
    });
    if (!state.wait([&] { return close_error_called && state.disconnected == 2; }))
    {
        std::cerr << "FAIL client pending request close\n";
        return 1;
    }
    std::cout << "PASS client pending request close\n";
    std::atomic_int destructor_callbacks = 0;
    {
        chat::client shutting_down;
        std::promise<void> ready;
        shutting_down.set_connected_handler([&] { ready.set_value(); });
        shutting_down.connect(url);
        if (ready.get_future().wait_for(5s) != std::future_status::ready) { return 1; }
        shutting_down.get_avatar(92, 1, [&](auto) { ++destructor_callbacks; });
        auto barrier = avatar_call.operator()<chat::authentication_result>([&](auto h) { shutting_down.authenticate("alice", "secret", h); });
        if (!barrier) { return 1; }
    }
    if (destructor_callbacks != 0) { std::cerr << "FAIL pending avatar callback during destruction\n"; return 1; }
    std::cout << "PASS client destruction suppresses pending avatar callback\n";
    for (int callback = 0; callback < 3; ++callback)
    {
        auto released = std::make_shared<std::promise<void>>();
        auto release_future = released->get_future();
        auto lifetime = std::shared_ptr<int>(new int(0), [released](int* value) {
            delete value;
            released->set_value();
        });
        auto owned = std::make_unique<chat::client>();
        owned->set_error_handler([lifetime](auto const&) {});
        lifetime.reset();
        auto destroy = [&owned] { owned.reset(); };
        auto ready = std::make_shared<std::promise<void>>();
        auto ready_future = ready->get_future();
        if (callback == 0) { owned->set_connected_handler(destroy); }
        else { owned->set_connected_handler([ready] { ready->set_value(); }); }
        if (callback == 2) { owned->set_message_handler([destroy](auto) { destroy(); }); }
        owned->connect(url);
        if (callback != 0)
        {
            if (ready_future.wait_for(5s) != std::future_status::ready) { return 1; }
            if (callback == 1) { owned->authenticate("alice", "secret", [destroy](auto) { destroy(); }); }
            else { owned->send_message(2, "outgoing", [](auto) {}); }
        }
        if (release_future.wait_for(5s) != std::future_status::ready)
        { std::cerr << "FAIL client destroyed from callback cannot finish shutdown\n"; return 1; }
    }
    std::cout << "PASS client destruction from connected, request and message callbacks releases network ownership\n";
    return 0;
}
