#include <cassert>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <future>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

#include <chat/client.hpp>

#include "emoji_segments.hpp"

using namespace std::chrono_literals;

namespace
{
template <class T>
T wait(std::future<T> result)
{
    assert(result.wait_for(12s) == std::future_status::ready);
    return result.get();
}

template <class T, class Invoke>
auto request(Invoke invoke)
{
    std::promise<std::expected<T, chat::error>> result;
    invoke([&](auto value) { result.set_value(std::move(value)); });
    return wait(result.get_future());
}

constexpr std::string_view sample = "中\"反\\\n👩🏽‍💻éÄΣ";
constexpr std::string_view escaped = "中\\\"反\\\\\\n👩🏽‍💻éÄΣ";

std::string message_text(std::size_t total, std::string const& envelope)
{
    return std::string(total - envelope.size() - escaped.size(), 'x') + std::string(sample);
}

void unicode()
{
    // Qt stores UTF-16. Each sequence is one user-perceived character.
    auto const text = QString::fromUtf8("中👩🏽‍💻🇨🇳✈️é");
    assert((grapheme_ends(text) == QList<int>{1, 8, 12, 14, 16}));
    assert((emoji_segments(text) == QList<QPair<int, int>>{{1, 7}, {8, 4}, {12, 2}}));
    assert(grapheme_prefix(text, 7) == QStringLiteral("中"));
    assert(grapheme_prefix(text, 13) == QString::fromUtf8("中👩🏽‍💻🇨🇳"));
    assert(grapheme_prefix(text, 15) == QString::fromUtf8("中👩🏽‍💻🇨🇳✈️"));
}
}    // namespace

int main(int argc, char** argv)
{
    assert(argc == 5);
    unicode();
    auto const conversation = std::to_string(std::atoll(argv[3]));
    auto const message = std::to_string(std::atoll(argv[4]));
    auto const conv = std::atoll(argv[3]);
    auto const mid = std::atoll(argv[4]);
    std::promise<void> connected;
    std::promise<chat::message> event;
    chat::client sdk(argv[2]);
    sdk.set_connected_handler([&] { connected.set_value(); });
    sdk.set_message_handler(
        [&](chat::message value)
        {
            if (value.from == 2 && value.text == sample)
            {
                event.set_value(std::move(value));
            }
        });
    sdk.connect(argv[1]);
    wait(connected.get_future());
    auto auth = request<chat::authentication_result>([&](auto cb) { sdk.authenticate("a", "pass", std::move(cb)); });
    assert(auth && auth->authenticated && auth->user == 1);

    // Authentication is request 1. All three edit requests have one-digit IDs.
    auto const edit = "{\"jsonrpc\":\"2.0\",\"method\":\"edit_message\",\"params\":{\"conversation\":" + conversation + ",\"message\":" + message +
                      ",\"text\":\"\"},\"id\":2}";
    for (std::size_t bytes : {65535, 65536, 65537})
    {
        auto text = message_text(bytes, edit);
        auto result = request<chat::message>([&](auto cb) { sdk.edit_message(conv, mid, text, std::move(cb)); });
        if (bytes == 65537)
        {
            assert(!result && result.error().kind == chat::error_kind::protocol);
        }
        else
        {
            // The request reached the server. Its separate broadcast limit can
            // reject an edit whose outbound notification would exceed 64 KiB.
            assert(!result && result.error().kind == chat::error_kind::rpc && result.error().code == -32602);
        }
    }
    auto edited = request<chat::message>([&](auto cb) { sdk.edit_message(conv, mid, std::string(sample), std::move(cb)); });
    assert(edited && edited->text == sample);

    // The Outbox must still fit if replay uses a 19-digit request ID.
    for (std::size_t bytes : {65535, 65536, 65537})
    {
        auto const cid = std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(bytes);
        auto const send = "{\"jsonrpc\":\"2.0\",\"method\":\"send_message\",\"params\":{\"conversation\":" + conversation +
                          ",\"text\":\"\",\"client_id\":" + std::to_string(cid) + "},\"id\":9223372036854775807}";
        auto result = request<void>([&](auto cb) { sdk.send_message(conv, message_text(bytes, send), std::move(cb), {}, cid); });
        if (bytes == 65537)
        {
            assert(!result && result.error().kind == chat::error_kind::protocol);
        }
        else
        {
            assert(result);
        }
    }

    std::promise<std::expected<chat::contact_book, chat::error>> contacts;
    std::promise<std::expected<std::vector<chat::login_session>, chat::error>> sessions;
    std::optional<chat::user_profile> profile;
    sdk.get_contacts([&](auto value) { contacts.set_value(std::move(value)); });
    sdk.get_user_profile(1,
                         [&](auto value)
                         {
                             assert(value);
                             profile = std::move(*value);
                             sdk.get_login_sessions([&](auto list) { sessions.set_value(std::move(list)); });
                         });
    auto user_sessions = wait(sessions.get_future());
    assert(user_sessions && !user_sessions->empty());
    auto note = wait(event.get_future());
    auto book = wait(contacts.get_future());
    assert(book && book->users.size() == 1 && book->users[0].username == "b");
    assert(profile && profile->username == "a" && profile->nickname == "中👩🏽‍💻");
    assert(note.text == sample);

    auto registered = request<std::int64_t>([&](auto cb) { sdk.register_user("sdk_unicode_case", "pass", std::move(cb)); });
    assert(registered && *registered > 2);
    auto invalid = request<chat::user_profile>([&](auto cb) { sdk.get_user_profile(999999, std::move(cb)); });
    assert(!invalid && invalid.error().kind == chat::error_kind::rpc && invalid.error().code < 0);

    auto ended = request<chat::contact_book>([&](auto cb) { sdk.get_contacts(std::move(cb)); });
    assert(!ended && ended.error().kind == chat::error_kind::transport);
    std::cout << "PASS SDK wire limits, owned replies/notification, reentrant request, RPC error, disconnect drain and Qt grapheme boundaries\n";
}
