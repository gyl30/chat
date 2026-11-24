#ifndef CHAT_CLIENT_SRC_RPC_PAYLOAD_HPP
#define CHAT_CLIENT_SRC_RPC_PAYLOAD_HPP

#include <cstdint>
#include <expected>
#include <meta>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <simdjson.h>

#include <chat/error.hpp>
#include <chat/detail/websocket_limits.hpp>

namespace chat::detail
{
namespace dom = simdjson::dom;

using builder = simdjson::builder::string_builder;

// Append only the fields present in this method's parameters. The builder owns
// the bytes; a caller's strings never need to survive until send_request.
class request_params
{
   public:
    template <class T>
    void add(std::string_view key, T const& value)
    {
        if (fields_.size() != 0)
        {
            fields_.append_comma();
        }
        fields_.escape_and_append_with_quotes(key);
        fields_.append_colon();
        if constexpr (std::is_convertible_v<T const&, std::string_view>)
        {
            fields_.escape_and_append_with_quotes(std::string_view(value));
        }
        else
        {
            simdjson::builder::append(fields_, value);
        }
    }

    auto view() const { return fields_.view(); }

   private:
    builder fields_;
};

inline void add_params(request_params&) {}

template <class T, class... Tail>
void add_params(request_params& params, std::string_view key, T const& value, Tail const&... tail)
{
    params.add(key, value);
    add_params(params, tail...);
}

template <class... Args>
request_params make_params(Args const&... args)
{
    request_params result;
    add_params(result, args...);
    return result;
}

inline std::expected<std::string, error> encode_request(std::string_view method, request_params const& params, std::int64_t id)
{
    std::string_view fields;
    if (params.view().get(fields))
    {
        return std::unexpected(error{error_kind::protocol, 0, "Could not encode request"});
    }
    builder json;
    json.start_object();
    json.append_key_value("jsonrpc", std::string_view("2.0"));
    json.append_comma();
    json.append_key_value("method", method);
    json.append_comma();
    json.escape_and_append_with_quotes("params");
    json.append_colon();
    json.start_object();
    json.append_raw(fields);
    json.end_object();
    json.append_comma();
    json.append_key_value("id", id);
    json.end_object();
    std::string_view payload;
    if (json.view().get(payload))
    {
        return std::unexpected(error{error_kind::protocol, 0, "Could not encode request"});
    }
    if (payload.size() > max_websocket_request_size)
    {
        return std::unexpected(error{error_kind::protocol, 0, "Request exceeds 64 KiB"});
    }
    return std::string(payload);
}

template <class Object>
std::optional<dom::element> find_field(Object object, std::string_view key)
{
    dom::element value;
    return object.at_key(key).get(value) ? std::nullopt : std::optional(value);
}

inline std::optional<dom::object> object_of(dom::element value)
{
    dom::object object;
    return value.get(object) ? std::nullopt : std::optional(object);
}

template <class T>
bool read_payload(dom::element value, T& result);

template <class T>
bool read_member(std::optional<dom::element> value, T& result)
{
    return value && read_payload(*value, result);
}

template <class T>
bool read_member(std::optional<dom::element> value, std::optional<T>& result)
{
    if (!value || value->is_null())
    {
        result.reset();
        return true;
    }
    return read_payload(*value, result.emplace());
}

// These plain payloads use the same public member and wire names. DOM is used
// here and for notifications so irregular payloads can reuse nested fields
// without reparsing the frame or relying on field order.
template <class T>
bool read_payload(dom::element value, T& result)
{
    if constexpr (std::is_same_v<T, std::string>)
    {
        std::string_view string;
        if (value.get(string))
        {
            return false;
        }
        result.assign(string);
        return true;
    }
    else if constexpr (std::is_arithmetic_v<T>)
    {
        return !value.get(result);
    }
    else if constexpr (std::ranges::range<T>)
    {
        dom::array array;
        if (value.get(array))
        {
            return false;
        }
        result.reserve(array.size());
        for (auto entry : array)
        {
            if (!read_payload(entry, result.emplace_back()))
            {
                return false;
            }
        }
        return true;
    }
    else
    {
        dom::object object;
        if (value.get(object))
        {
            return false;
        }
        template for (constexpr auto member :
                      std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current())))
        {
            if (!read_member(find_field(object, std::meta::identifier_of(member)), result.[:member:]))
            {
                return false;
            }
        }
        return true;
    }
}

template <class T>
std::optional<T> read_payload(dom::element value)
{
    T result{};
    return read_payload(value, result) ? std::optional(std::move(result)) : std::nullopt;
}

}    // namespace chat::detail
#endif
